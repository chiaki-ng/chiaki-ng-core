#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/guest_visit/visiting_player_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <variant>

namespace kakusan_test::visiting {
namespace {

class VisitingPlayerInput : public VisitingPlayerTest {};
class VisitingPlayerRumble : public VisitingPlayerTest {};
class VisitingPlayerLiveness : public VisitingPlayerTest {};

TEST_F(VisitingPlayerInput, PacesAllowedStateWithinTheRateLimits) {
  Rig rig;
  rig.allowed = libchiaking::net::kakusan::button_mask & ~home;
  drive(rig, Stage::granted);
  EXPECT_TRUE(
      rig.feed(ControllerSampled{rig.now, pressed(south | home)}).empty());
  static_cast<void>(rig.feed(ControllerReady{rig.now}));
  auto effects = rig.receive(control::Active{seat});
  const auto t0 = rig.now;
  const auto* item = first<SendInput>(effects);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item->frame.sequence, 0U);
  EXPECT_EQ(item->frame.state.buttons, south);
  rig.now = t0 + 1'000;
  EXPECT_EQ(
      count<SendInput>(rig.feed(ControllerSampled{rig.now, pressed(east)})),
      0U);
  rig.now = t0 + 2'000;
  EXPECT_EQ(count<SendInput>(
                rig.feed(ControllerSampled{rig.now, pressed(north | home)})),
            0U);
  EXPECT_EQ(rig.spoke.next_deadline_us(), t0 + state_min_gap_us);
  EXPECT_TRUE(rig.at(t0 + state_min_gap_us - 1).empty());
  effects = rig.at(t0 + state_min_gap_us);
  item = first<SendInput>(effects);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item->frame.sequence, 1U);
  EXPECT_EQ(item->frame.state.buttons, north);
  EXPECT_EQ(item->frame.elapsed_ms, 4U);
  const auto second = t0 + state_min_gap_us;
  EXPECT_TRUE(rig.at(second + state_period_us - 1).empty());
  effects = rig.at(second + state_period_us);
  item = first<SendInput>(effects);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item->frame.sequence, 2U);
  EXPECT_EQ(item->frame.state.buttons, north);
  rig.now = second + state_period_us + 10'000;
  effects = rig.feed(ControllerSampled{rig.now, pressed(south)});
  item = first<SendInput>(effects);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item->frame.sequence, 3U);
  EXPECT_EQ(item->frame.state.buttons, south);
  libchiaking::net::kakusan::InputState invalid = pressed(east);
  invalid.touch.id = 3;
  EXPECT_TRUE(rig.feed(ControllerSampled{rig.now, invalid}).empty());

  std::size_t idle = 0;
  const auto idle_start = rig.now;
  while (true) {
    const auto deadline = rig.spoke.next_deadline_us();
    ASSERT_TRUE(deadline);
    if (*deadline >= idle_start + 1'000'000) break;
    effects = rig.at(*deadline);
    idle += count<SendInput>(effects);
    answer_ping(rig, effects);
  }
  EXPECT_EQ(idle, 60U);

  std::size_t burst = 0;
  std::uint32_t last_sequence = 0;
  const auto burst_start = rig.now;
  for (std::uint64_t step = 1; step <= 1000; ++step) {
    rig.now = burst_start + step * 1'000;
    auto state = pressed(0);
    state.left_x = static_cast<std::int16_t>(step);
    effects = rig.feed(ControllerSampled{rig.now, state});
    for (const auto& effect : effects) {
      if (const auto* frame = std::get_if<SendInput>(&effect)) {
        ++burst;
        EXPECT_GT(frame->frame.sequence, last_sequence);
        last_sequence = frame->frame.sequence;
      }
    }
    answer_ping(rig, effects);
  }
  EXPECT_LE(burst, 240U);
  EXPECT_GE(burst, 190U);
  const auto deadline = rig.spoke.next_deadline_us();
  ASSERT_TRUE(deadline);
  effects = rig.at(*deadline);
  item = first<SendInput>(effects);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item->frame.state.left_x, 1000);
}

