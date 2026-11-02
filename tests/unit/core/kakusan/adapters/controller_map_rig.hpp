#pragma once

#include "src/core/kakusan/adapters/controller_map.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace kakusan_test {

class ControllerMapTest : public ::testing::Test {
 protected:
  using Button = libchiaking::net::kakusan::Button;
  using ControllerButton = libchiaking::controllers::ControllerButton;
  using ControllerMap = libchiaking::kakusan::ControllerMap;
  using SourceRumble = libchiaking::kakusan::SourceRumble;
  template <class Value>
  using Result = libchiaking::net::kakusan::Result<Value>;

  static constexpr std::uint32_t trigger_maximum =
      ControllerMap::trigger_maximum;

  static constexpr std::uint32_t touchpad_width = ControllerMap::touchpad_width;

  static constexpr std::uint32_t touchpad_height =
      ControllerMap::touchpad_height;

  static constexpr std::uint32_t all_allowed =
      libchiaking::net::kakusan::button_mask;

  static constexpr auto bit(Button button) -> std::uint32_t {
    return static_cast<std::uint32_t>(button);
  }

  static constexpr auto bit(ControllerButton button) -> std::uint32_t {
    return static_cast<std::uint32_t>(button);
  }

  static constexpr auto scale_normalized(std::uint16_t value,
                                         std::uint32_t maximum) noexcept
      -> std::uint32_t {
    return ControllerMap::scale_normalized(value, maximum);
  }

  static auto to_source_state(
      const libchiaking::net::kakusan::InputState& state, std::uint32_t allowed)
      -> Result<libchiaking::controllers::ControllerState> {
    return ControllerMap::to_source_state(state, allowed);
  }

  static auto from_source_state(
      const libchiaking::controllers::ControllerState& state)
      -> libchiaking::net::kakusan::InputState {
    return ControllerMap::from_source_state(state);
  }

  static auto rumble_from_event(
      const libchiaking::controllers::ControllerFeedback& feedback)
      -> std::optional<SourceRumble> {
    return ControllerMap::rumble_from_feedback(feedback);
  }

  static auto seat(std::uint8_t index) -> libchiaking::controllers::SeatNumber {
    return libchiaking::controllers::SeatNumber::create(index).value();
  }

  static auto map(const libchiaking::net::kakusan::InputState& state,
                  std::uint32_t allowed = all_allowed)
      -> libchiaking::controllers::ControllerState {
    const auto mapped = to_source_state(state, allowed);
    EXPECT_TRUE(mapped);
    return mapped.value();
  }

  static auto has_bit(const libchiaking::net::kakusan::InputState& state,
                      Button button) -> bool {
    return (state.buttons & bit(button)) != 0;
  }

  static auto with_buttons(std::uint32_t buttons)
      -> libchiaking::net::kakusan::InputState {
    libchiaking::net::kakusan::InputState state;
    state.buttons = buttons;
    return state;
  }

  static auto rumble(std::uint8_t pad, std::uint8_t left, std::uint8_t right)
      -> libchiaking::controllers::ControllerFeedback {
    return libchiaking::controllers::ControllerFeedback{
        seat(pad), libchiaking::controllers::Rumble{left, right}};
  }

  static auto round_trip(const libchiaking::controllers::ControllerState& state)
      -> libchiaking::controllers::ControllerState {
    const auto input = from_source_state(state);
    EXPECT_TRUE(
        libchiaking::net::kakusan::ControllerFrameCodec::validate_input_state(
            input));
    return map(input);
  }
};

}  // namespace kakusan_test
