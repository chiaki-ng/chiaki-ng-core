/// @file
/// @brief Declares ControllerMap, the translation between the console's
/// controller state and rumble, as the play context speaks them, and the
/// `net::kakusan` wire's input and feedback frames.
#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "libchiaking/detail/play/session_share.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace libchiaking::kakusan {

/// @brief A rumble from the console for one console seat, in the wire's 16-bit
/// motor scale.
///
/// Plain data. Made from controllers::Rumble by
/// ControllerMap::rumble_from_feedback and routed by the host to the guest
/// playing in that seat.
struct SourceRumble {
  /// @brief The console seat the rumble is for.
  controllers::SeatNumber seat{};
  /// @brief Strength of the low-frequency (left) motor, 0 to 65535.
  std::uint16_t low_frequency{};
  /// @brief Strength of the high-frequency (right) motor, 0 to 65535.
  std::uint16_t high_frequency{};

  /// @brief Compares every field.
  /// @return True when seat and both strengths are equal.
  constexpr bool operator==(const SourceRumble&) const noexcept = default;
};

/// @brief Converts controller input and rumble between the play context and the
/// `net::kakusan` wire.
///
/// The wire uses position-named buttons (south, east, ...), 16-bit triggers
/// with separate L2 and R2 digital bits, sticks limited to plus or minus 32767,
/// one touch contact in a 0 to 65535 square, and 16-bit motors. The console
/// side uses PlayStation-named buttons, 8-bit triggers, full 16-bit sticks,
/// touches in touchpad pixels (1920 by 1080) and 8-bit motors.
///
/// Stateless; every member is static and may be called from any thread.
class ControllerMap {
 public:
  /// @brief The console's fully pressed trigger value.
  static constexpr std::uint32_t trigger_maximum = 255;
  /// @brief Width of the console's touchpad, in touch units.
  static constexpr std::uint32_t touchpad_width = 1920;
  /// @brief Height of the console's touchpad, in touch units.
  static constexpr std::uint32_t touchpad_height = 1080;
  /// @brief The largest value of the wire's unsigned 16-bit scales.
  static constexpr std::uint32_t axis_u16_max = 65535;

  /// @brief Scales a wire value from 0 to 65535 into 0 to `maximum`, rounding
  /// to nearest.
  /// @param value The wire value.
  /// @param maximum The top of the target range.
  /// @return The scaled value, at most `maximum`.
  static constexpr auto scale_normalized(std::uint16_t value,
                                         std::uint32_t maximum) noexcept
      -> std::uint32_t {
    return (static_cast<std::uint32_t>(value) * maximum + axis_u16_max / 2U) /
           axis_u16_max;
  }

  /// @brief Turns a guest's wire input into the controller state the console
  /// is sent for their seat.
  ///
  /// Buttons outside `allowed_buttons` are dropped. A trigger whose digital
  /// bit is set is sent fully pressed; otherwise its analog value is scaled
  /// to 0 to
  /// 255. Sticks pass unchanged. An active touch becomes the first touch,
  /// scaled to touchpad units; other touches stay inactive.
  ///
  /// @param state The guest's input.
  /// @param allowed_buttons The wire button mask the host's rules allow.
  /// @return The console's controller state.
  /// @retval net::kakusan::Error::protocol The input fails the wire's own
  ///     validation.
  [[nodiscard]] static auto to_source_state(
      const net::kakusan::InputState& state, std::uint32_t allowed_buttons)
      -> net::kakusan::Result<controllers::ControllerState>;

  /// @brief Turns the guest's local controller state into wire input.
  ///
  /// Triggers are widened to 16 bits, and a fully pressed trigger also sets
  /// its digital bit. Sticks at -32768 are raised to -32767. Only the first
  /// active touch is carried, normalised to 0 to 65535.
  ///
  /// @param state The controller state the guest's frontend sent.
  /// @return The wire input.
  static auto from_source_state(const controllers::ControllerState& state)
      -> net::kakusan::InputState;

