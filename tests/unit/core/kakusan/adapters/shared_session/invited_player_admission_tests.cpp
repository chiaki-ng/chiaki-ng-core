#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_session/invited_player_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace kakusan_test::invited {
namespace {

class InvitedPlayerMessages : public InvitedPlayerTest {};
class InvitedPlayerAdmission : public InvitedPlayerTest {};
class InvitedPlayerSourceReady : public InvitedPlayerTest {};
class InvitedPlayerReadiness : public InvitedPlayerTest {};
class InvitedPlayerOrdering : public InvitedPlayerTest {};

TEST_F(InvitedPlayerMessages, LeaveOnAMessageTheStateDoesNotExpect) {
  for (const auto stage :
       {Stage::channel, Stage::verifying, Stage::coded,
        Stage::authenticated_sent, Stage::admitting, Stage::reserved,
        Stage::setup_sent, Stage::ready_wait, Stage::media}) {
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
      EXPECT_EQ(count<EraseSecrets>(rig.log, from), 1U);
    }
  }
}

TEST_F(InvitedPlayerAdmission, ReservesASeatOnlyOnceTheUserApproves) {
  Rig rig;
  drive(rig, Stage::admitting, false);
  EXPECT_EQ(count<ReserveSeat>(rig.log), 0U);
  auto effects = rig.feed(SeatReserved{rig.now, seat});
  EXPECT_EQ(effects.size(), 1U);
  EXPECT_EQ(the<ReleaseSeat>(effects).binding, seat);
  EXPECT_EQ(sent<control::AdmissionPending>(rig.log), nullptr);
  EXPECT_TRUE(rig.feed(SourceJoined{rig.now, vectors().setup.clone(), 4, 2,
                                    libchiaking::net::kakusan::button_mask})
                  .empty());
  EXPECT_TRUE(rig.feed(NoCapacity{rig.now}).empty());
  effects = rig.feed(AdmissionApproved{rig.now});
  EXPECT_EQ(count<ReserveSeat>(effects), 1U);
  EXPECT_TRUE(rig.feed(AdmissionApproved{rig.now}).empty());
  EXPECT_EQ(sent<control::SourceSetup>(rig.log), nullptr);
  EXPECT_EQ(count<StartMedia>(rig.log), 0U);
  EXPECT_TRUE(rig.feed(SourceJoined{rig.now, vectors().setup.clone(), 4, 2,
                                    libchiaking::net::kakusan::button_mask})
                  .empty());
  effects = rig.feed(SeatReserved{rig.now, seat});
  EXPECT_NE(sent<control::AdmissionPending>(effects), nullptr);
  EXPECT_EQ(rig.spoke.binding(), seat);
  effects = rig.feed(SeatReserved{rig.now, SeatBinding{4, 8}});
  EXPECT_EQ(effects.size(), 1U);
  EXPECT_EQ(the<ReleaseSeat>(effects).binding, (SeatBinding{4, 8}));
}

TEST_F(InvitedPlayerAdmission, IgnoresSeatAndSourceEventsBeforeAuthentication) {
  Rig rig;
  drive(rig, Stage::verifying);
  EXPECT_TRUE(rig.feed(SourceJoined{rig.now, vectors().setup.clone(), 4, 2,
                                    libchiaking::net::kakusan::button_mask})
                  .empty());
  EXPECT_TRUE(rig.feed(NoCapacity{rig.now}).empty());
  EXPECT_TRUE(rig.feed(SourceJoinFailed{rig.now}).empty());
  EXPECT_TRUE(rig.feed(CapacityRevoked{rig.now}).empty());
  EXPECT_TRUE(rig.feed(RumbleSampled{rig.now, 1, 1}).empty());
  EXPECT_TRUE(rig.feed(InputReceived{rig.now, input(0, 1)}).empty());
  EXPECT_EQ(rig.spoke.state(), Progress::verifying);
  EXPECT_EQ(count<ReserveSeat>(rig.log), 0U);
}

TEST_F(InvitedPlayerAdmission, ReportsNoCapacityToTheGuest) {
  Rig rig;
  drive(rig, Stage::admitting);
  const auto from = rig.log.size();
  const auto effects = rig.feed(NoCapacity{rig.now});
  EXPECT_EQ(failure_sent(effects), Error::no_capacity);
  EXPECT_EQ(shown(effects).error, Error::no_capacity);
  expect_cleanup(rig, from, {});
}

TEST_F(InvitedPlayerAdmission, ReleasesTheSeatWhenTheSourceCannotJoin) {
  Rig rig;
  drive(rig, Stage::reserved);
  const auto from = rig.log.size();
  EXPECT_TRUE(rig.feed(NoCapacity{rig.now}).empty());
  const auto effects = rig.feed(SourceJoinFailed{rig.now});
  EXPECT_EQ(failure_sent(effects), Error::source_join_failed);
  expect_cleanup(rig, from, {.removal = true, .release = true});
}

