#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_session/invited_player_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <utility>

namespace kakusan_test::invited {
namespace {

class InvitedPlayerPairing : public InvitedPlayerTest {};
class InvitedPlayerCodes : public InvitedPlayerTest {};
class InvitedPlayerPakeOrder : public InvitedPlayerTest {};
class InvitedPlayerGuesses : public InvitedPlayerTest {};

TEST_F(InvitedPlayerPairing, FailsOnTheWrongTranscriptBeforeShowingACode) {
  Rig rig;
  drive(rig, Stage::channel);
  auto wrong = vectors().context.transcript;
  wrong[0] ^= std::byte{1};
  const auto effects = rig.receive(control::Hello{wrong});
  EXPECT_EQ(failure_sent(effects), Error::transcript);
  EXPECT_EQ(shown(effects).error, Error::transcript);
  expect_cleanup(rig, 0, {});
  EXPECT_EQ(count<DisplayCode>(rig.log), 0U);
}

TEST_F(InvitedPlayerPairing, TimesOutWithoutCodes) {
  Rig rig;
  drive(rig, Stage::verifying);
  EXPECT_EQ(rig.spoke.next_deadline_us(), start_us + pairing_timeout_us);
  EXPECT_TRUE(rig.at(start_us + pairing_timeout_us - 1).empty());
  const auto effects = rig.at(start_us + pairing_timeout_us);
  EXPECT_EQ(failure_sent(effects), Error::timeout);
  expect_cleanup(rig, 0, {});
}

TEST_F(InvitedPlayerPairing, CountsATimeoutAfterConfirmingAsAGuess) {
  Rig rig;
  drive(rig, Stage::confirm_a);
  const auto effects = rig.at(start_us + pairing_timeout_us);
  EXPECT_EQ(failure_sent(effects), Error::timeout);
  expect_cleanup(rig, 0, {.guess = true});
}

TEST_F(InvitedPlayerPairing, WaitsWithoutPingsForTheGuestsAuthentication) {
  Rig rig;
  drive(rig, Stage::authenticated_sent);
  EXPECT_TRUE(rig.at(start_us + 200'000'000).empty());
  EXPECT_EQ(rig.spoke.state(), Progress::verifying);
  EXPECT_EQ(count_sent<control::Ping>(rig.log), 0U);
  const auto effects = rig.at(start_us + pairing_timeout_us);
  EXPECT_EQ(failure_sent(effects), Error::timeout);
  expect_cleanup(rig, 0, {});
}

TEST_F(InvitedPlayerCodes, StartPairingOnlyOnceBothAreKnownAfterIdentity) {
  Rig rig;
  drive(rig, Stage::channel);
  EXPECT_TRUE(
      rig.feed(LocalCode{
                   rig.now,
                   libchiaking::net::kakusan::Code::from_value(4821).value()})
          .empty());
  EXPECT_TRUE(
      rig.feed(PeerCodeEntered{rig.now, vectors().guest_code.clone()}).empty());
  static_cast<void>(rig.receive(control::Hello{vectors().context.transcript}));
  static_cast<void>(rig.receive(vectors().peer_proof));
  EXPECT_FALSE(libchiaking::net::kakusan::Code::from_value(10000));
  EXPECT_TRUE(
      rig.feed(PeerCodeEntered{rig.now, vectors().guest_code.clone()}).empty());
  EXPECT_TRUE(
      rig
          .feed(PeerCodeEntered{
              rig.now, libchiaking::net::kakusan::Code::parse("0000").value()})
          .empty());
  const auto effects = rig.feed(LocalCode{
      rig.now, libchiaking::net::kakusan::Code::from_value(4821).value()});
  EXPECT_EQ(code_shown(effects), vectors().host_code.view());
  EXPECT_EQ(message<control::PakeA>(effects).point, vectors().pa);
  EXPECT_TRUE(
      rig.feed(LocalCode{
                   rig.now,
                   libchiaking::net::kakusan::Code::from_value(1234).value()})
          .empty());
  EXPECT_TRUE(
      rig.feed(PeerCodeEntered{rig.now, vectors().guest_code.clone()}).empty());
}

TEST_F(InvitedPlayerGuesses, RecordsAGuessWhenTheGuestsCodeIsWrong) {
  Rig rig;
  drive(rig, Stage::coded);
  auto effects = rig.feed(PeerCodeEntered{
      rig.now, libchiaking::net::kakusan::Code::parse("7307").value()});
  EXPECT_NE(message<control::PakeA>(effects).point, vectors().pa);
  effects = rig.receive(control::PakeB{vectors().pb});
  EXPECT_NE(message<control::ConfirmA>(effects).mac, vectors().ca);
  effects = rig.receive(control::ConfirmB{vectors().cb});
  EXPECT_EQ(failure_sent(effects), Error::pairing_failed);
  EXPECT_EQ(sent<control::Authenticated>(effects), nullptr);
  expect_cleanup(rig, 0, {.guess = true});
  expect_closed_after_grace(rig);
}

TEST_F(InvitedPlayerGuesses, RecordsAGuessForAWrongConfirmation) {
  Rig rig;
  drive(rig, Stage::confirm_a);
  auto mac = vectors().cb;
  mac[63] ^= std::byte{1};
  const auto effects = rig.receive(control::ConfirmB{mac});
  EXPECT_EQ(failure_sent(effects), Error::pairing_failed);
  expect_cleanup(rig, 0, {.guess = true});
}

TEST_F(InvitedPlayerGuesses, RecordsAGuessForACorruptPakeB) {
  Rig rig;
  drive(rig, Stage::pake_a);
  auto point = vectors().pb;
  point[64] ^= std::byte{1};
  const auto effects = rig.receive(control::PakeB{point});
  EXPECT_EQ(failure_sent(effects), Error::pairing_failed);
  EXPECT_EQ(sent<control::ConfirmA>(effects), nullptr);
  expect_cleanup(rig, 0, {.guess = true});
}

TEST_F(InvitedPlayerPakeOrder, CountsAMessageOutOfOrderAsAGuess) {
  const auto cases = std::to_array<std::pair<Stage, control::Message>>({
      {Stage::channel, control::PakeB{vectors().pb}},
      {Stage::channel, control::PakeA{vectors().pa}},
      {Stage::verifying, control::PakeB{vectors().pb}},
      {Stage::verifying, control::ConfirmB{vectors().cb}},
      {Stage::coded, control::ConfirmA{vectors().ca}},
      {Stage::pake_a, control::PakeA{vectors().pa}},
      {Stage::pake_a, control::ConfirmA{vectors().ca}},
      {Stage::pake_a, control::ConfirmB{vectors().cb}},
      {Stage::confirm_a, control::PakeB{vectors().pb}},
      {Stage::authenticated_sent, control::ConfirmB{vectors().cb}},
      {Stage::authenticated_sent, control::PakeB{vectors().pb}},
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

TEST_F(InvitedPlayerPakeOrder, TreatsPairingMessagesAfterAdmissionAsProtocol) {
  for (const auto& item : {control::Message{control::PakeB{vectors().pb}},
                           control::Message{control::ConfirmA{vectors().ca}},
                           control::Message{control::PakeA{vectors().pa}},
                           control::Message{control::ConfirmB{vectors().cb}}}) {
    SCOPED_TRACE(item.index());
    Rig rig;
    drive(rig, Stage::admitting);
    const auto effects =
        rig.receive(test_support::kakusan::Messages::clone(item));
    EXPECT_EQ(failure_sent(effects), Error::protocol);
    EXPECT_EQ(count<RecordAuthenticationFailure>(rig.log), 0U);
  }
}

TEST_F(InvitedPlayerGuesses, CountsCancellingAfterTheConfirmationAsAGuess) {
  Rig rig;
  drive(rig, Stage::confirm_a);
  const auto effects = rig.feed(Cancel{rig.now, control::Reason::user});
  EXPECT_EQ(count<SendControl>(effects), 0U);
  EXPECT_EQ(rig.spoke.state(), Progress::gone);
  expect_cleanup(rig, 0, {.guess = true});
}

TEST_F(InvitedPlayerGuesses, CountsTheGuestsFailureAfterTheConfirmation) {
  Rig rig;
  drive(rig, Stage::confirm_a);
  const auto effects = rig.receive(control::Failure{Error::pairing_failed});
  EXPECT_EQ(count<SendControl>(effects), 0U);
  EXPECT_TRUE(shown(effects).by_peer);
  EXPECT_EQ(shown(effects).error, Error::pairing_failed);
  expect_cleanup(rig, 0, {.guess = true});
  expect_closed_after_grace(rig);
}

TEST_F(InvitedPlayerGuesses, CountsALostTransportAfterTheConfirmation) {
  Rig rig;
  drive(rig, Stage::confirm_a);
  static_cast<void>(rig.feed(TransportFailed{rig.now}));
  expect_cleanup(rig, 0, {.guess = true});
}

TEST_F(InvitedPlayerGuesses, DoNotCountCancellingBeforeTheConfirmation) {
  Rig rig;
  drive(rig, Stage::pake_a);
  static_cast<void>(rig.feed(Cancel{rig.now, control::Reason::user}));
  expect_cleanup(rig, 0, {});
}

TEST_F(InvitedPlayerGuesses, CountTheGuestsReportOfAFailedPairing) {
  Rig rig;
  drive(rig, Stage::verifying);
  static_cast<void>(rig.receive(control::Failure{Error::pairing_failed}));
  expect_cleanup(rig, 0, {.guess = true});
}

TEST_F(InvitedPlayerGuesses, DoNotCountTheGuestsTimeout) {
  Rig rig;
  drive(rig, Stage::verifying);
  static_cast<void>(rig.receive(control::Failure{Error::timeout}));
  expect_cleanup(rig, 0, {});
}

TEST_F(InvitedPlayerGuesses, DoNotCountAWrongAuthenticatedTranscript) {
  Rig rig;
  drive(rig, Stage::authenticated_sent);
  auto wrong = vectors().context.transcript;
  wrong[5] ^= std::byte{1};
  const auto effects = rig.receive(control::Authenticated{wrong});
  EXPECT_EQ(failure_sent(effects), Error::transcript);
  expect_cleanup(rig, 0, {});
}

}  // namespace
}  // namespace kakusan_test::invited
