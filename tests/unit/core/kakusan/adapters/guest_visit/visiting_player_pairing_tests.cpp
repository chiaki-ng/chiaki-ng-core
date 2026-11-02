#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/guest_visit/visiting_player_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <utility>

namespace kakusan_test::visiting {
namespace {

class VisitingPlayerPairing : public VisitingPlayerTest {};
class VisitingPlayerPakeOrder : public VisitingPlayerTest {};

TEST_F(VisitingPlayerPairing, FailsOnTheWrongTranscriptBeforeShowingACode) {
  Rig rig;
  drive(rig, Stage::channel);
  auto wrong = vectors().context.transcript;
  wrong[9] ^= std::byte{1};
  const auto effects = rig.receive(control::Hello{wrong});
  EXPECT_EQ(failure_sent(effects), Error::transcript);
  EXPECT_EQ(count<DisplayCode>(rig.log), 0U);
  expect_cleanup(rig, 0, {});
}

TEST_F(VisitingPlayerPairing, HoldsAnEarlyPakeAUntilBothCodesAreKnown) {
  Rig rig;
  drive(rig, Stage::verifying);
  EXPECT_TRUE(rig.receive(control::PakeA{vectors().pa}).empty());
  EXPECT_TRUE(rig.at(start_us + 100'000'000).empty());
  EXPECT_EQ(
      rig.feed(LocalCode{
                   rig.now,
                   libchiaking::net::kakusan::Code::from_value(7306).value()})
          .size(),
      1U);
  EXPECT_EQ(rig.spoke.next_deadline_us(), start_us + pairing_timeout_us);
  const auto effects =
      rig.feed(PeerCodeEntered{rig.now, vectors().host_code.clone()});
  EXPECT_EQ(message<control::PakeB>(effects).point, vectors().pb);
}

TEST_F(VisitingPlayerPairing, TimesOutWithoutTheHostsCode) {
  Rig rig;
  drive(rig, Stage::coded);
  EXPECT_TRUE(rig.receive(control::PakeA{vectors().pa}).empty());
  EXPECT_TRUE(rig.at(start_us + pairing_timeout_us - 1).empty());
  const auto effects = rig.at(start_us + pairing_timeout_us);
  EXPECT_EQ(failure_sent(effects), Error::timeout);
  EXPECT_EQ(sent<control::PakeB>(rig.log), nullptr);
  expect_cleanup(rig, 0, {});
}

TEST_F(VisitingPlayerPairing, TimesOutWithoutAuthentication) {
  Rig rig;
  drive(rig, Stage::confirm_b);
  const auto effects = rig.at(start_us + pairing_timeout_us);
  EXPECT_EQ(failure_sent(effects), Error::timeout);
  expect_cleanup(rig, 0, {});
}

TEST_F(VisitingPlayerPairing, AnswersOnceItsOwnCodeIsDrawnLast) {
  Rig rig;
  drive(rig, Stage::verifying);
  EXPECT_TRUE(
      rig.feed(PeerCodeEntered{rig.now, vectors().host_code.clone()}).empty());
  EXPECT_FALSE(libchiaking::net::kakusan::Code::from_value(10000));
  EXPECT_TRUE(rig.receive(control::PakeA{vectors().pa}).empty());
  const auto effects = rig.feed(LocalCode{
      rig.now, libchiaking::net::kakusan::Code::from_value(7306).value()});
  EXPECT_EQ(code_shown(effects), vectors().guest_code.view());
  EXPECT_EQ(message<control::PakeB>(effects).point, vectors().pb);
  EXPECT_TRUE(
      rig
          .feed(LocalCode{
              rig.now, libchiaking::net::kakusan::Code::from_value(1).value()})
          .empty());
}

TEST_F(VisitingPlayerPairing, RecordsAGuessWhenTheHostsCodeIsWrong) {
  Rig rig;
  drive(rig, Stage::coded);
  auto effects = rig.feed(PeerCodeEntered{
      rig.now, libchiaking::net::kakusan::Code::parse("4822").value()});
  EXPECT_TRUE(effects.empty());
  effects = rig.receive(control::PakeA{vectors().pa});
  EXPECT_NE(message<control::PakeB>(effects).point, vectors().pb);
  effects = rig.receive(control::ConfirmA{vectors().ca});
  EXPECT_EQ(failure_sent(effects), Error::pairing_failed);
  EXPECT_EQ(sent<control::ConfirmB>(effects), nullptr);
  expect_cleanup(rig, 0, {.guess = true});
  expect_closed_after_grace(rig);
}

TEST_F(VisitingPlayerPairing, RecordsAGuessForACorruptPakeA) {
  Rig rig;
  drive(rig, Stage::entered);
  auto point = vectors().pa;
  point[64] ^= std::byte{1};
  const auto effects = rig.receive(control::PakeA{point});
  EXPECT_EQ(failure_sent(effects), Error::pairing_failed);
  EXPECT_EQ(sent<control::PakeB>(effects), nullptr);
  expect_cleanup(rig, 0, {.guess = true});
}

TEST_F(VisitingPlayerPairing, RecordsAGuessForAWrongConfirmation) {
  Rig rig;
  drive(rig, Stage::pake_b);
  auto mac = vectors().ca;
  mac[0] ^= std::byte{1};
  const auto effects = rig.receive(control::ConfirmA{mac});
  EXPECT_EQ(failure_sent(effects), Error::pairing_failed);
  expect_cleanup(rig, 0, {.guess = true});
}

TEST_F(VisitingPlayerPairing, RefusesAuthenticationOfAnotherTranscript) {
  Rig rig;
  drive(rig, Stage::confirm_b);
  auto wrong = vectors().context.transcript;
  wrong[31] ^= std::byte{1};
  const auto effects = rig.receive(control::Authenticated{wrong});
  EXPECT_EQ(failure_sent(effects), Error::transcript);
  EXPECT_EQ(count_sent<control::Authenticated>(rig.log), 0U);
  expect_cleanup(rig, 0, {});
}

TEST_F(VisitingPlayerPairing, RefusesAuthenticationBeforeTheConfirmation) {
  Rig rig;
  drive(rig, Stage::pake_b);
  const auto effects =
      rig.receive(control::Authenticated{vectors().context.transcript});
  EXPECT_EQ(failure_sent(effects), Error::protocol);
  expect_cleanup(rig, 0, {});
}

TEST_F(VisitingPlayerPairing, RecordsAGuessWhenTheHostReportsAFailedPairing) {
  Rig rig;
  drive(rig, Stage::pake_b);
  static_cast<void>(rig.receive(control::Failure{Error::pairing_failed}));
  expect_cleanup(rig, 0, {.guess = true});
}

TEST_F(VisitingPlayerPairing, LeavesSilentlyWhenCancelledAfterConfirming) {
  Rig rig;
  drive(rig, Stage::confirm_b);
  const auto effects = rig.feed(Cancel{rig.now, control::Reason::user});
  EXPECT_EQ(count<SendControl>(effects), 0U);
  expect_cleanup(rig, 0, {});
}

TEST_F(VisitingPlayerPakeOrder, CountsAMessageOutOfOrderAsAGuess) {
  const auto cases = std::to_array<std::pair<Stage, control::Message>>({
      {Stage::channel, control::PakeA{vectors().pa}},
      {Stage::channel, control::ConfirmA{vectors().ca}},
      {Stage::verifying, control::ConfirmA{vectors().ca}},
      {Stage::verifying, control::PakeB{vectors().pb}},
      {Stage::entered, control::ConfirmB{vectors().cb}},
      {Stage::pake_b, control::PakeA{vectors().pa}},
      {Stage::pake_b, control::PakeB{vectors().pb}},
      {Stage::confirm_b, control::ConfirmA{vectors().ca}},
      {Stage::confirm_b, control::ConfirmB{vectors().cb}},
  });
  for (std::size_t index = 0; index < cases.size(); ++index) {
    SCOPED_TRACE(index);
    const auto& [stage, item] = cases[index];
    Rig rig;
    drive(rig, stage);
    const auto effects =
        rig.receive(test_support::kakusan::Messages::clone(item));
    EXPECT_EQ(failure_sent(effects), Error::pairing_failed);
    expect_cleanup(rig, 0, {.guess = true});
  }
}

TEST_F(VisitingPlayerPakeOrder, CountsARepeatedPakeAAsAGuess) {
  Rig rig;
  drive(rig, Stage::coded);
  EXPECT_TRUE(rig.receive(control::PakeA{vectors().pa}).empty());
  const auto effects = rig.receive(control::PakeA{vectors().pa});
  EXPECT_EQ(failure_sent(effects), Error::pairing_failed);
}

TEST_F(VisitingPlayerPakeOrder, TreatsPairingMessagesAfterAdmissionAsProtocol) {
  for (const auto& item : {control::Message{control::PakeA{vectors().pa}},
                           control::Message{control::ConfirmA{vectors().ca}},
                           control::Message{control::PakeB{vectors().pb}}}) {
    SCOPED_TRACE(item.index());
    Rig rig;
    drive(rig, Stage::admitting);
    const auto effects =
        rig.receive(test_support::kakusan::Messages::clone(item));
    EXPECT_EQ(failure_sent(effects), Error::protocol);
    EXPECT_EQ(count<RecordAuthenticationFailure>(rig.log), 0U);
  }
}

}  // namespace
}  // namespace kakusan_test::visiting
