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

class ControllerMapInput : public ControllerMapTest {};

TEST_F(ControllerMapInput, StartsFromAllZero) {
  const libchiaking::controllers::ControllerState state;
  EXPECT_EQ(state.buttons, 0);
  EXPECT_EQ(state.l2, 0);
  EXPECT_EQ(state.r2, 0);
  EXPECT_EQ(state.left_x, 0);
  EXPECT_EQ(state.left_y, 0);
  EXPECT_EQ(state.right_x, 0);
  EXPECT_EQ(state.right_y, 0);
  EXPECT_EQ(state.touches[0].id, -1);
  EXPECT_EQ(state.touches[1].id, -1);
  EXPECT_EQ(state.touches[0].x, 0);
  EXPECT_EQ(state.touches[0].y, 0);
  const auto mapped = map(libchiaking::net::kakusan::InputState{});
  EXPECT_EQ(mapped.buttons, 0);
  EXPECT_EQ(mapped.l2, 0);
  EXPECT_EQ(mapped.r2, 0);
  EXPECT_EQ(mapped.left_x, 0);
  EXPECT_EQ(mapped.left_y, 0);
  EXPECT_EQ(mapped.right_x, 0);
  EXPECT_EQ(mapped.right_y, 0);
  EXPECT_EQ(mapped.touches[0].id, -1);
  EXPECT_EQ(mapped.touches[1].id, -1);
}

TEST_F(ControllerMapInput, LeavesMotionAtItsDefault) {
  libchiaking::controllers::ControllerState defaults;
  libchiaking::net::kakusan::InputState input;
  input.buttons = libchiaking::net::kakusan::button_mask;
  input.left_trigger = 65535;
  input.touch = libchiaking::net::kakusan::TouchContact{true, 1, 100, 200};
  const auto state = map(input);
  EXPECT_EQ(state.gyro_x, defaults.gyro_x);
  EXPECT_EQ(state.gyro_y, defaults.gyro_y);
  EXPECT_EQ(state.gyro_z, defaults.gyro_z);
  EXPECT_EQ(state.accel_x, defaults.accel_x);
  EXPECT_EQ(state.accel_y, defaults.accel_y);
  EXPECT_EQ(state.accel_z, defaults.accel_z);
  EXPECT_EQ(state.orient_x, defaults.orient_x);
  EXPECT_EQ(state.orient_y, defaults.orient_y);
  EXPECT_EQ(state.orient_z, defaults.orient_z);
  EXPECT_EQ(state.orient_w, defaults.orient_w);
  EXPECT_EQ(state.touches[1].id, -1);
}

TEST_F(ControllerMapInput, MapsEveryButton) {
  struct Pair {
    Button wire;
    ControllerButton source;
  };
  constexpr std::array<Pair, 16> pairs{{
      {Button::south, ControllerButton::cross},
      {Button::east, ControllerButton::moon},
      {Button::west, ControllerButton::box},
      {Button::north, ControllerButton::pyramid},
      {Button::dpad_up, ControllerButton::dpad_up},
      {Button::dpad_down, ControllerButton::dpad_down},
      {Button::dpad_left, ControllerButton::dpad_left},
      {Button::dpad_right, ControllerButton::dpad_right},
      {Button::l1, ControllerButton::l1},
      {Button::r1, ControllerButton::r1},
      {Button::l3, ControllerButton::l3},
      {Button::r3, ControllerButton::r3},
      {Button::options, ControllerButton::options},
      {Button::share, ControllerButton::share},
      {Button::home, ControllerButton::ps},
      {Button::touchpad, ControllerButton::touchpad},
  }};
  std::uint32_t seen = 0;
  for (const auto& pair : pairs) {
    const auto state = map(with_buttons(bit(pair.wire)));
    EXPECT_EQ(state.buttons, bit(pair.source));
    EXPECT_EQ(state.l2, 0);
    EXPECT_EQ(state.r2, 0);
    seen |= bit(pair.source);
  }
  EXPECT_EQ(seen, 0xffffU);

  const auto everything =
      map(with_buttons(libchiaking::net::kakusan::button_mask));
  EXPECT_EQ(everything.buttons, 0xffffU);
  EXPECT_EQ(everything.l2, 255);
  EXPECT_EQ(everything.r2, 255);
}