TEST_F(InvitedPlayerAdmission, TimesOutAMinuteAfterTheReservation) {
  Rig rig;
  drive(rig, Stage::reserved);
  const auto reserved = rig.now;
  EXPECT_EQ(rig.spoke.next_deadline_us(), reserved + ping_interval_us);
  keep_alive(rig, reserved + admission_timeout_us);
  const auto from = rig.log.size();
  EXPECT_EQ(count<SendControl>(rig.at(reserved + admission_timeout_us - 1)),
            0U);
  const auto effects = rig.at(reserved + admission_timeout_us);
  EXPECT_EQ(failure_sent(effects), Error::timeout);
  expect_cleanup(rig, from, {.removal = true, .release = true});
}

TEST_F(InvitedPlayerAdmission, RefusesASourceWithoutRoomForTheGuest) {
  Rig rig;
  drive(rig, Stage::reserved);
  const auto from = rig.log.size();
  const auto effects =
      rig.feed(SourceJoined{rig.now, vectors().setup.clone(), 2, 2,
                            libchiaking::net::kakusan::button_mask});
  EXPECT_EQ(failure_sent(effects), Error::source_join_failed);
  EXPECT_EQ(sent<control::SourceSetup>(rig.log), nullptr);
  expect_cleanup(rig, from, {.removal = true, .release = true});
}

TEST_F(InvitedPlayerAdmission, RefusesASourceThatWithholdsRequiredButtons) {
  Rig rig;
  drive(rig, Stage::reserved);
  const auto effects = rig.feed(
      SourceJoined{rig.now, vectors().setup.clone(), 4, 2,
                   libchiaking::net::kakusan::button_mask &
                       ~static_cast<std::uint32_t>(
                           libchiaking::net::kakusan::Button::options)});
  EXPECT_EQ(failure_sent(effects), Error::source_join_failed);
}

TEST_F(InvitedPlayerAdmission, RefusesASourceWithoutAVideoProfile) {
  Rig rig;
  drive(rig, Stage::reserved);
  auto setup = vectors().setup.clone();
  setup.video_profiles.clear();
  const auto effects = rig.feed(SourceJoined{
      rig.now, std::move(setup), 4, 2, libchiaking::net::kakusan::button_mask});
  EXPECT_EQ(failure_sent(effects), Error::source_join_failed);
}

TEST_F(InvitedPlayerAdmission, SendsTheSetupOnlyOnce) {
  Rig rig;
  drive(rig, Stage::setup_sent);
  EXPECT_TRUE(rig.feed(SourceJoined{rig.now, vectors().setup.clone(), 4, 2,
                                    libchiaking::net::kakusan::button_mask})
                  .empty());
  EXPECT_TRUE(rig.feed(SourceJoinFailed{rig.now}).empty());
  EXPECT_EQ(count_sent<control::SourceSetup>(rig.log), 1U);
}

TEST_F(InvitedPlayerSourceReady, TimesOutAfterTheSetupIsAcknowledged) {
  Rig rig;
  drive(rig, Stage::setup_sent);
  EXPECT_EQ(rig.spoke.next_deadline_us(), rig.now + ping_interval_us);
  EXPECT_TRUE(rig.ack(rig.hello_token).empty());
  EXPECT_TRUE(rig.ack(999).empty());
  EXPECT_EQ(rig.spoke.next_deadline_us(), rig.now + ping_interval_us);
  rig.now += 500'000;
  EXPECT_TRUE(rig.ack(rig.setup_token).empty());
  const auto acked = rig.now;
  EXPECT_TRUE(rig.ack(rig.setup_token).empty());
  keep_alive(rig, acked + source_ready_timeout_us);
  const auto from = rig.log.size();
  EXPECT_EQ(count<SendControl>(rig.at(acked + source_ready_timeout_us - 1)),
            0U);
  const auto effects = rig.at(acked + source_ready_timeout_us);
  EXPECT_EQ(failure_sent(effects), Error::timeout);
  expect_cleanup(rig, from,
                 {.neutral = true, .removal = true, .release = true});
  EXPECT_EQ(count<GrantSeat>(rig.log), 0U);
}

TEST_F(InvitedPlayerSourceReady, GrantsTheSeatBeforeTheSetupIsAcknowledged) {
  Rig rig;
  drive(rig, Stage::setup_sent);
  const auto effects = rig.receive(control::SourceReady{setup_hash()});
  EXPECT_NE(sent<control::SeatGranted>(effects), nullptr);
  EXPECT_TRUE(rig.ack(rig.setup_token).empty());
  EXPECT_EQ(rig.spoke.next_deadline_us(), rig.now + ping_interval_us);
}