  /// @brief Scales a rumble by the console's haptic intensity setting.
  /// @param rumble The rumble at full strength.
  /// @param intensity The setting: off, weak (one third), medium (a half) or
  ///     strong (unchanged).
  /// @return The rumble with both motors scaled and rounded.
  static auto scaled(SourceRumble rumble,
                     controllers::FeedbackIntensity intensity) noexcept
      -> SourceRumble;
  /// @brief Picks a rumble out of the console's controller feedback.
  /// @param feedback The feedback.
  /// @return The rumble for the feedback's seat with both motors widened to
  ///     16 bits, or nothing when the feedback is not a rumble.
  [[nodiscard]] static auto rumble_from_feedback(
      const controllers::ControllerFeedback& feedback) noexcept
      -> std::optional<SourceRumble>;

 private:
  /// @brief One button's name on the wire and on the console.
  struct ButtonPair {
    /// @brief The wire button.
    net::kakusan::Button wire;
    /// @brief The console button.
    controllers::ControllerButton source;
  };

  /// @brief Every button that maps one to one; L2 and R2 are handled with the
  /// triggers.
  static constexpr std::array<ButtonPair, 16> button_table{{
      {net::kakusan::Button::south, controllers::ControllerButton::cross},
      {net::kakusan::Button::east, controllers::ControllerButton::moon},
      {net::kakusan::Button::west, controllers::ControllerButton::box},
      {net::kakusan::Button::north, controllers::ControllerButton::pyramid},
      {net::kakusan::Button::dpad_up, controllers::ControllerButton::dpad_up},
      {net::kakusan::Button::dpad_down,
       controllers::ControllerButton::dpad_down},
      {net::kakusan::Button::dpad_left,
       controllers::ControllerButton::dpad_left},
      {net::kakusan::Button::dpad_right,
       controllers::ControllerButton::dpad_right},
      {net::kakusan::Button::l1, controllers::ControllerButton::l1},
      {net::kakusan::Button::r1, controllers::ControllerButton::r1},
      {net::kakusan::Button::l3, controllers::ControllerButton::l3},
      {net::kakusan::Button::r3, controllers::ControllerButton::r3},
      {net::kakusan::Button::options, controllers::ControllerButton::options},
      {net::kakusan::Button::share, controllers::ControllerButton::share},
      {net::kakusan::Button::home, controllers::ControllerButton::ps},
      {net::kakusan::Button::touchpad, controllers::ControllerButton::touchpad},
  }};
  /// @brief Multiplier that widens 0 to 255 exactly onto 0 to 65535.
  static constexpr std::uint16_t byte_to_word = 257;

  /// @brief The console trigger value for a wire trigger.
  /// @param value The wire's analog value, 0 to 65535.
  /// @param digital Whether the trigger's digital bit is set.
  /// @return 255 when digital, otherwise the scaled value.
  static auto analog(std::uint16_t value, bool digital) noexcept
      -> std::uint8_t;
  /// @brief Tests one wire button in a mask.
  /// @param buttons The mask.
  /// @param button The button.
  /// @return True when it is set.
  static auto has(std::uint32_t buttons, net::kakusan::Button button) noexcept
      -> bool;
  /// @brief Widens an 8-bit value onto the 16-bit scale.
  /// @param value The value.
  /// @return `value` times 257.
  static auto widen(std::uint8_t value) noexcept -> std::uint16_t;
  /// @brief Keeps a stick axis within the wire's limit.
  /// @param value The console's axis value.
  /// @return The value, raised to -32767 if it was lower.
  static auto clamp_axis(std::int16_t value) noexcept -> std::int16_t;
  /// @brief Scales a value from 0 to `maximum` onto 0 to 65535, rounding to
  /// nearest.
  /// @param value The value; anything above `maximum` counts as `maximum`.
  /// @param maximum The top of the source range; must not be zero.
  /// @return The scaled value.
  static auto normalize(std::uint16_t value, std::uint32_t maximum) noexcept
      -> std::uint16_t;
};

}  // namespace libchiaking::kakusan