TEST_F(ControllerMapInput, DropsButtonsTheHostDoesNotAllow) {
  const auto without_optional = libchiaking::net::kakusan::button_mask &
                                ~libchiaking::net::kakusan::optional_buttons;
  auto state = map(with_buttons(libchiaking::net::kakusan::button_mask),
                   without_optional);
  EXPECT_EQ((state.buttons & bit(ControllerButton::share)), 0);
  EXPECT_EQ((state.buttons & bit(ControllerButton::ps)), 0);
  EXPECT_EQ(state.buttons, (0xffffU & ~bit(ControllerButton::share) &
                            ~bit(ControllerButton::ps)));
  EXPECT_EQ(state.l2, 255);
  EXPECT_EQ(state.r2, 255);

  state = map(with_buttons(bit(Button::share)), without_optional);
  EXPECT_EQ(state.buttons, 0);
  state = map(with_buttons(bit(Button::home)), without_optional);
  EXPECT_EQ(state.buttons, 0);
  state = map(with_buttons(bit(Button::share)),
              without_optional | bit(Button::share));
  EXPECT_EQ(state.buttons, bit(ControllerButton::share));
  state = map(with_buttons(bit(Button::home)),
              without_optional | bit(Button::home));
  EXPECT_EQ(state.buttons, bit(ControllerButton::ps));

  state = map(with_buttons(libchiaking::net::kakusan::button_mask), 0);
  EXPECT_EQ(state.buttons, 0);
  EXPECT_EQ(state.l2, 0);
  EXPECT_EQ(state.r2, 0);
  state = map(with_buttons(bit(Button::l2) | bit(Button::south)),
              libchiaking::net::kakusan::button_mask & ~bit(Button::l2));
  EXPECT_EQ(state.l2, 0);
  EXPECT_EQ(state.buttons, bit(ControllerButton::cross));
  state = map(with_buttons(bit(Button::south)), 0xffffffffU);
  EXPECT_EQ(state.buttons, bit(ControllerButton::cross));
}

TEST_F(ControllerMapInput, ScalesTriggersToOneByte) {
  struct Case {
    std::uint16_t value;
    std::uint8_t expected;
  };
  constexpr std::array<Case, 9> cases{{
      {0, 0},
      {1, 0},
      {128, 0},
      {129, 1},
      {257, 1},
      {32767, 127},
      {32768, 128},
      {65534, 255},
      {65535, 255},
  }};
  for (const auto& entry : cases) {
    libchiaking::net::kakusan::InputState input;
    input.left_trigger = entry.value;
    input.right_trigger = static_cast<std::uint16_t>(65535U - entry.value);
    const auto state = map(input);
    EXPECT_EQ(state.l2, entry.expected);
    const auto mirrored = scale_normalized(
        static_cast<std::uint16_t>(65535U - entry.value), trigger_maximum);
    EXPECT_EQ(state.r2, mirrored);
  }

  std::uint32_t previous = 0;
  for (std::uint32_t value = 0; value <= 65535U; ++value) {
    const auto expected = (value * 255U + 32767U) / 65535U;
    libchiaking::net::kakusan::InputState input;
    input.left_trigger = static_cast<std::uint16_t>(value);
    const auto state = map(input);
    EXPECT_EQ(state.l2, expected);
    EXPECT_TRUE(expected >= previous);
    EXPECT_TRUE(expected <= 255U);
    previous = expected;
  }
}

TEST_F(ControllerMapInput, GivesDigitalTriggersTheMaximum) {
  libchiaking::net::kakusan::InputState input;
  input.buttons = bit(Button::l2);
  auto state = map(input);
  EXPECT_EQ(state.l2, 255);
  EXPECT_EQ(state.r2, 0);
  EXPECT_EQ(state.buttons, 0);

  input.buttons = bit(Button::r2);
  state = map(input);
  EXPECT_EQ(state.l2, 0);
  EXPECT_EQ(state.r2, 255);

  input.buttons = bit(Button::l2) | bit(Button::r2);
  input.left_trigger = 65535;
  input.right_trigger = 12000;
  state = map(input);
  EXPECT_EQ(state.l2, 255);
  EXPECT_EQ(state.r2, 255);

  input.buttons = 0;
  input.left_trigger = 12000;
  input.right_trigger = 65535;
  state = map(input);
  EXPECT_EQ(state.l2, scale_normalized(12000, trigger_maximum));
  EXPECT_EQ(state.r2, 255);
}