TEST_F(InvitedPlayerSourceReady, RefusesTheHashOfAnotherSetup) {
  Rig rig;
  drive(rig, Stage::setup_acked);
  auto wrong = setup_hash();
  wrong[31] ^= std::byte{1};
  const auto from = rig.log.size();
  const auto effects = rig.receive(control::SourceReady{wrong});
  EXPECT_EQ(failure_sent(effects), Error::protocol);
  EXPECT_EQ(count<GrantSeat>(effects), 0U);
  expect_cleanup(rig, from,
                 {.neutral = true, .removal = true, .release = true});
}

TEST_F(InvitedPlayerReadiness, RevokesTheSeatWhenTheGuestIsNeverReady) {
  Rig rig;
  drive(rig, Stage::ready_wait);
  EXPECT_EQ(rig.spoke.next_deadline_us(), rig.now + ping_interval_us);
  rig.now += 1'000'000;
  EXPECT_TRUE(rig.ack(rig.grant_token).empty());
  const auto acked = rig.now;
  EXPECT_TRUE(rig.ack(rig.grant_token).empty());
  keep_alive(rig, acked + ready_timeout_us);
  const auto from = rig.log.size();
  EXPECT_EQ(count<SendControl>(rig.at(acked + ready_timeout_us - 1)), 0U);
  const auto effects = rig.at(acked + ready_timeout_us);
  const auto& revoked = closing<control::SeatRevoked>(effects);
  EXPECT_EQ(revoked.binding, seat);
  EXPECT_EQ(revoked.reason, control::Reason::timeout);
  expect_cleanup(rig, from,
                 {.neutral = true, .removal = true, .release = true});
}

TEST_F(InvitedPlayerReadiness, RefusesReadinessForAnotherSeat) {
  for (const auto wrong : {SeatBinding{2, 7}, SeatBinding{3, 8}}) {
    SCOPED_TRACE(::testing::Message() << "seat " << +wrong.seat
                                      << " generation " << wrong.generation);
    Rig rig;
    drive(rig, Stage::grant_acked);
    const auto effects = rig.receive(control::Ready{wrong});
    EXPECT_EQ(failure_sent(effects), Error::protocol);
    EXPECT_EQ(sent<control::Active>(effects), nullptr);
  }
}

TEST_F(InvitedPlayerReadiness, ActivatesBeforeTheGrantIsAcknowledged) {
  Rig rig;
  drive(rig, Stage::ready_wait);
  const auto effects = rig.receive(control::Ready{seat});
  EXPECT_NE(sent<control::Active>(effects), nullptr);
  EXPECT_TRUE(rig.ack(rig.grant_token).empty());
  EXPECT_EQ(rig.spoke.state(), Progress::playing);
}

TEST_F(InvitedPlayerOrdering, StartsMediaOnlyAfterEveryPrecondition) {
  Rig rig;
  drive(rig, Stage::media);
  std::optional<std::size_t> authenticated_index;
  std::optional<std::size_t> join_index;
  std::optional<std::size_t> setup_index;
  std::optional<std::size_t> active_index;
  std::optional<std::size_t> media_index;
  for (std::size_t index = 0; index < rig.log.size(); ++index) {
    const auto& effect = rig.log[index];
    if (const auto* item = std::get_if<SendControl>(&effect)) {
      if (is<control::Authenticated>(item->message))
        authenticated_index = index;
      if (is<control::SourceSetup>(item->message)) setup_index = index;
      if (is<control::Active>(item->message)) active_index = index;
    }
    if (std::holds_alternative<RequestSourceJoin>(effect)) join_index = index;
    if (std::holds_alternative<StartMedia>(effect)) media_index = index;
  }
  ASSERT_TRUE(authenticated_index);
  ASSERT_TRUE(join_index);
  ASSERT_TRUE(setup_index);
  ASSERT_TRUE(active_index);
  ASSERT_TRUE(media_index);
  EXPECT_LT(*authenticated_index, *join_index);
  EXPECT_LT(*join_index, *setup_index);
  EXPECT_LT(*setup_index, *active_index);
  EXPECT_LT(*active_index, *media_index);
  EXPECT_EQ(count<SendControl>(rig.log), 9U);
  EXPECT_EQ(count<GrantSeat>(rig.log), 1U);
  EXPECT_EQ(count<ReserveSeat>(rig.log), 1U);
  EXPECT_EQ(count<EraseSecrets>(rig.log), 0U);
  EXPECT_EQ(count<ConsumeInvitation>(rig.log), 1U);
}

}  // namespace
}  // namespace kakusan_test::invited
