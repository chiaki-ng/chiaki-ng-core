#include "src/core/net/protocol/remote_play/stream/controller_delivery.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <variant>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

class ControllerFeedbackState : public ::testing::Test {
 protected:
  void SetUp() override {
    state.controller_id = 3;
    state.left_x = -32768;
    state.left_y = 32767;
    state.right_x = 1234;
    state.right_y = -5678;
    state.gyro_x = 1.5F;
    state.gyro_y = -2.25F;
    state.gyro_z = 0.5F;
    state.accel_x = 0.25F;
    state.accel_y = -1.0F;
    state.accel_z = 4.0F;
    state.orient_x = 0.1F;
    state.orient_y = 0.2F;
    state.orient_z = -0.3F;
    state.orient_w = 0.9F;
  }

  remote_play::ControllerState state;
};

TEST_F(ControllerFeedbackState, CarriesTheSticksAndMotionOfTheState) {
  const auto mapped = remote_play::ControllerSample::feedback_state(state);
  ASSERT_TRUE(mapped);
  const auto& value = mapped.value();
  EXPECT_EQ(value.controller_id, 3U);
  EXPECT_EQ(value.left_x, -32768);
  EXPECT_EQ(value.left_y, 32767);
  EXPECT_EQ(value.right_x, 1234);
  EXPECT_EQ(value.right_y, -5678);
  EXPECT_EQ(value.gyro_x, 1.5F);
  EXPECT_EQ(value.gyro_y, -2.25F);
  EXPECT_EQ(value.gyro_z, 0.5F);
  EXPECT_EQ(value.accel_x, 0.25F);
  EXPECT_EQ(value.accel_y, -1.0F);
  EXPECT_EQ(value.accel_z, 4.0F);
  EXPECT_EQ(value.orient_x, 0.1F);
  EXPECT_EQ(value.orient_y, 0.2F);
  EXPECT_EQ(value.orient_z, -0.3F);
  EXPECT_EQ(value.orient_w, 0.9F);
}

TEST_F(ControllerFeedbackState, EmulatesTriggerStatesThePadDidNotReport) {
  const auto mapped = remote_play::ControllerSample::feedback_state(state);
  ASSERT_TRUE(mapped);
  EXPECT_TRUE(mapped.value().trigger_states_emulated);
  EXPECT_EQ(mapped.value().left_trigger_state, 0U);
  EXPECT_EQ(mapped.value().right_trigger_state, 0U);
}

TEST_F(ControllerFeedbackState, CarriesReportedTriggerStates) {
  state.trigger_states = std::array<std::uint8_t, 2>{4, 7};
  const auto reported = remote_play::ControllerSample::feedback_state(state);
  ASSERT_TRUE(reported);
  EXPECT_FALSE(reported.value().trigger_states_emulated);
  EXPECT_EQ(reported.value().left_trigger_state, 4U);
  EXPECT_EQ(reported.value().right_trigger_state, 7U);
}

TEST_F(ControllerFeedbackState, AcceptsControllerIdsZeroToThreeOnly) {
  state.controller_id = 4;
  const auto bad = remote_play::ControllerSample::feedback_state(state);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error(),
            remote_play::ControllerFeedbackError::invalid_controller_id);
  state.controller_id = 0;
  EXPECT_TRUE(remote_play::ControllerSample::feedback_state(state));
}

class ControllerFeedbackHistory : public ::testing::Test {
 protected:
  remote_play::ControllerState before;
  remote_play::ControllerState after;
};

TEST_F(ControllerFeedbackHistory, ReportsAReplacedTouchAsTheOldTouchLifting) {
  before.touches[0] = {3, 100, 200};
  after.touches[0] = {7, 300, 400};
  const auto delta =
      remote_play::ControllerSample::history_delta(before, after);
  ASSERT_TRUE(delta);
  ASSERT_EQ(delta.value().size(), 1U);
  const auto up = std::get<takion::FeedbackTouchEvent>(delta.value()[0]);
  EXPECT_FALSE(up.down);
  EXPECT_EQ(up.pointer_id, 3U);
  EXPECT_EQ(up.x, 100U);
  EXPECT_EQ(up.y, 200U);
}

TEST_F(ControllerFeedbackHistory, ReportsANewTouchGoingDown) {
  after.touches[0] = {2, 50, 60};
  const auto delta =
      remote_play::ControllerSample::history_delta(before, after);
  ASSERT_TRUE(delta);
  ASSERT_EQ(delta.value().size(), 1U);
  const auto down = std::get<takion::FeedbackTouchEvent>(delta.value()[0]);
  EXPECT_TRUE(down.down);
  EXPECT_EQ(down.pointer_id, 2U);
  EXPECT_EQ(down.x, 50U);
  EXPECT_EQ(down.y, 60U);
}

