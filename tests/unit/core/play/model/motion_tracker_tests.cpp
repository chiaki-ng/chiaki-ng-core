#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <limits>

namespace play_test {
namespace {

namespace controllers = libchiaking::controllers;
using controllers::ControllerState;
using std::chrono::microseconds;

class MotionTracker : public ::testing::Test {
 protected:
  static auto near(float value, float expected) -> testing::AssertionResult {
    if (std::fabs(value - expected) < 0.02F) return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << value << " is not within 0.02 of " << expected;
  }

  static auto length(const ControllerState& state) -> float {
    return std::sqrt(
        state.orient_x * state.orient_x + state.orient_y * state.orient_y +
        state.orient_z * state.orient_z + state.orient_w * state.orient_w);
  }

  static auto read(const controllers::MotionTracker& tracker)
      -> ControllerState {
    ControllerState state;
    tracker.apply(state);
    return state;
  }

  static void rest(controllers::MotionTracker& tracker, microseconds& time,
                   int samples) {
    for (int index = 0; index < samples; ++index) {
      tracker.accelerometer({0.0F, 1.0F, 0.0F}, time);
      tracker.gyroscope({0.0F, 0.0F, 0.0F}, time);
      time += microseconds{4000};
    }
  }
};

TEST_F(MotionTracker, LeavesTheStateAloneUntilSampled) {
  controllers::MotionTracker tracker;
  EXPECT_FALSE(tracker.sampled());
  tracker.recentre();
  EXPECT_FALSE(tracker.sampled());
  tracker.accelerometer({std::numeric_limits<float>::quiet_NaN(), 1.0F, 0.0F},
                        microseconds{0});
  tracker.gyroscope({0.0F, std::numeric_limits<float>::infinity(), 0.0F},
                    microseconds{0});
  EXPECT_FALSE(tracker.sampled());
  ControllerState state;
  state.gyro_x = 9.0F;
  state.orient_w = 0.25F;
  tracker.apply(state);
  EXPECT_EQ(state.gyro_x, 9.0F);
  EXPECT_EQ(state.orient_w, 0.25F);
}

TEST_F(MotionTracker, IntegratesSamplesAndRecentresToGravity) {
  controllers::MotionTracker tracker;
  microseconds time{1000};
  rest(tracker, time, 400);
  EXPECT_TRUE(tracker.sampled());
  const auto still = read(tracker);
  EXPECT_TRUE(near(still.accel_y, 1.0F));
  EXPECT_TRUE(near(still.gyro_x, 0.0F));
  EXPECT_TRUE(near(length(still), 1.0F));

  for (int index = 0; index < 50; ++index) {
    tracker.gyroscope({0.0F, 3.0F, 0.0F}, time);
    time += microseconds{4000};
  }
  const auto turned = read(tracker);
  EXPECT_TRUE(near(turned.gyro_y, 3.0F));
  EXPECT_TRUE(near(length(turned), 1.0F));
  const float moved = std::fabs(turned.orient_x - still.orient_x) +
                      std::fabs(turned.orient_y - still.orient_y) +
                      std::fabs(turned.orient_z - still.orient_z) +
                      std::fabs(turned.orient_w - still.orient_w);
  EXPECT_GE(moved, 0.05F);

  for (int index = 0; index < 50; ++index) {
    tracker.accelerometer({0.5F, 0.8F, 0.2F}, time);
    tracker.gyroscope({0.0F, 0.0F, 0.0F}, time);
    time += microseconds{4000};
  }
  tracker.recentre();
  const auto centred = read(tracker);
  EXPECT_TRUE(near(centred.accel_x, 0.0F));
  EXPECT_TRUE(near(centred.accel_y, 1.0F));
  EXPECT_TRUE(near(centred.accel_z, 0.0F));
  EXPECT_TRUE(near(length(centred), 1.0F));
  const controllers::OrientationTracker fresh;
  ControllerState expected;
  fresh.apply(expected);
  EXPECT_EQ(centred.orient_w, expected.orient_w);
  EXPECT_EQ(centred.orient_x, expected.orient_x);
  EXPECT_EQ(centred.orient_y, expected.orient_y);
  EXPECT_EQ(centred.orient_z, expected.orient_z);
}

TEST_F(MotionTracker, AppliesOnlyTheMotionFields) {
  controllers::MotionTracker tracker;
  tracker.accelerometer({0.0F, 1.0F, 0.0F}, microseconds{1000});
  tracker.gyroscope({0.25F, 0.0F, 0.0F}, microseconds{5000});
  ControllerState state;
  state.buttons = 0x1234;
  state.l2 = 7;
  state.touches[0] = {4, 10, 20};
  tracker.apply(state);
  EXPECT_EQ(state.buttons, 0x1234);
  EXPECT_EQ(state.l2, 7);
  EXPECT_EQ(state.touches[0].id, 4);
  EXPECT_EQ(state.touches[0].x, 10);
  EXPECT_EQ(state.gyro_x, 0.25F);
}

TEST_F(MotionTracker, TreatsTurnsBelowOneDegreePerSecondAsStill) {
  controllers::MotionTracker tracker;
  microseconds time{1000};
  rest(tracker, time, 400);
  const auto still = read(tracker);
  for (int index = 0; index < 250; ++index) {
    tracker.gyroscope({0.017F, -0.01F, 0.005F}, time);
    time += microseconds{4000};
  }
  const auto creeping = read(tracker);
  EXPECT_EQ(creeping.gyro_x, 0.0F);
  EXPECT_EQ(creeping.gyro_y, 0.0F);
  EXPECT_EQ(creeping.gyro_z, 0.0F);
  EXPECT_EQ(creeping.orient_x, still.orient_x);
  EXPECT_EQ(creeping.orient_y, still.orient_y);
  EXPECT_EQ(creeping.orient_z, still.orient_z);
  EXPECT_EQ(creeping.orient_w, still.orient_w);
  tracker.gyroscope({0.02F, 0.0F, -0.5F}, time);
  const auto turning = read(tracker);
  EXPECT_EQ(turning.gyro_x, 0.02F);
  EXPECT_EQ(turning.gyro_z, -0.5F);

  tracker.set_angular_velocity_deadband(false);
  tracker.gyroscope({0.01F, 0.0F, 0.0F}, time);
  EXPECT_EQ(read(tracker).gyro_x, 0.01F);
}

TEST_F(MotionTracker, TurnsOnTheGyroscopeAloneWithoutTiltCorrection) {
  const auto turn = [](controllers::MotionTracker& tracker) {
    microseconds time{1000};
    rest(tracker, time, 400);
    for (int index = 0; index < 400; ++index) {
      tracker.accelerometer({0.0F, 1.0F, 0.0F}, time);
      tracker.gyroscope({1.0F, 0.0F, 0.0F}, time);
      time += microseconds{4000};
    }
    return read(tracker);
  };
  controllers::MotionTracker corrected;
  controllers::MotionTracker gyro_only;
  gyro_only.set_tilt_correction(false);
  const auto pulled = turn(corrected);
  const auto free = turn(gyro_only);
  EXPECT_TRUE(near(length(pulled), 1.0F));
  EXPECT_TRUE(near(length(free), 1.0F));
  EXPECT_TRUE(!near(pulled.orient_x, free.orient_x) ||
              !near(pulled.orient_y, free.orient_y) ||
              !near(pulled.orient_z, free.orient_z) ||
              !near(pulled.orient_w, free.orient_w));
  gyro_only.recentre();
  gyro_only.gyroscope({1.0F, 0.0F, 0.0F}, microseconds{4000000});
  EXPECT_TRUE(near(length(read(gyro_only)), 1.0F));
}

}  // namespace
}  // namespace play_test
