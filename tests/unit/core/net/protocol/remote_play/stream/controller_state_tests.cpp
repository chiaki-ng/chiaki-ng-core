#include "src/core/net/protocol/remote_play/stream/controller.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;

TEST(ControllerStateDefaults, HoldNoButtonsTriggersOrStickTravel) {
  const remote_play::ControllerState state;
  EXPECT_EQ(state.controller_id, 0U);
  EXPECT_EQ(state.buttons, 0U);
  EXPECT_EQ(state.l2, 0U);
  EXPECT_EQ(state.r2, 0U);
  EXPECT_EQ(state.left_x, 0);
  EXPECT_EQ(state.left_y, 0);
  EXPECT_EQ(state.right_x, 0);
  EXPECT_EQ(state.right_y, 0);
}

TEST(ControllerStateDefaults, HoldNoTouches) {
  const remote_play::ControllerState state;
  for (std::size_t index = 0; index < state.touches.size(); ++index) {
    const auto& touch = state.touches[index];
    EXPECT_EQ(touch.id, -1) << "touch " << index;
    EXPECT_EQ(touch.x, 0U) << "touch " << index;
    EXPECT_EQ(touch.y, 0U) << "touch " << index;
  }
}

TEST(ControllerStateDefaults, RestUprightWithGravityOnY) {
  const remote_play::ControllerState state;
  EXPECT_EQ(state.gyro_x, 0.0F);
  EXPECT_EQ(state.gyro_y, 0.0F);
  EXPECT_EQ(state.gyro_z, 0.0F);
  EXPECT_EQ(state.accel_x, 0.0F);
  EXPECT_EQ(state.accel_y, 1.0F);
  EXPECT_EQ(state.accel_z, 0.0F);
  EXPECT_EQ(state.orient_x, 0.0F);
  EXPECT_EQ(state.orient_y, 0.0F);
  EXPECT_EQ(state.orient_z, 0.0F);
  EXPECT_EQ(state.orient_w, 1.0F);
}

TEST(ControllerButtons, EachTakeTheNextBitOfTheLowSixteen) {
  constexpr std::array buttons{remote_play::ControllerButton::cross,
                               remote_play::ControllerButton::moon,
                               remote_play::ControllerButton::box,
                               remote_play::ControllerButton::pyramid,
                               remote_play::ControllerButton::dpad_left,
                               remote_play::ControllerButton::dpad_right,
                               remote_play::ControllerButton::dpad_up,
                               remote_play::ControllerButton::dpad_down,
                               remote_play::ControllerButton::l1,
                               remote_play::ControllerButton::r1,
                               remote_play::ControllerButton::l3,
                               remote_play::ControllerButton::r3,
                               remote_play::ControllerButton::options,
                               remote_play::ControllerButton::share,
                               remote_play::ControllerButton::touchpad,
                               remote_play::ControllerButton::ps};
  std::uint32_t mask{};
  for (std::size_t index = 0; index < buttons.size(); ++index) {
    const auto bit = static_cast<std::uint32_t>(buttons[index]);
    EXPECT_EQ(bit, std::uint32_t{1} << index) << "button " << index;
    mask |= bit;
  }
  EXPECT_EQ(mask, 0xffffU);
}

}  // namespace
}  // namespace remote_play_test