TEST_F(VisitingPlayerRumble, AppliesNewerFeedbackAndStopsItWhenStale) {
  Rig rig;
  drive(rig, Stage::active);
  rig.now += 1'000;
  const auto applied_at = rig.now;
  auto effects = rig.feed(FeedbackReceived{rig.now, feedback(5, 100)});
  const auto* applied = first<ApplyRumble>(effects);
  ASSERT_NE(applied, nullptr);
  EXPECT_EQ(applied->low_frequency, 100U);
  EXPECT_EQ(applied->high_frequency, 0U);
  EXPECT_TRUE(rig.feed(FeedbackReceived{rig.now, feedback(5, 200)}).empty());
  EXPECT_TRUE(rig.feed(FeedbackReceived{rig.now, feedback(4, 200)}).empty());
  EXPECT_TRUE(
      rig.feed(FeedbackReceived{rig.now, feedback(0x80000005U, 200)}).empty());
  EXPECT_EQ(count<StopRumble>(rig.at(applied_at + rumble_stop_us - 1)), 0U);
  effects = rig.at(applied_at + rumble_stop_us);
  EXPECT_EQ(count<StopRumble>(effects), 1U);
  EXPECT_EQ(count<StopRumble>(rig.at(applied_at + 2 * rumble_stop_us)), 0U);
  rig.now = applied_at + 300'000;
  effects = rig.feed(FeedbackReceived{rig.now, feedback(6, 50)});
  EXPECT_EQ(the<ApplyRumble>(effects).low_frequency, 50U);
  rig.now += 100'000;
  effects = rig.feed(FeedbackReceived{rig.now, feedback(7, 0)});
  EXPECT_EQ(the<ApplyRumble>(effects).low_frequency, 0U);
  const auto zero_at = rig.now;
  EXPECT_EQ(count<StopRumble>(rig.at(zero_at + rumble_stop_us)), 0U);
  rig.now += 1;
  effects = rig.feed(FeedbackReceived{rig.now, feedback(8, 10)});
  const auto renewed = rig.now;
  rig.now = renewed + 200'000;
  static_cast<void>(rig.feed(FeedbackReceived{rig.now, feedback(9, 10)}));
  EXPECT_EQ(count<StopRumble>(rig.at(renewed + rumble_stop_us)), 0U);
  EXPECT_EQ(count<StopRumble>(rig.at(renewed + 200'000 + rumble_stop_us)), 1U);
}

TEST_F(VisitingPlayerRumble, LeavesOnFeedbackForAnotherSeat) {
  Rig rig;
  drive(rig, Stage::active);
  const auto from = rig.log.size();
  const auto effects =
      rig.feed(FeedbackReceived{rig.now, feedback(0, 1, SeatBinding{3, 8})});
  EXPECT_EQ(failure_sent(effects), Error::protocol);
  expect_cleanup(rig, from, {.receiver = true, .rumble = true});
}

TEST_F(VisitingPlayerLiveness, LeavesWhenPongsStopMatchingPings) {
  Rig rig;
  drive(rig, Stage::admitting);
  const auto base = rig.admitted_at;
  EXPECT_TRUE(rig.at(base + ping_interval_us - 1).empty());
  auto effects = rig.at(base + ping_interval_us);
  const auto first_nonce = message<control::Ping>(effects).nonce;
  rig.now = base + 2'500'000;
  EXPECT_TRUE(rig.receive(control::Pong{first_nonce}).empty());
  rig.now = base + 3'000'000;
  EXPECT_TRUE(rig.receive(control::Pong{first_nonce}).empty());
  const auto second =
      message<control::Ping>(rig.at(base + 2 * ping_interval_us)).nonce;
  const auto third =
      message<control::Ping>(rig.at(base + 3 * ping_interval_us)).nonce;
  EXPECT_NE(second, first_nonce);
  EXPECT_NE(third, second);
  rig.now = base + 6'500'000;
  EXPECT_TRUE(rig.receive(control::Pong{second}).empty());
  effects = rig.at(base + 2'500'000 + liveness_timeout_us - 1);
  EXPECT_EQ(terminal(effects), nullptr);
  const auto from = rig.log.size();
  effects = rig.at(base + 2'500'000 + liveness_timeout_us);
  EXPECT_EQ(closing<control::Leave>(effects).reason, control::Reason::timeout);
  expect_cleanup(rig, from, {});
}

TEST_F(VisitingPlayerLiveness, CountsTheHostsPingAsLife) {
  Rig rig;
  drive(rig, Stage::admitting);
  control::Ping ping;
  ping.nonce[1] = std::byte{0x55};
  rig.now += 4'000'000;
  const auto effects = rig.receive(ping);
  EXPECT_EQ(message<control::Pong>(effects).nonce, ping.nonce);
  const auto refreshed = rig.now;
  static_cast<void>(rig.at(refreshed + liveness_timeout_us - 1));
  EXPECT_EQ(rig.spoke.state(), Progress::waiting_for_seat);
  static_cast<void>(rig.at(refreshed + liveness_timeout_us));
  EXPECT_EQ(rig.spoke.state(), Progress::leaving);
}

TEST_F(VisitingPlayerLiveness, CountsOnlyNewFeedbackAsLifeWhilePlaying) {
  Rig rig;
  drive(rig, Stage::active);
  const auto begin = rig.now;
  std::uint32_t sequence = 0;
  for (auto time = begin + 100'000; time < begin + 20'000'000;
       time += 100'000) {
    static_cast<void>(rig.at(time));
    EXPECT_EQ(count<ApplyRumble>(
                  rig.feed(FeedbackReceived{rig.now, feedback(sequence++, 0)})),
              1U);
  }
  EXPECT_EQ(rig.spoke.state(), Progress::playing);
  const auto last = rig.now;
  for (auto time = last + 100'000; time < last + liveness_timeout_us;
       time += 100'000) {
    static_cast<void>(rig.at(time));
    EXPECT_EQ(
        count<ApplyRumble>(rig.feed(FeedbackReceived{rig.now, feedback(0, 0)})),
        0U);
  }
  EXPECT_EQ(rig.spoke.state(), Progress::playing);
  const auto from = rig.log.size();
  static_cast<void>(rig.at(last + liveness_timeout_us));
  EXPECT_EQ(rig.spoke.state(), Progress::leaving);
  expect_cleanup(rig, from, {.receiver = true, .rumble = true});
}

TEST_F(VisitingPlayerLiveness, SendsNoPingsBeforeAuthentication) {
  Rig rig;
  drive(rig, Stage::confirm_b);
  static_cast<void>(rig.at(start_us + 200'000'000));
  EXPECT_EQ(rig.spoke.state(), Progress::verifying);
  EXPECT_EQ(count_sent<control::Ping>(rig.log), 0U);
}

}  // namespace
}  // namespace kakusan_test::visiting
