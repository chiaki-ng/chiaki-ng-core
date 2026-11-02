#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace play_test {
namespace {

namespace controllers = libchiaking::controllers;
using controllers::ControllerState;

class ControllerInput : public ::testing::Test {
 protected:
  static auto idled() -> controllers::ControllerInput {
    ControllerState seeded;
    seeded.buttons = 0x5a5a;
    seeded.l2 = 9;
    seeded.right_y = -4;
    seeded.touches[0] = {3, 1, 2};
    seeded.gyro_z = 4.0F;
    seeded.accel_y = 0.5F;
    seeded.orient_w = 0.25F;
    controllers::ControllerInput input{seeded, 5};
    input.set_idle();
    return input;
  }
};

TEST_F(ControllerInput, GoesIdleWithGravityAndNoTouches) {
  const auto input = idled();
  EXPECT_EQ(input.state().buttons, 0);
  EXPECT_EQ(input.state().l2, 0);
  EXPECT_EQ(input.state().right_y, 0);
  EXPECT_EQ(input.state().touches[0].id, -1);
  EXPECT_EQ(input.state().touches[1].id, -1);
  EXPECT_EQ(input.state().touches[0].x, 0);
  EXPECT_EQ(input.state().touches[1].y, 0);
  EXPECT_EQ(input.state().accel_y, 1.0F);
  EXPECT_EQ(input.state().orient_w, 1.0F);
  EXPECT_EQ(input.state().gyro_z, 0.0F);
  EXPECT_EQ(input.next_touch_id(), 0);
}

TEST_F(ControllerInput, GivesATouchTheNextSevenBitIdAndAFreeSlot) {
  auto input = idled();
  EXPECT_EQ(input.start_touch(10, 20), std::uint8_t{0});
  EXPECT_EQ(input.start_touch(30, 40), std::uint8_t{1});
  EXPECT_FALSE(input.start_touch(50, 60));
  EXPECT_EQ(input.state().touches[1].x, 30);
  EXPECT_EQ(input.state().touches[1].y, 40);
  input.stop_touch(0);
  EXPECT_EQ(input.state().touches[0].id, -1);
  EXPECT_EQ(input.start_touch(1, 2), std::uint8_t{2});
  input.move_touch(0x82, 7, 8);
  EXPECT_EQ(input.state().touches[0].x, 7);
  EXPECT_EQ(input.state().touches[0].y, 8);
  input.stop_touch(0x82);
  EXPECT_EQ(input.state().touches[0].id, 2);

  controllers::ControllerInput wrapping{input.state(), 127};
  wrapping.stop_touch(1);
  EXPECT_EQ(wrapping.start_touch(0, 0), std::uint8_t{127});
  EXPECT_EQ(wrapping.next_touch_id(), 0);
}

TEST_F(ControllerInput, MatchesIgnoringIdleTouchPositionsAndMotionNoise) {
  controllers::ControllerInput a;
  controllers::ControllerInput b;
  EXPECT_TRUE(a.matches(b));
  b.state().touches[0].x = 99;
  EXPECT_TRUE(a.matches(b));
  b.state().touches[0].id = 3;
  EXPECT_FALSE(a.matches(b));
  b.state().touches[0].id = -1;
  b.state().gyro_x = 0.00000005F;
  EXPECT_TRUE(a.matches(b));
  b.state().gyro_x = 0.001F;
  EXPECT_FALSE(a.matches(b));
  b.state().gyro_x = 0.0F;
  b.state().buttons = 1;
  EXPECT_FALSE(a.matches(b));
  b.state().buttons = 0;
  EXPECT_TRUE(a.matches(b));
}

TEST_F(ControllerInput, IsIdenticalOnlyWhenEveryFieldIsEqual) {
  ControllerState base;
  base.buttons = 3;
  base.gyro_x = 1.0F;
  const controllers::ControllerInput a{base};
  EXPECT_TRUE(a.identical(controllers::ControllerInput{base}));
  auto changed = base;
  changed.gyro_x = 0.0F;
  changed.accel_x = controllers::ControllerInput::motion_epsilon / 2.0F;
  auto still = base;
  still.gyro_x = 0.0F;
  EXPECT_TRUE(controllers::ControllerInput{still}.matches(
      controllers::ControllerInput{changed}));
  EXPECT_FALSE(controllers::ControllerInput{still}.identical(
      controllers::ControllerInput{changed}));
  changed = base;
  changed.gyro_x = 1.5F;
  EXPECT_FALSE(a.identical(controllers::ControllerInput{changed}));
  changed = base;
  changed.touches[1].x = 9;
  EXPECT_TRUE(a.matches(controllers::ControllerInput{changed}));
  EXPECT_FALSE(a.identical(controllers::ControllerInput{changed}));
  changed = base;
  EXPECT_TRUE(a.identical(controllers::ControllerInput{changed}));
  changed = base;
  changed.r2 = 1;
  EXPECT_FALSE(a.identical(controllers::ControllerInput{changed}));
}

TEST_F(ControllerInput, MergesTwoPadsKeepingTheStrongerInput) {
  controllers::ControllerInput a;
  controllers::ControllerInput b;
  a.state().buttons = 1;
  b.state().buttons = 4;
  a.state().l2 = 10;
  b.state().l2 = 200;
  a.state().left_x = -300;
  b.state().left_x = 200;
  b.state().touches[1] = {5, 11, 0};
  b.state().gyro_y = 2.0F;
  b.state().orient_x = 0.5F;
  const controllers::ControllerInput seeded{a.state(), 9};
  const auto merged = seeded.merged(b);
  const auto& out = merged.state();
  EXPECT_EQ(out.buttons, 5);
  EXPECT_EQ(out.l2, 200);
  EXPECT_EQ(out.left_x, -300);
  EXPECT_EQ(out.touches[1].id, 5);
  EXPECT_EQ(out.touches[1].x, 11);
  EXPECT_EQ(out.touches[0].id, -1);
  EXPECT_EQ(out.gyro_y, 2.0F);
  EXPECT_EQ(out.orient_x, 0.5F);
  EXPECT_EQ(merged.next_touch_id(), 0);

  a.state().gyro_z = 3.0F;
  const auto moving = a.merged(b).state();
  EXPECT_EQ(moving.gyro_z, 3.0F);
  EXPECT_EQ(moving.gyro_y, 0.0F);
  EXPECT_EQ(moving.orient_x, 0.0F);
}

}  // namespace
}  // namespace play_test
