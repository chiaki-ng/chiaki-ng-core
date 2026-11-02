#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_session/invited_player_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace kakusan_test::invited {
namespace {

class InvitedPlayerMedia : public InvitedPlayerTest {};
class InvitedPlayerInput : public InvitedPlayerTest {};
class InvitedPlayerWatchdog : public InvitedPlayerTest {};
class InvitedPlayerRumble : public InvitedPlayerTest {};
class InvitedPlayerLiveness : public InvitedPlayerTest {};

TEST_F(InvitedPlayerMedia, StartsOnlyOnceActiveIsAcknowledged) {
  Rig rig;
  drive(rig, Stage::active);
  EXPECT_EQ(count<StartMedia>(rig.log), 0U);
  EXPECT_TRUE(rig.ack(rig.grant_token).empty());
  EXPECT_TRUE(rig.ack(rig.setup_token).empty());
  auto effects = rig.ack(rig.active_token);
  EXPECT_EQ(count<StartMedia>(effects), 1U);
  EXPECT_TRUE(rig.ack(rig.active_token).empty());
  const auto from = rig.log.size();
  effects = rig.feed(Cancel{rig.now, control::Reason::user});
  EXPECT_EQ(closing<control::End>(effects).reason, control::Reason::user);
  expect_cleanup(
      rig, from,
      {.media = true, .neutral = true, .removal = true, .release = true});
}

TEST_F(InvitedPlayerMedia, AsksTheSourceForAnIdrOnRequest) {
  Rig rig;
  drive(rig, Stage::media);
  const auto effects = rig.receive(control::SourceIdr{});
  EXPECT_EQ(count<RequestIdr>(effects), 1U);
  EXPECT_EQ(rig.spoke.state(), Progress::playing);
}

TEST_F(InvitedPlayerInput, IsAppliedOnlyOnceTheGuestIsReady) {
  Rig rig;
  drive(rig, Stage::grant_acked);
  EXPECT_TRUE(rig.feed(InputReceived{rig.now, input(0, 1)}).empty());
  EXPECT_TRUE(
      rig.feed(InputReceived{rig.now, input(0, 1, SeatBinding{2, 7})}).empty());
  EXPECT_EQ(rig.spoke.state(), Progress::getting_ready);
  static_cast<void>(rig.receive(control::Ready{seat}));
  const auto effects = rig.feed(InputReceived{rig.now, input(0, 1)});
  EXPECT_EQ(count<ApplyInput>(effects), 1U);
}

TEST_F(InvitedPlayerInput, LosesButtonsTheSourceWithholds) {
  Rig rig;
  drive(rig, Stage::admitting);
  EXPECT_EQ(rig.feed(SeatReserved{rig.now, seat}).size(), 2U);
  constexpr std::uint32_t limited =
      libchiaking::net::kakusan::button_mask &
      ~static_cast<std::uint32_t>(libchiaking::net::kakusan::Button::home);
  static_cast<void>(
      rig.feed(SourceJoined{rig.now, vectors().setup.clone(), 4, 2, limited}));
  static_cast<void>(rig.receive(control::SourceReady{setup_hash()}));
  EXPECT_EQ(message<control::SeatGranted>(rig.log).allowed_buttons, limited);
  static_cast<void>(rig.receive(control::Ready{seat}));
  const auto home =
      static_cast<std::uint32_t>(libchiaking::net::kakusan::Button::home);
  const auto south =
      static_cast<std::uint32_t>(libchiaking::net::kakusan::Button::south);
  const auto effects = rig.feed(InputReceived{rig.now, input(5, home | south)});
  const auto* applied = first<ApplyInput>(effects);
  ASSERT_NE(applied, nullptr);
  EXPECT_EQ(applied->binding, seat);
  EXPECT_EQ(applied->state.buttons, south);
  EXPECT_EQ(applied->state.left_x, 100);
}

TEST_F(InvitedPlayerInput, AcceptsOnlyNewerSequencesAcrossTheWrap) {
  Rig rig;
  drive(rig, Stage::media);
  auto effects = rig.feed(InputReceived{rig.now, input(0xfffffffeU, 1)});
  EXPECT_EQ(count<ApplyInput>(effects), 1U);
  EXPECT_TRUE(rig.feed(InputReceived{rig.now, input(0xfffffffeU, 2)}).empty());
  EXPECT_TRUE(rig.feed(InputReceived{rig.now, input(0x7ffffffeU, 2)}).empty());
  EXPECT_TRUE(rig.feed(InputReceived{rig.now, input(0xfffffffdU, 2)}).empty());
  effects = rig.feed(InputReceived{rig.now, input(1, 4)});
  EXPECT_EQ(the<ApplyInput>(effects).state.buttons, 4U);
  EXPECT_TRUE(rig.feed(InputReceived{rig.now, input(0x80000001U, 8)}).empty());
  effects = rig.feed(InputReceived{rig.now, input(0x80000000U, 8)});
  EXPECT_EQ(count<ApplyInput>(effects), 1U);
}

TEST_F(InvitedPlayerInput, LeavesOnInputForAnotherSeat) {
  for (const auto wrong : {SeatBinding{2, 7}, SeatBinding{3, 6}}) {
    SCOPED_TRACE(::testing::Message() << "seat " << +wrong.seat
                                      << " generation " << wrong.generation);
    Rig rig;
    drive(rig, Stage::media);
    const auto from = rig.log.size();
    const auto effects = rig.feed(InputReceived{rig.now, input(0, 1, wrong)});
    EXPECT_EQ(failure_sent(effects), Error::protocol);
    expect_cleanup(
        rig, from,
        {.media = true, .neutral = true, .removal = true, .release = true});
  }
}

TEST_F(InvitedPlayerInput, LeavesOnAnInvalidTouch) {
  Rig rig;
  drive(rig, Stage::active);
  auto bad = input(0, 1);
  bad.state.touch.id = 4;
  const auto effects = rig.feed(InputReceived{rig.now, bad});
  EXPECT_EQ(failure_sent(effects), Error::protocol);
}

TEST_F(InvitedPlayerWatchdog, InjectsNeutralWhenInputPauses) {
  Rig rig;
  drive(rig, Stage::media);
  const auto active = rig.active_at;
  EXPECT_EQ(rig.spoke.next_deadline_us(), active + input_neutral_us);
  rig.now = active + 10'000;
  static_cast<void>(rig.feed(InputReceived{rig.now, input(0, 1)}));
  rig.now = active + 200'000;
  EXPECT_TRUE(rig.feed(InputReceived{rig.now, input(0, 1)}).empty());
  EXPECT_EQ(rig.spoke.next_deadline_us(), active + 10'000 + input_neutral_us);
  EXPECT_TRUE(rig.at(active + 10'000 + input_neutral_us - 1).empty());
  auto effects = rig.at(active + 10'000 + input_neutral_us);
  EXPECT_EQ(count<InjectNeutral>(effects), 1U);
  EXPECT_EQ(the<InjectNeutral>(effects).binding, seat);
  EXPECT_TRUE(rig.at(active + 400'000).empty());
  rig.now = active + 500'000;
  effects = rig.feed(InputReceived{rig.now, input(1, 2)});
  EXPECT_EQ(count<ApplyInput>(effects), 1U);
  EXPECT_TRUE(rig.at(active + 500'000 + input_neutral_us - 1).empty());
  EXPECT_EQ(count<InjectNeutral>(rig.at(active + 500'000 + input_neutral_us)),
            1U);
}

TEST_F(InvitedPlayerWatchdog, InjectsNeutralWhenNoInputEverArrives) {
  Rig rig;
  drive(rig, Stage::media);
  EXPECT_EQ(count<InjectNeutral>(rig.at(rig.active_at + input_neutral_us - 1)),
            0U);
  EXPECT_EQ(count<InjectNeutral>(rig.at(rig.active_at + input_neutral_us)), 1U);
}

TEST_F(InvitedPlayerWatchdog, RevokesTheSeatWhenOnlyStaleInputArrives) {
  Rig rig;
  drive(rig, Stage::media);
  rig.now = rig.active_at + 1'000;
  static_cast<void>(rig.feed(InputReceived{rig.now, input(0, 1)}));
  const auto last = rig.now;
  std::size_t pings = 0;
  for (auto time = rig.admitted_at + ping_interval_us;
       time < last + input_revoke_us; time += ping_interval_us) {
    const auto effects = rig.at(time);
    if (const auto* item = sent<control::Ping>(effects)) {
      ++pings;
      static_cast<void>(rig.receive(
          control::Pong{std::get<control::Ping>(item->message).nonce}));
    }
    EXPECT_EQ(rig.spoke.state(), Progress::playing);
    rig.now = time + 1;
    EXPECT_TRUE(rig.feed(InputReceived{rig.now, input(0, 1)}).empty());
  }
  EXPECT_EQ(pings, 3U);
  const auto from = rig.log.size();
  EXPECT_EQ(count<SendControl>(rig.at(last + input_revoke_us - 1)), 0U);
  EXPECT_EQ(rig.spoke.state(), Progress::playing);
  const auto effects = rig.at(last + input_revoke_us);
  const auto& revoked = closing<control::SeatRevoked>(effects);
  EXPECT_EQ(revoked.binding, seat);
  EXPECT_EQ(revoked.reason, control::Reason::timeout);
  expect_cleanup(
      rig, from,
      {.media = true, .neutral = true, .removal = true, .release = true});
  expect_closed_after_grace(rig);
}

TEST_F(InvitedPlayerWatchdog, KeepsPlayingWhileInputFlows) {
  Rig rig;
  drive(rig, Stage::media);
  std::uint32_t sequence = 0;
  const auto begin = rig.now;
  for (auto time = begin + 100'000; time < begin + 20'000'000;
       time += 100'000) {
    static_cast<void>(rig.at(time));
    EXPECT_EQ(count<ApplyInput>(
                  rig.feed(InputReceived{rig.now, input(sequence++, 1)})),
              1U);
  }
  EXPECT_EQ(rig.spoke.state(), Progress::playing);
  EXPECT_GT(count_sent<control::Ping>(rig.log), 5U);
}

TEST_F(InvitedPlayerRumble, PacesFeedbackOnceMediaStarts) {
  Rig rig;
  drive(rig, Stage::active);
  EXPECT_TRUE(rig.feed(RumbleSampled{rig.now, 100, 0}).empty());
  rig.now += 5'000;
  auto effects = rig.ack(rig.active_token);
  EXPECT_EQ(count<StartMedia>(effects), 1U);
  const auto* feedback = first<SendFeedback>(effects);
  ASSERT_NE(feedback, nullptr);
  EXPECT_EQ(feedback->frame.binding, seat);
  EXPECT_EQ(feedback->frame.sequence, 0U);
  EXPECT_EQ(feedback->frame.low_frequency, 100U);
  EXPECT_EQ(feedback->frame.elapsed_ms, 5U);
  const auto sent_at = rig.now;
  rig.now = sent_at + 1'000;
  EXPECT_TRUE(rig.feed(RumbleSampled{rig.now, 200, 0}).empty());
  rig.now = sent_at + 2'000;
  EXPECT_TRUE(rig.feed(RumbleSampled{rig.now, 300, 7}).empty());
  EXPECT_EQ(rig.spoke.next_deadline_us(), sent_at + state_min_gap_us);
  EXPECT_TRUE(rig.at(sent_at + state_min_gap_us - 1).empty());
  effects = rig.at(sent_at + state_min_gap_us);
  feedback = first<SendFeedback>(effects);
  ASSERT_NE(feedback, nullptr);
  EXPECT_EQ(feedback->frame.sequence, 1U);
  EXPECT_EQ(feedback->frame.low_frequency, 300U);
  EXPECT_EQ(feedback->frame.high_frequency, 7U);
  EXPECT_EQ(count<SendFeedback>(effects), 1U);
  const auto second = sent_at + state_min_gap_us;
  EXPECT_EQ(count<SendFeedback>(rig.at(second + state_period_us - 1)), 0U);
  effects = rig.at(second + state_period_us);
  feedback = first<SendFeedback>(effects);
  ASSERT_NE(feedback, nullptr);
  EXPECT_EQ(feedback->frame.sequence, 2U);
  EXPECT_EQ(feedback->frame.low_frequency, 300U);
  const auto third = second + state_period_us;
  EXPECT_EQ(feedback->frame.elapsed_ms,
            static_cast<std::uint32_t>((third - rig.active_at) / 1000));
  rig.now = third + state_min_gap_us;
  effects = rig.feed(RumbleSampled{rig.now, 0, 0});
  feedback = first<SendFeedback>(effects);
  ASSERT_NE(feedback, nullptr);
  EXPECT_EQ(feedback->frame.low_frequency, 0U);
  EXPECT_EQ(feedback->frame.sequence, 3U);
  for (std::uint64_t step = 1; step < 10; ++step)
    EXPECT_EQ(count<SendFeedback>(rig.at(rig.now + state_period_us)), 0U);
  rig.now += 1;
  EXPECT_EQ(count<SendFeedback>(rig.feed(RumbleSampled{rig.now, 0, 0})), 0U);
  std::size_t burst = 0;
  for (std::uint64_t step = 0; step < 1000; ++step) {
    rig.now += 1'000;
    burst += count<SendFeedback>(rig.feed(
        RumbleSampled{rig.now, static_cast<std::uint16_t>(step + 1), 0}));
    burst += count<SendFeedback>(rig.at(rig.now));
  }
  EXPECT_LE(burst, 240U);
  EXPECT_GE(burst, 190U);
}

TEST_F(InvitedPlayerLiveness, EndsWhenPongsStopMatchingPings) {
  Rig rig;
  drive(rig, Stage::admitting, false);
  const auto base = rig.admitted_at;
  EXPECT_EQ(rig.spoke.next_deadline_us(), base + ping_interval_us);
  EXPECT_TRUE(rig.at(base + ping_interval_us - 1).empty());
  auto effects = rig.at(base + ping_interval_us);
  EXPECT_EQ(count_sent<control::Ping>(effects), 1U);
  const auto first_nonce = message<control::Ping>(effects).nonce;
  rig.now = base + 2'500'000;
  EXPECT_TRUE(rig.receive(control::Pong{first_nonce}).empty());
  rig.now = base + 3'000'000;
  EXPECT_TRUE(rig.receive(control::Pong{first_nonce}).empty());
  effects = rig.at(base + 2 * ping_interval_us);
  const auto second_nonce = message<control::Ping>(effects).nonce;
  EXPECT_NE(second_nonce, first_nonce);
  effects = rig.at(base + 3 * ping_interval_us);
  const auto third_nonce = message<control::Ping>(effects).nonce;
  EXPECT_NE(third_nonce, second_nonce);
  rig.now = base + 6'500'000;
  EXPECT_TRUE(rig.receive(control::Pong{second_nonce}).empty());
  effects = rig.at(base + 2'500'000 + liveness_timeout_us - 1);
  EXPECT_EQ(count_sent<control::Ping>(effects), 1U);
  EXPECT_EQ(terminal(effects), nullptr);
  EXPECT_EQ(rig.spoke.state(), Progress::waiting_for_seat);
  const auto from = rig.log.size();
  effects = rig.at(base + 2'500'000 + liveness_timeout_us);
  EXPECT_EQ(closing<control::End>(effects).reason, control::Reason::timeout);
  expect_cleanup(rig, from, {});
}

TEST_F(InvitedPlayerLiveness, CountsARepeatedPongOnce) {
  Rig rig;
  drive(rig, Stage::admitting, false);
  const auto base = rig.admitted_at;
  static_cast<void>(rig.at(base + ping_interval_us));
  const auto second =
      message<control::Ping>(rig.at(base + 2 * ping_interval_us)).nonce;
  rig.now = base + 5'000'000;
  EXPECT_TRUE(rig.receive(control::Pong{second}).empty());
  EXPECT_TRUE(rig.receive(control::Pong{second}).empty());
  static_cast<void>(rig.at(base + 5'000'000 + liveness_timeout_us - 1));
  EXPECT_EQ(rig.spoke.state(), Progress::waiting_for_seat);
  const auto effects = rig.at(base + 5'000'000 + liveness_timeout_us);
  EXPECT_EQ(closing<control::End>(effects).reason, control::Reason::timeout);
}

TEST_F(InvitedPlayerLiveness, EndsWithoutAnyPong) {
  Rig rig;
  drive(rig, Stage::admitting, false);
  const auto base = rig.admitted_at;
  EXPECT_EQ(count_sent<control::Ping>(rig.at(base + ping_interval_us)), 1U);
  EXPECT_EQ(count_sent<control::Ping>(rig.at(base + 2 * ping_interval_us)), 1U);
  static_cast<void>(rig.at(base + liveness_timeout_us - 1));
  EXPECT_EQ(rig.spoke.state(), Progress::waiting_for_seat);
  const auto effects = rig.at(base + liveness_timeout_us);
  EXPECT_EQ(closing<control::End>(effects).reason, control::Reason::timeout);
  EXPECT_EQ(count_sent<control::Ping>(effects), 0U);
}

TEST_F(InvitedPlayerLiveness, CountsTheGuestsPingAsLife) {
  Rig rig;
  drive(rig, Stage::admitting, false);
  control::Ping ping;
  ping.nonce[3] = std::byte{0x77};
  rig.now += 3'000'000;
  const auto effects = rig.receive(ping);
  EXPECT_EQ(message<control::Pong>(effects).nonce, ping.nonce);
  const auto refreshed = rig.now;
  static_cast<void>(rig.at(refreshed + liveness_timeout_us - 1));
  EXPECT_EQ(rig.spoke.state(), Progress::waiting_for_seat);
  static_cast<void>(rig.at(refreshed + liveness_timeout_us));
  EXPECT_EQ(rig.spoke.state(), Progress::leaving);
}

}  // namespace
}  // namespace kakusan_test::invited