TEST_F(ControllerFeedbackHistory, ReportsAMovedTouchAsDownAtItsNewPlace) {
  before.touches[0] = {2, 50, 60};
  after = before;
  after.touches[0].x = 51;
  const auto delta =
      remote_play::ControllerSample::history_delta(before, after);
  ASSERT_TRUE(delta);
  ASSERT_EQ(delta.value().size(), 1U);
  const auto down = std::get<takion::FeedbackTouchEvent>(delta.value()[0]);
  EXPECT_TRUE(down.down);
  EXPECT_EQ(down.pointer_id, 2U);
  EXPECT_EQ(down.x, 51U);
  const auto unchanged =
      remote_play::ControllerSample::history_delta(after, after);
  ASSERT_TRUE(unchanged);
  EXPECT_TRUE(unchanged.value().empty());
}

TEST_F(ControllerFeedbackHistory, OrdersTouchesThenButtonsThenTriggers) {
  before.touches[0] = {1, 1, 1};
  before.touches[1] = {2, 2, 2};
  before.buttons = (1U << 0) | (1U << 15);
  after.touches[0] = {1, 9, 9};
  after.touches[1] = {4, 8, 8};
  after.buttons = (1U << 1) | (1U << 14);
  after.l2 = 0x7f;
  after.r2 = 0xff;
  const auto delta =
      remote_play::ControllerSample::history_delta(before, after);
  ASSERT_TRUE(delta);
  ASSERT_EQ(delta.value().size(), 8U);
  const auto& events = delta.value();
  EXPECT_TRUE(std::get<takion::FeedbackTouchEvent>(events[0]).down);
  EXPECT_FALSE(std::get<takion::FeedbackTouchEvent>(events[1]).down);
  EXPECT_EQ(std::get<takion::FeedbackDigitalButtonEvent>(events[2]).button,
            takion::FeedbackDigitalButton::cross);
  EXPECT_FALSE(std::get<takion::FeedbackDigitalButtonEvent>(events[2]).pressed);
  EXPECT_EQ(std::get<takion::FeedbackDigitalButtonEvent>(events[3]).button,
            takion::FeedbackDigitalButton::moon);
  EXPECT_TRUE(std::get<takion::FeedbackDigitalButtonEvent>(events[3]).pressed);
  EXPECT_EQ(std::get<takion::FeedbackDigitalButtonEvent>(events[4]).button,
            takion::FeedbackDigitalButton::touchpad);
  EXPECT_TRUE(std::get<takion::FeedbackDigitalButtonEvent>(events[4]).pressed);
  EXPECT_EQ(std::get<takion::FeedbackDigitalButtonEvent>(events[5]).button,
            takion::FeedbackDigitalButton::ps);
  EXPECT_FALSE(std::get<takion::FeedbackDigitalButtonEvent>(events[5]).pressed);
  EXPECT_EQ(std::get<takion::FeedbackAnalogButtonEvent>(events[6]).button,
            takion::FeedbackAnalogButton::l2);
  EXPECT_EQ(std::get<takion::FeedbackAnalogButtonEvent>(events[6]).value,
            0x7fU);
  EXPECT_EQ(std::get<takion::FeedbackAnalogButtonEvent>(events[7]).button,
            takion::FeedbackAnalogButton::r2);
  EXPECT_EQ(std::get<takion::FeedbackAnalogButtonEvent>(events[7]).value,
            0xffU);
}

TEST_F(ControllerFeedbackHistory, RefusesAnInvalidTouchId) {
  before.touches[0].id = -2;
  const auto bad = remote_play::ControllerSample::history_delta(before, after);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error(), remote_play::ControllerHistoryError::invalid_touch_id);
}

TEST_F(ControllerFeedbackHistory, RefusesButtonsAboveTheSixteenKnownBits) {
  before.buttons = 1U << 16;
  const auto bad = remote_play::ControllerSample::history_delta(before, after);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error(),
            remote_play::ControllerHistoryError::invalid_button_mask);
}

TEST_F(ControllerFeedbackHistory, RefusesStatesOfDifferentControllers) {
  after.controller_id = 1;
  const auto bad = remote_play::ControllerSample::history_delta(before, after);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error(),
            remote_play::ControllerHistoryError::controller_id_mismatch);
}

TEST_F(ControllerFeedbackHistory, RefusesAControllerIdAboveThree) {
  after.controller_id = 4;
  const auto bad = remote_play::ControllerSample::history_delta(before, after);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error(),
            remote_play::ControllerHistoryError::invalid_controller_id);
}

}  // namespace
}  // namespace remote_play_test
