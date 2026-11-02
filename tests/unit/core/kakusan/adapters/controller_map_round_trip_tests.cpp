#include "src/core/kakusan/adapters/controller_map.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/controller_map_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace kakusan_test {
namespace {

using namespace libchiaking::kakusan;
using namespace libchiaking::net::kakusan;
using libchiaking::controllers::ControllerButton;
using libchiaking::net::kakusan::Button;

class ControllerMapRoundTrip : public ControllerMapTest {};

TEST_F(ControllerMapRoundTrip, KeepsEverySourceButton) {
  std::uint32_t seen = 0;
  for (std::uint32_t index = 0; index < 16; ++index) {
    libchiaking::controllers::ControllerState state;
    state.buttons = 1U << index;
    const auto input = from_source_state(state);
    EXPECT_TRUE(
        libchiaking::net::kakusan::ControllerFrameCodec::validate_input_state(
            input));
    EXPECT_NE(input.buttons, 0);
    EXPECT_EQ((input.buttons & (input.buttons - 1)), 0);
    EXPECT_EQ((input.buttons & (bit(Button::l2) | bit(Button::r2))), 0);
    seen |= input.buttons;
    EXPECT_EQ(round_trip(state).buttons, state.buttons);
  }
  EXPECT_EQ(seen, 0xffffU);
  EXPECT_EQ(
      from_source_state(libchiaking::controllers::ControllerState{}).buttons,
      0);

  libchiaking::controllers::ControllerState everything;
  everything.buttons = 0xffffffffU;
  const auto input = from_source_state(everything);
  EXPECT_EQ(input.buttons, 0xffffU);
  EXPECT_EQ(round_trip(everything).buttons, 0xffffU);
  EXPECT_EQ(from_source_state(everything).buttons, input.buttons);

  libchiaking::controllers::ControllerState cross;
  cross.buttons = bit(ControllerButton::cross) | bit(ControllerButton::ps);
  EXPECT_EQ(from_source_state(cross).buttons,
            (bit(Button::south) | bit(Button::home)));
}

TEST_F(ControllerMapRoundTrip, KeepsEveryTriggerValue) {
  for (std::uint32_t value = 0; value <= 255U; ++value) {
    libchiaking::controllers::ControllerState state;
    state.l2 = static_cast<std::uint8_t>(value);
    state.r2 = static_cast<std::uint8_t>(255U - value);
    const auto input = from_source_state(state);
    EXPECT_TRUE(
        libchiaking::net::kakusan::ControllerFrameCodec::validate_input_state(
            input));
    EXPECT_EQ(input.left_trigger, value * 257U);
    EXPECT_EQ(input.right_trigger, (255U - value) * 257U);
    EXPECT_EQ(has_bit(input, Button::l2), (value == 255U));
    EXPECT_EQ(has_bit(input, Button::r2), (value == 0U));
    const auto back = round_trip(state);
    EXPECT_EQ(back.l2, state.l2);
    EXPECT_EQ(back.r2, state.r2);
    EXPECT_EQ(back.buttons, 0);
  }
}

TEST_F(ControllerMapRoundTrip, ClampsSticksToTheWireRange) {
  libchiaking::controllers::ControllerState state;
  state.left_x = -32768;
  state.left_y = 32767;
  state.right_x = -32767;
  state.right_y = -32768;
  auto input = from_source_state(state);
  EXPECT_TRUE(
      libchiaking::net::kakusan::ControllerFrameCodec::validate_input_state(
          input));
  EXPECT_EQ(input.left_x, -libchiaking::net::kakusan::stick_limit);
  EXPECT_EQ(input.left_y, libchiaking::net::kakusan::stick_limit);
  EXPECT_EQ(input.right_x, -libchiaking::net::kakusan::stick_limit);
  EXPECT_EQ(input.right_y, -libchiaking::net::kakusan::stick_limit);

  for (std::int32_t value = -32767; value <= 32767; value += 7) {
    state.left_x = static_cast<std::int16_t>(value);
    state.left_y = static_cast<std::int16_t>(-value);
    state.right_x = static_cast<std::int16_t>(value / 2);
    state.right_y = static_cast<std::int16_t>(value);
    const auto back = round_trip(state);
    EXPECT_EQ(back.left_x, state.left_x);
    EXPECT_EQ(back.left_y, state.left_y);
    EXPECT_EQ(back.right_x, state.right_x);
    EXPECT_EQ(back.right_y, state.right_y);
  }
}

TEST_F(ControllerMapRoundTrip, KeepsTouch) {
  libchiaking::controllers::ControllerState state;
  auto input = from_source_state(state);
  EXPECT_EQ(input.touch, libchiaking::net::kakusan::TouchContact{});

  struct Corner {
    std::uint16_t x;
    std::uint16_t y;
    std::uint16_t wire_x;
    std::uint16_t wire_y;
  };
  constexpr std::array<Corner, 4> corners{{
      {0, 0, 0, 0},
      {1919, 0, 65535, 0},
      {0, 1079, 0, 65535},
      {1919, 1079, 65535, 65535},
  }};
  for (const auto& corner : corners) {
    state.touches[0] =
        libchiaking::controllers::ControllerTouch{5, corner.x, corner.y};
    input = from_source_state(state);
    EXPECT_TRUE(
        libchiaking::net::kakusan::ControllerFrameCodec::validate_input_state(
            input));
    EXPECT_TRUE(input.touch.active);
    EXPECT_EQ(input.touch.id, 5);
    EXPECT_EQ(input.touch.x, corner.wire_x);
    EXPECT_EQ(input.touch.y, corner.wire_y);
    const auto back = round_trip(state);
    EXPECT_EQ(back.touches[0].id, 5);
    EXPECT_EQ(back.touches[0].x, corner.x);
    EXPECT_EQ(back.touches[0].y, corner.y);
    EXPECT_EQ(back.touches[1].id, -1);
  }

  for (std::uint32_t x = 0; x < touchpad_width; ++x) {
    state.touches[0] = libchiaking::controllers::ControllerTouch{
        1, static_cast<std::uint16_t>(x),
        static_cast<std::uint16_t>(x % touchpad_height)};
    const auto back = round_trip(state);
    EXPECT_EQ(back.touches[0].x, x);
    EXPECT_EQ(back.touches[0].y, x % touchpad_height);
  }

  state.touches[0] =
      libchiaking::controllers::ControllerTouch{127, 65535, 65535};
  input = from_source_state(state);
  EXPECT_TRUE(
      libchiaking::net::kakusan::ControllerFrameCodec::validate_input_state(
          input));
  EXPECT_EQ(input.touch.id, 127);
  EXPECT_EQ(input.touch.x, 65535);
  EXPECT_EQ(input.touch.y, 65535);

  state.touches[0] = libchiaking::controllers::ControllerTouch{-1, 300, 400};
  state.touches[1] = libchiaking::controllers::ControllerTouch{9, 960, 540};
  input = from_source_state(state);
  EXPECT_TRUE(input.touch.active);
  EXPECT_EQ(input.touch.id, 9);
  auto back = round_trip(state);
  EXPECT_EQ(back.touches[0].id, 9);
  EXPECT_EQ(back.touches[0].x, 960);
  EXPECT_EQ(back.touches[0].y, 540);

  state.touches[0] = libchiaking::controllers::ControllerTouch{3, 10, 20};
  back = round_trip(state);
  EXPECT_EQ(back.touches[0].id, 3);
  EXPECT_EQ(back.touches[0].x, 10);
  EXPECT_EQ(back.touches[0].y, 20);

  state.touches[0] = libchiaking::controllers::ControllerTouch{-7, 10, 20};
  state.touches[1] = libchiaking::controllers::ControllerTouch{-1, 30, 40};
  input = from_source_state(state);
  EXPECT_EQ(input.touch, libchiaking::net::kakusan::TouchContact{});
  EXPECT_TRUE(
      libchiaking::net::kakusan::ControllerFrameCodec::validate_input_state(
          input));
}

TEST_F(ControllerMapRoundTrip, ProducesOnlyValidStates) {
  libchiaking::controllers::ControllerState state;
  state.buttons = 0xffffffffU;
  state.l2 = 255;
  state.r2 = 1;
  state.left_x = -32768;
  state.right_y = -32768;
  state.touches[0] =
      libchiaking::controllers::ControllerTouch{127, 65535, 65535};
  const auto input = from_source_state(state);
  EXPECT_TRUE(
      libchiaking::net::kakusan::ControllerFrameCodec::validate_input_state(
          input));
  EXPECT_EQ(input.buttons, (0xffffU | bit(Button::l2)));
  EXPECT_TRUE(to_source_state(input, all_allowed));
}

}  // namespace
}  // namespace kakusan_test
