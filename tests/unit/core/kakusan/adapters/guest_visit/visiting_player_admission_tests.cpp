#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/guest_visit/visiting_player_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <variant>

namespace kakusan_test::visiting {
namespace {

class VisitingPlayerMessages : public VisitingPlayerTest {};
class VisitingPlayerAdmission : public VisitingPlayerTest {};
class VisitingPlayerReadiness : public VisitingPlayerTest {};
class VisitingPlayerOrdering : public VisitingPlayerTest {};

TEST_F(VisitingPlayerMessages, LeaveOnAMessageTheStateDoesNotExpect) {
  for (const auto stage :
       {Stage::channel, Stage::verifying, Stage::entered, Stage::confirm_b,
        Stage::admitting, Stage::pending, Stage::setup, Stage::source_ready,
        Stage::granted, Stage::ready, Stage::active}) {
    for (const auto& item : all_messages()) {
      if (permitted(stage, item)) continue;
      SCOPED_TRACE(::testing::Message() << "stage " << static_cast<int>(stage)
                                        << " message " << item.index());
      Rig rig;
      drive(rig, stage);
      const auto from = rig.log.size();
      const auto effects =
          rig.receive(test_support::kakusan::Messages::clone(item));
      EXPECT_EQ(failure_sent(effects), Error::protocol);
      EXPECT_EQ(rig.spoke.state(), Progress::leaving);
      EXPECT_EQ(count<SendControl>(effects), 1U);
      EXPECT_EQ(count<RecordAuthenticationFailure>(rig.log), 0U);
      EXPECT_EQ(count<ConfigureReceiver>(rig.log, from), 0U);
      EXPECT_EQ(count<EraseSecrets>(rig.log, from), 1U);
    }
  }
}

TEST_F(VisitingPlayerAdmission, TimesOutAMinuteAfterThePendingNotice) {
  Rig rig;
  drive(rig, Stage::admitting);
  EXPECT_EQ(rig.spoke.next_deadline_us(), rig.now + ping_interval_us);
  rig.now += 1'000'000;
  EXPECT_TRUE(rig.receive(control::AdmissionPending{}).empty());
  const auto pending = rig.now;
  keep_alive(rig, pending + admission_timeout_us);
  const auto from = rig.log.size();
  EXPECT_EQ(count<SendControl>(rig.at(pending + admission_timeout_us - 1)), 0U);
  const auto effects = rig.at(pending + admission_timeout_us);
  EXPECT_EQ(failure_sent(effects), Error::timeout);
  expect_cleanup(rig, from, {});
}

TEST_F(VisitingPlayerAdmission, StopsTheAdmissionClockOnceTheSetupArrives) {
  Rig rig;
  drive(rig, Stage::pending);
  EXPECT_TRUE(rig.feed(ReceiverReady{rig.now}).empty());
  EXPECT_TRUE(rig.feed(ControllerReady{rig.now}).empty());
  EXPECT_TRUE(rig.feed(ReceiverFailed{rig.now}).empty());
  static_cast<void>(rig.receive(control::Message{vectors().setup.clone()}));
  const auto pending_deadline = rig.now + admission_timeout_us;
  keep_alive(rig, pending_deadline + 1'000'000);
  EXPECT_EQ(rig.spoke.state(), Progress::waiting_for_seat);
}

TEST_F(VisitingPlayerAdmission, FailsWithACodecErrorWhenTheReceiverFails) {
  Rig rig;
  drive(rig, Stage::setup);
  const auto from = rig.log.size();
  const auto effects = rig.feed(ReceiverFailed{rig.now});
  EXPECT_EQ(failure_sent(effects), Error::codec);
  EXPECT_EQ(shown(effects).error, Error::codec);
  EXPECT_EQ(count_sent<control::SourceReady>(rig.log), 0U);
  expect_cleanup(rig, from, {.receiver = true});
}

TEST_F(VisitingPlayerAdmission, ForwardsMalformedDataAsAFailure) {
  Rig rig;
  drive(rig, Stage::pending);
  const auto effects = rig.feed(MalformedData{rig.now, Error::codec});
  EXPECT_EQ(failure_sent(effects), Error::codec);
  expect_cleanup(rig, 0, {});
}

TEST_F(VisitingPlayerAdmission, RefusesASetupForAnotherProtocol) {
  Rig rig;
  drive(rig, Stage::pending);
  auto setup = vectors().setup.clone();
  setup.source_protocol = 9;
  const auto effects = rig.receive(control::Message{setup.clone()});
  EXPECT_EQ(failure_sent(effects), Error::protocol);
  EXPECT_EQ(count<ConfigureReceiver>(rig.log), 0U);
}

TEST_F(VisitingPlayerAdmission, ReportsTheSourceReadyOnlyOnce) {
  Rig rig;
  drive(rig, Stage::source_ready);
  EXPECT_TRUE(rig.feed(ReceiverReady{rig.now}).empty());
  EXPECT_TRUE(rig.feed(ControllerReady{rig.now}).empty());
  EXPECT_EQ(count_sent<control::SourceReady>(rig.log), 1U);
}

TEST_F(VisitingPlayerReadiness, RefusesActivationOfAnotherSeat) {
  for (const auto wrong : {SeatBinding{2, 7}, SeatBinding{3, 8}}) {
    SCOPED_TRACE(::testing::Message() << "seat " << +wrong.seat
                                      << " generation " << wrong.generation);
    Rig rig;
    drive(rig, Stage::ready);
    const auto from = rig.log.size();
    const auto effects = rig.receive(control::Active{wrong});
    EXPECT_EQ(failure_sent(effects), Error::protocol);
    EXPECT_EQ(count<StartMediaReception>(effects), 0U);
    expect_cleanup(rig, from, {.receiver = true});
  }
}

TEST_F(VisitingPlayerReadiness, IgnoresFeedbackAndIdrBeforeBeingActive) {
  Rig rig;
  drive(rig, Stage::granted);
  EXPECT_TRUE(rig.feed(FeedbackReceived{rig.now, feedback(0, 5)}).empty());
  EXPECT_TRUE(rig.feed(IdrNeeded{rig.now}).empty());
  static_cast<void>(rig.feed(ControllerReady{rig.now}));
  EXPECT_TRUE(rig.feed(ControllerReady{rig.now}).empty());
  EXPECT_EQ(count_sent<control::Ready>(rig.log), 1U);
}

TEST_F(VisitingPlayerReadiness, AsksForAnIdrWhilePlaying) {
  Rig rig;
  drive(rig, Stage::active);
  const auto effects = rig.feed(IdrNeeded{rig.now});
  EXPECT_EQ(count_sent<control::SourceIdr>(effects), 1U);
  const auto* idr = sent<control::SourceIdr>(effects);
  ASSERT_NE(idr, nullptr);
  EXPECT_TRUE(rig.feed(ControlAcked{rig.now, idr->token}).empty());
  EXPECT_TRUE(rig.feed(ControlAcked{rig.now, 0}).empty());
  EXPECT_TRUE(rig.feed(ControlAcked{rig.now, 12345}).empty());
  EXPECT_EQ(rig.spoke.state(), Progress::playing);
}

TEST_F(VisitingPlayerOrdering, ConfiguresTheReceiverOnlyAfterAuthentication) {
  Rig rig;
  drive(rig, Stage::active);
  std::optional<std::size_t> authenticated_index;
  std::optional<std::size_t> configure_index;
  std::optional<std::size_t> reception_index;
  for (std::size_t index = 0; index < rig.log.size(); ++index) {
    const auto& effect = rig.log[index];
    if (const auto* item = std::get_if<SendControl>(&effect)) {
      if (is<control::Authenticated>(item->message))
        authenticated_index = index;
    }
    if (std::holds_alternative<ConfigureReceiver>(effect))
      configure_index = index;
    if (std::holds_alternative<StartMediaReception>(effect))
      reception_index = index;
  }
  ASSERT_TRUE(authenticated_index);
  ASSERT_TRUE(configure_index);
  ASSERT_TRUE(reception_index);
  EXPECT_LT(*authenticated_index, *configure_index);
  EXPECT_LT(*configure_index, *reception_index);
  EXPECT_EQ(count<SendControl>(rig.log), 7U);
  EXPECT_EQ(count<EraseSecrets>(rig.log), 0U);
}

TEST_F(VisitingPlayerOrdering, NeverConfiguresTheReceiverBeforeAdmission) {
  for (const auto stage :
       {Stage::channel, Stage::verifying, Stage::confirm_b}) {
    SCOPED_TRACE(static_cast<int>(stage));
    Rig rig;
    drive(rig, stage);
    static_cast<void>(rig.receive(control::Message{vectors().setup.clone()}));
    EXPECT_EQ(count<ConfigureReceiver>(rig.log), 0U);
  }
}

}  // namespace
}  // namespace kakusan_test::visiting
