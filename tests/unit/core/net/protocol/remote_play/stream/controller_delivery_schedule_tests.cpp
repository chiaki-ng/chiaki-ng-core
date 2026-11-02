#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "tests/unit/core/net/protocol/remote_play/stream/controller_delivery_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>

namespace remote_play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;

class FeedbackStateScheduling : public ControllerDeliveryRig {
 protected:
  void SetUp() override { schedule.mark_sent(state, 1'000); }

  remote_play::FeedbackStateSchedule schedule;
  State state;
};

TEST_F(FeedbackStateScheduling, CommitsOnlyWhenAStateIsMarkedSent) {
  remote_play::FeedbackStateSchedule fresh;
  const State idle;
  EXPECT_TRUE(fresh.should_send(idle, 1'000));
  EXPECT_TRUE(fresh.should_send(idle, 1'001));
}

TEST_F(FeedbackStateScheduling,
       RepeatsAnUnchangedStateAfterTwoHundredMilliseconds) {
  EXPECT_FALSE(schedule.should_send(state, 200'999));
  EXPECT_TRUE(schedule.should_send(state, 201'000));
}

TEST_F(FeedbackStateScheduling, SendsWhenTheClockGoesBackwards) {
  EXPECT_TRUE(schedule.should_send(state, 999));
}

TEST_F(FeedbackStateScheduling, IgnoresButtonsTriggersAndTouches) {
  state.buttons = buttons({Button::cross});
  state.l2 = 100;
  state.r2 = 200;
  state.touches[0] = {3, 500, 600};
  EXPECT_FALSE(schedule.should_send(state, 1'001));
}

TEST_F(FeedbackStateScheduling, SendsMovedSticksAndMotionBeyondTheTolerance) {
  state.buttons = buttons({Button::cross});
  state.l2 = 100;
  state.r2 = 200;
  state.touches[0] = {3, 500, 600};
  state.left_x = 1;
  EXPECT_TRUE(schedule.should_send(state, 1'001));
  schedule.mark_sent(state, 1'001);
  state.gyro_x = 0.00000005F;
  EXPECT_FALSE(schedule.should_send(state, 1'002));
  state.gyro_x = 0.0000002F;
  EXPECT_TRUE(schedule.should_send(state, 1'002));
  schedule.mark_sent(state, 1'002);
  state.orient_w = 0.9F;
  EXPECT_TRUE(schedule.should_send(state, 1'003));
  schedule.mark_sent(state, 1'003);
  state.right_y = -1;
  EXPECT_TRUE(schedule.should_send(state, 1'004));
  schedule.mark_sent(state, 1'004);
  state.gyro_x = std::numeric_limits<float>::quiet_NaN();
  EXPECT_TRUE(schedule.should_send(state, 1'005));
}

TEST_F(FeedbackStateScheduling, FollowsTheIntervalOfItsPolicy) {
  state.gyro_x = 0.0000002F;
  remote_play::FeedbackStateSchedule faster{Policy{1'000, 0.0000001F}};
  faster.mark_sent(state, 10);
  EXPECT_FALSE(faster.should_send(state, 1'009));
  EXPECT_TRUE(faster.should_send(state, 1'010));
}

class FeedbackDeliveryPolicies : public ControllerDeliveryRig {};

TEST_F(FeedbackDeliveryPolicies, CloudLinksDefaultToStandardFeedback) {
  const play::CloudLinkConfiguration cloud;
  EXPECT_EQ(cloud.feedback, remote_play::FeedbackMode::standard);
}

TEST_F(FeedbackDeliveryPolicies, SwitchCompatibilityUsesTheSwitchPolicy) {
  play::CloudLinkConfiguration cloud;
  cloud.feedback = remote_play::FeedbackMode::switch_compatibility;
  const auto console = Policy::for_mode(cloud.feedback);
  const auto expected = Policy::switch_console();
  EXPECT_EQ(console.recent_history_events, expected.recent_history_events);
  EXPECT_EQ(console.history_repeats, expected.history_repeats);
  EXPECT_EQ(console.per_edge_flush, expected.per_edge_flush);
}

TEST_F(FeedbackDeliveryPolicies,
       StandardFeedbackSendsTheWholeWindowTwiceFlushingEachEdge) {
  const auto expected = Policy::switch_console();
  const auto standard = Policy::for_mode(remote_play::FeedbackMode::standard);
  EXPECT_EQ(standard.recent_history_events, expected.recent_history_events);
  EXPECT_EQ(standard.history_repeats, expected.history_repeats);
  EXPECT_EQ(standard.per_edge_flush, expected.per_edge_flush);
  EXPECT_EQ(standard.recent_history_events,
            remote_play::feedback_history_capacity);
  EXPECT_EQ(standard.history_repeats, 2U);
  EXPECT_TRUE(standard.per_edge_flush);
}

class ControllerDeliveryDeadline : public ControllerDeliveryRig {};

TEST_F(ControllerDeliveryDeadline,
       FollowsTheHistoryRepeatsThenTheStateRefresh) {
  Sink sink;
  Delivery delivery{sink, 1'000};
  EXPECT_EQ(delivery.next_deadline_us(), std::optional<std::uint64_t>{201'000});
  State state;
  state.buttons = buttons({Button::cross});
  ASSERT_TRUE(delivery.submit(state, 5'000));
  const auto calls = sink.calls.size();
  EXPECT_EQ(delivery.next_deadline_us(), std::optional<std::uint64_t>{25'000});
  EXPECT_TRUE(delivery.refresh(24'999));
  EXPECT_EQ(sink.calls.size(), calls);
  EXPECT_TRUE(delivery.refresh(25'000));
  EXPECT_EQ(sink.calls.size(), calls + 1);
  EXPECT_EQ(delivery.next_deadline_us(), std::optional<std::uint64_t>{45'000});
  EXPECT_TRUE(delivery.refresh(45'000));
  EXPECT_EQ(sink.calls.size(), calls + 2);
  EXPECT_EQ(delivery.next_deadline_us(), std::optional<std::uint64_t>{201'000});
  EXPECT_TRUE(delivery.refresh(200'999));
  EXPECT_EQ(sink.calls.size(), calls + 2);
  EXPECT_TRUE(delivery.refresh(201'000));
  EXPECT_EQ(sink.calls.size(), calls + 3);
  EXPECT_EQ(sink.calls.back().kind, Kind::state);
  EXPECT_EQ(delivery.next_deadline_us(), std::optional<std::uint64_t>{401'000});
}

class ControllerSampleFeedback : public ControllerDeliveryRig {
 protected:
  static constexpr float tolerance = remote_play::default_motion_tolerance;
  const State left;
  State right;
};

TEST_F(ControllerSampleFeedback, IsTheSameWithinTheMotionTolerance) {
  EXPECT_TRUE(
      remote_play::ControllerSample::same_feedback(left, right, tolerance));
  right.gyro_x = 0.00000005F;
  EXPECT_TRUE(
      remote_play::ControllerSample::same_feedback(left, right, tolerance));
}

TEST_F(ControllerSampleFeedback, DiffersBeyondTheMotionTolerance) {
  right.gyro_x = 0.0000002F;
  EXPECT_FALSE(
      remote_play::ControllerSample::same_feedback(left, right, tolerance));
}

TEST_F(ControllerSampleFeedback, DiffersWithATouch) {
  right.touches[1] = {2, 10, 10};
  EXPECT_FALSE(
      remote_play::ControllerSample::same_feedback(left, right, tolerance));
}

TEST_F(ControllerSampleFeedback, DiffersWithATrigger) {
  right.l2 = 1;
  EXPECT_FALSE(
      remote_play::ControllerSample::same_feedback(left, right, tolerance));
}

TEST_F(ControllerSampleFeedback, DiffersWithAButton) {
  right.buttons = 1;
  EXPECT_FALSE(
      remote_play::ControllerSample::same_feedback(left, right, tolerance));
}

TEST_F(ControllerSampleFeedback, DiffersWithAStick) {
  right.left_y = 1;
  EXPECT_FALSE(
      remote_play::ControllerSample::same_feedback(left, right, tolerance));
}

}  // namespace
}  // namespace remote_play_test