TEST_F(ControllerMapInput, PassesSticksThrough) {
  libchiaking::net::kakusan::InputState input;
  input.left_x = -32767;
  input.left_y = 32767;
  input.right_x = 32767;
  input.right_y = -32767;
  auto state = map(input);
  EXPECT_EQ(state.left_x, -32767);
  EXPECT_EQ(state.left_y, 32767);
  EXPECT_EQ(state.right_x, 32767);
  EXPECT_EQ(state.right_y, -32767);

  input = libchiaking::net::kakusan::InputState{};
  input.left_x = 1;
  input.left_y = -1;
  input.right_x = -2;
  input.right_y = 2;
  state = map(input);
  EXPECT_EQ(state.left_x, 1);
  EXPECT_EQ(state.left_y, -1);
  EXPECT_EQ(state.right_x, -2);
  EXPECT_EQ(state.right_y, 2);

  input.left_x = -32768;
  EXPECT_FALSE(to_source_state(input, all_allowed));
}

TEST_F(ControllerMapInput, ScalesTouchToTheTouchpad) {
  libchiaking::net::kakusan::InputState input;
  input.touch = libchiaking::net::kakusan::TouchContact{true, 0, 0, 0};
  auto state = map(input);
  EXPECT_EQ(state.touches[0].id, 0);
  EXPECT_EQ(state.touches[0].x, 0);
  EXPECT_EQ(state.touches[0].y, 0);
  EXPECT_EQ(state.touches[1].id, -1);

  input.touch =
      libchiaking::net::kakusan::TouchContact{true, 127, 65535, 65535};
  state = map(input);
  EXPECT_EQ(state.touches[0].id, 127);
  EXPECT_EQ(state.touches[0].x, touchpad_width - 1);
  EXPECT_EQ(state.touches[0].y, touchpad_height - 1);
  EXPECT_EQ(state.touches[0].x, 1919);
  EXPECT_EQ(state.touches[0].y, 1079);

  input.touch = libchiaking::net::kakusan::TouchContact{true, 9, 32768, 32768};
  state = map(input);
  EXPECT_EQ(state.touches[0].id, 9);
  EXPECT_EQ(state.touches[0].x, 960);
  EXPECT_EQ(state.touches[0].y, 540);

  input.touch = libchiaking::net::kakusan::TouchContact{true, 1, 1, 1};
  state = map(input);
  EXPECT_EQ(state.touches[0].x, 0);
  EXPECT_EQ(state.touches[0].y, 0);
  input.touch = libchiaking::net::kakusan::TouchContact{true, 1, 34, 61};
  state = map(input);
  EXPECT_EQ(state.touches[0].x, 1);
  EXPECT_EQ(state.touches[0].y, 1);

  for (std::uint32_t value = 0; value <= 65535U; value += 17) {
    input.touch = libchiaking::net::kakusan::TouchContact{
        true, 2, static_cast<std::uint16_t>(value),
        static_cast<std::uint16_t>(65535U - value)};
    state = map(input);
    EXPECT_EQ(state.touches[0].x, (value * 1919U + 32767U) / 65535U);
    EXPECT_EQ(state.touches[0].y, ((65535U - value) * 1079U + 32767U) / 65535U);
    EXPECT_TRUE(state.touches[0].x <= 1919);
    EXPECT_TRUE(state.touches[0].y <= 1079);
  }

  input.touch = libchiaking::net::kakusan::TouchContact{};
  state = map(input);
  EXPECT_EQ(state.touches[0].id, -1);
  EXPECT_EQ(state.touches[1].id, -1);
  EXPECT_EQ(state.touches[0].x, 0);
  EXPECT_EQ(state.touches[0].y, 0);

  input.touch = libchiaking::net::kakusan::TouchContact{false, 3, 10, 10};
  EXPECT_FALSE(to_source_state(input, all_allowed));
  input.touch = libchiaking::net::kakusan::TouchContact{true, 128, 10, 10};
  EXPECT_FALSE(to_source_state(input, all_allowed));
}

TEST_F(ControllerMapInput, RefusesUnknownButtonBits) {
  libchiaking::net::kakusan::InputState input;
  input.buttons = 1U << 18U;
  EXPECT_FALSE(to_source_state(input, 0xffffffffU));
  input.buttons = 0x80000000U;
  EXPECT_FALSE(to_source_state(input, 0xffffffffU));
}

}  // namespace
}  // namespace kakusan_test
