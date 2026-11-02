#include "src/core/kakusan/adapters/controller_map.hpp"

#include <algorithm>
#include <cmath>

namespace libchiaking::kakusan {

auto ControllerMap::analog(std::uint16_t value, bool digital) noexcept
    -> std::uint8_t {
  const auto scaled = scale_normalized(value, trigger_maximum);
  return static_cast<std::uint8_t>(digital ? trigger_maximum : scaled);
}

auto ControllerMap::has(std::uint32_t buttons,
                        net::kakusan::Button button) noexcept -> bool {
  return (buttons & static_cast<std::uint32_t>(button)) != 0;
}

auto ControllerMap::widen(std::uint8_t value) noexcept -> std::uint16_t {
  return static_cast<std::uint16_t>(value * byte_to_word);
}

auto ControllerMap::clamp_axis(std::int16_t value) noexcept -> std::int16_t {
  return std::max(value, static_cast<std::int16_t>(-net::kakusan::stick_limit));
}

auto ControllerMap::normalize(std::uint16_t value,
                              std::uint32_t maximum) noexcept -> std::uint16_t {
  const auto clamped = std::min<std::uint32_t>(value, maximum);
  return static_cast<std::uint16_t>((clamped * axis_u16_max + maximum / 2U) /
                                    maximum);
}

auto ControllerMap::to_source_state(const net::kakusan::InputState& state,
                                    std::uint32_t allowed_buttons)
    -> net::kakusan::Result<controllers::ControllerState> {
  if (!net::kakusan::ControllerFrameCodec::validate_input_state(state))
    return net::kakusan::fail(net::kakusan::Error::protocol);
  controllers::ControllerState output;
  const auto buttons =
      state.buttons & allowed_buttons & net::kakusan::button_mask;
  for (const auto& pair : button_table) {
    if (has(buttons, pair.wire))
      output.buttons |= static_cast<std::uint32_t>(pair.source);
  }
  output.l2 =
      analog(state.left_trigger, has(buttons, net::kakusan::Button::l2));
  output.r2 =
      analog(state.right_trigger, has(buttons, net::kakusan::Button::r2));
  output.left_x = state.left_x;
  output.left_y = state.left_y;
  output.right_x = state.right_x;
  output.right_y = state.right_y;
  if (state.touch.active) {
    output.touches[0].id = static_cast<std::int8_t>(state.touch.id);
    output.touches[0].x = static_cast<std::uint16_t>(
        scale_normalized(state.touch.x, touchpad_width - 1));
    output.touches[0].y = static_cast<std::uint16_t>(
        scale_normalized(state.touch.y, touchpad_height - 1));
  }
  return output;
}

auto ControllerMap::from_source_state(const controllers::ControllerState& state)
    -> net::kakusan::InputState {
  net::kakusan::InputState output;
  for (const auto& pair : button_table) {
    if ((state.buttons & static_cast<std::uint32_t>(pair.source)) != 0)
      output.buttons |= static_cast<std::uint32_t>(pair.wire);
  }
  output.left_trigger = widen(state.l2);
  output.right_trigger = widen(state.r2);
  if (state.l2 == trigger_maximum)
    output.buttons |= static_cast<std::uint32_t>(net::kakusan::Button::l2);
  if (state.r2 == trigger_maximum)
    output.buttons |= static_cast<std::uint32_t>(net::kakusan::Button::r2);
  output.left_x = clamp_axis(state.left_x);
  output.left_y = clamp_axis(state.left_y);
  output.right_x = clamp_axis(state.right_x);
  output.right_y = clamp_axis(state.right_y);
  for (const auto& touch : state.touches) {
    if (touch.id < 0) continue;
    output.touch.active = true;
    output.touch.id = static_cast<std::uint8_t>(touch.id);
    output.touch.x = normalize(touch.x, touchpad_width - 1);
    output.touch.y = normalize(touch.y, touchpad_height - 1);
    break;
  }
  return output;
}

auto ControllerMap::scaled(SourceRumble rumble,
                           controllers::FeedbackIntensity intensity) noexcept
    -> SourceRumble {
  const auto factor = [intensity]() noexcept -> double {
    switch (intensity) {
      case controllers::FeedbackIntensity::off:
        return 0.0;
      case controllers::FeedbackIntensity::strong:
        return 1.0;
      case controllers::FeedbackIntensity::medium:
        return 0.5;
      case controllers::FeedbackIntensity::weak:
        return 0.33;
    }
    return 1.0;
  }();
  const auto scale = [factor](std::uint16_t value) noexcept {
    return static_cast<std::uint16_t>(std::lround(value * factor));
  };
  rumble.low_frequency = scale(rumble.low_frequency);
  rumble.high_frequency = scale(rumble.high_frequency);
  return rumble;
}

auto ControllerMap::rumble_from_feedback(
    const controllers::ControllerFeedback& feedback) noexcept
    -> std::optional<SourceRumble> {
  const auto* rumble = feedback.get<controllers::Rumble>();
  if (rumble == nullptr) return std::nullopt;
  return SourceRumble{feedback.seat, widen(rumble->left), widen(rumble->right)};
}

}  // namespace libchiaking::kakusan
