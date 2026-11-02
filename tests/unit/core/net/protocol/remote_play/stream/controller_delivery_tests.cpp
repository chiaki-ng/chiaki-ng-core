#include "tests/unit/core/net/protocol/remote_play/stream/controller_delivery_rig.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace remote_play_test {
namespace {

class ControllerDeliveryPosting : public ControllerDeliveryRig {
 protected:
  void press_release_and_move() {
    state.buttons = buttons({Button::cross});
    ASSERT_TRUE(delivery.submit(state, 0));
    ASSERT_EQ(sink.calls.size(), 1U);
    EXPECT_TRUE(call_is(sink, 0, Kind::history));
    ASSERT_TRUE(delivery.submit(state, 1));
    EXPECT_EQ(sink.calls.size(), 1U);
    state.buttons = 0;
    ASSERT_TRUE(delivery.submit(state, 2));
    EXPECT_EQ(sink.calls.size(), 2U);
    EXPECT_TRUE(call_is(sink, sink.calls.size() - 1, Kind::history));
    state.left_x = 1;
    ASSERT_TRUE(delivery.submit(state, 3));
    EXPECT_EQ(sink.calls.size(), 3U);
    EXPECT_TRUE(call_is(sink, sink.calls.size() - 1, Kind::state));
  }

  void refresh_both() {
    ASSERT_NO_FATAL_FAILURE(press_release_and_move());
    ASSERT_TRUE(delivery.refresh(200'002));
    EXPECT_EQ(sink.calls.size(), 4U);
    EXPECT_TRUE(call_is(sink, sink.calls.size() - 1, Kind::history));
    ASSERT_TRUE(delivery.refresh(200'003));
    EXPECT_EQ(sink.calls.size(), 5U);
    EXPECT_TRUE(call_is(sink, sink.calls.size() - 1, Kind::state));
  }

  Sink sink;
  Delivery delivery{sink, 0};
  State state;
};

TEST_F(ControllerDeliveryPosting, PostsEdgesAsHistoryAndMovesAsState) {
  ASSERT_NO_FATAL_FAILURE(press_release_and_move());
}

TEST_F(ControllerDeliveryPosting, RepeatsTheHistoryAndTheStateOnRefresh) {
  ASSERT_NO_FATAL_FAILURE(refresh_both());
}

TEST_F(ControllerDeliveryPosting, RefusesAnInvalidSampleWithoutPosting) {
  ASSERT_NO_FATAL_FAILURE(refresh_both());
  auto invalid = state;
  invalid.controller_id = 4;
  EXPECT_FALSE(delivery.submit(invalid, 4));
  invalid = state;
  invalid.buttons = 1U << 20;
  EXPECT_FALSE(delivery.submit(invalid, 4));
  invalid = state;
  invalid.touches[0].id = -2;
  EXPECT_FALSE(delivery.submit(invalid, 4));
  EXPECT_EQ(sink.calls.size(), 5U);
}

TEST_F(ControllerDeliveryPosting, AcceptsLargeMotion) {
  ASSERT_NO_FATAL_FAILURE(refresh_both());
  auto shaken = state;
  shaken.gyro_x = 31.0F;
  shaken.accel_z = 6.0F;
  EXPECT_TRUE(delivery.submit(shaken, 4));
}

TEST_F(ControllerDeliveryPosting, SanitizesNonFiniteMotion) {
  Sink sanitized_sink;
  Delivery sanitized{sanitized_sink, 0};
  State broken;
  broken.buttons = buttons({Button::cross});
  broken.gyro_x = std::numeric_limits<float>::infinity();
  broken.orient_w = std::numeric_limits<float>::quiet_NaN();
  ASSERT_TRUE(sanitized.submit(broken, 200'000));
  EXPECT_EQ(sanitized_sink.calls.size(), 2U);
  EXPECT_TRUE(call_is(sanitized_sink, 0, Kind::state));
  EXPECT_TRUE(call_is(sanitized_sink, 1, Kind::history));
  EXPECT_EQ(sanitized_sink.last_state.gyro_x, 0.0F);
  EXPECT_EQ(sanitized_sink.last_state.orient_w, 1.0F);
  EXPECT_TRUE(sanitized.submit(broken, 200'001));
  EXPECT_EQ(sanitized_sink.calls.size(), 2U);
}

TEST_F(ControllerDeliveryPosting, RetriesFailedPostsWithTheNextSample) {
  Sink failures;
  Delivery retried{failures, 0};
  State pressed;
  pressed.buttons = buttons({Button::cross});
  failures.fail_history = true;
  failures.fail_state = true;
  EXPECT_TRUE(retried.submit(pressed, 200'010));
  EXPECT_TRUE(failures.calls.empty());
  EXPECT_TRUE(retried.submit(pressed, 200'011));
  EXPECT_EQ(failures.calls.size(), 2U);
  EXPECT_TRUE(call_is(failures, 0, Kind::state));
  EXPECT_TRUE(call_is(failures, 1, Kind::history));
  EXPECT_TRUE(retried.submit(pressed, 200'012));
  EXPECT_EQ(failures.calls.size(), 2U);
}

TEST_F(ControllerDeliveryPosting,
       RefreshesAnIdlePadEveryTwoHundredMilliseconds) {
  Sink idle_sink;
  Delivery idle{idle_sink, 1'000'000};
  EXPECT_TRUE(idle.refresh(1'000'000));
  EXPECT_TRUE(idle_sink.calls.empty());
  EXPECT_TRUE(idle.refresh(1'199'999));
  EXPECT_TRUE(idle_sink.calls.empty());
  EXPECT_TRUE(idle.refresh(1'200'000));
  ASSERT_EQ(idle_sink.calls.size(), 1U);
  EXPECT_TRUE(call_is(idle_sink, 0, Kind::state));
  EXPECT_EQ(idle_sink.calls[0].id, 0U);
  EXPECT_TRUE(idle.refresh(1'399'999));
  EXPECT_EQ(idle_sink.calls.size(), 1U);
  EXPECT_TRUE(idle.refresh(1'400'000));
  EXPECT_EQ(idle_sink.calls.size(), 2U);
}

TEST_F(ControllerDeliveryPosting, PostsAMoveRightAfterAnEarlyPress) {
  Sink early_move_sink;
  Delivery early_move{early_move_sink, 1'000'000};
  State early;
  early.buttons = 1;
  EXPECT_TRUE(early_move.submit(early, 1'000'001));
  EXPECT_EQ(early_move_sink.calls.size(), 1U);
  EXPECT_TRUE(call_is(early_move_sink, 0, Kind::history));
  early.left_y = -4;
  EXPECT_TRUE(early_move.submit(early, 1'000'002));
  EXPECT_EQ(early_move_sink.calls.size(), 2U);
  EXPECT_TRUE(call_is(early_move_sink, 1, Kind::state));
  EXPECT_EQ(early_move_sink.last_state.left_y, -4);
}

}  // namespace
}  // namespace remote_play_test
