#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

namespace play_test {
namespace {

namespace controllers = libchiaking::controllers;
using controllers::AccelZero;
using controllers::ControllerState;

class OrientationTracker : public ::testing::Test {
 protected:
  static constexpr float half_root_two = 0.70710677F;

  static auto settled() -> controllers::OrientationTracker {
    controllers::OrientationTracker tracker;
    const auto zero = AccelZero::inactive(false);
    tracker.update({0.1F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, zero, 1000);
    for (std::uint32_t index = 2; index < 64; ++index)
      tracker.update({}, {0.0F, 1.0F, 0.0F}, zero, index * 4000U);
    return tracker;
  }
};

TEST_F(OrientationTracker, StartsLyingFlatWithGravity) {
  const controllers::OrientationTracker tracker;
  EXPECT_LT(std::fabs(tracker.snapshot().orientation.x - half_root_two), 1e-6F);
  EXPECT_LT(std::fabs(tracker.snapshot().orientation.w - half_root_two), 1e-6F);
  EXPECT_EQ(tracker.snapshot().accel.y, 1.0F);
  EXPECT_EQ(tracker.snapshot().sample_index, 0u);
}

TEST_F(OrientationTracker, CountsAndStampsEachSample) {
  controllers::OrientationTracker tracker;
  tracker.update({0.1F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
                 AccelZero::inactive(false), 1000);
  EXPECT_EQ(tracker.snapshot().sample_index, 1u);
  EXPECT_EQ(tracker.snapshot().timestamp_us, 1000u);
  EXPECT_EQ(tracker.snapshot().gyro.x, 0.1F);
  EXPECT_LT(std::fabs(tracker.snapshot().orientation.x - half_root_two), 1e-6F);
}

TEST_F(OrientationTracker, FusesMotionIntoAUnitOrientation) {
  const auto tracker = settled();
  ControllerState state;
  tracker.apply(state);
  EXPECT_EQ(state.accel_y, 1.0F);
  EXPECT_EQ(state.gyro_x, 0.0F);
  const float norm =
      state.orient_x * state.orient_x + state.orient_y * state.orient_y +
      state.orient_z * state.orient_z + state.orient_w * state.orient_w;
  EXPECT_LT(std::fabs(norm - 1.0F), 1e-3F);
}

TEST_F(OrientationTracker, TakesAnEarlierTimestamp) {
  auto tracker = settled();
  tracker.update({}, {0.0F, 1.0F, 0.0F}, AccelZero::inactive(false), 100U);
  EXPECT_EQ(tracker.snapshot().timestamp_us, 100U);
}

TEST_F(OrientationTracker, RemovesTheAccelerometerZero) {
  controllers::OrientationTracker offset;
  offset.update({}, {0.5F, 3.0F, 0.25F},
                AccelZero::active({0.5F, 3.0F, 0.25F}, false), 1);
  EXPECT_EQ(offset.snapshot().accel.x, 0.0F);
  EXPECT_EQ(offset.snapshot().accel.y, 1.0F);
  controllers::OrientationTracker applied;
  applied.update({}, {0.5F, 3.0F, 0.25F}, 1);
  EXPECT_EQ(applied.snapshot().accel.y, 3.0F);
}

TEST_F(OrientationTracker, RestoresFromASnapshotTakenAfterAnEarlierSample) {
  auto tracker = settled();
  ControllerState state;
  tracker.apply(state);
  tracker.update({}, {0.0F, 1.0F, 0.0F}, AccelZero::inactive(false), 100U);
  const controllers::OrientationTracker restored{tracker.snapshot()};
  ControllerState again;
  restored.apply(again);
  EXPECT_EQ(again.orient_x, state.orient_x);
  EXPECT_EQ(again.orient_w, state.orient_w);
}

TEST_F(OrientationTracker, ReportsAnUprightPadAsTheIdentity) {
  const controllers::OrientationTracker upright;
  ControllerState neutral;
  upright.apply(neutral);
  EXPECT_LT(std::fabs(neutral.orient_w - 1.0F), 1e-6F);
  EXPECT_LT(std::fabs(neutral.orient_x), 1e-6F);
}

}  // namespace
}  // namespace play_test
