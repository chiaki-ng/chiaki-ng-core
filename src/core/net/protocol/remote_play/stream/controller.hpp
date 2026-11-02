/// @file
/// @brief The controller seats of a stream, and the controller state a caller
/// feeds into it: buttons, triggers, sticks, touchpad and motion of one
/// DualShock 4 or DualSense.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace libchiaking::net::protocol::remote_play {

/// @brief One of the four controller seats a stream carries.
///
/// A value type, valid from construction.
class Pad {
 public:
  /// @brief The number of seats.
  static constexpr std::size_t count = 4;

  /// @brief The host's seat.
  constexpr Pad() noexcept = default;

  /// @brief A seat by index.
  ///
  /// @param index The index.
  /// @return The seat, or nothing for 4 and above.
  static constexpr auto create(std::size_t index) noexcept
      -> std::optional<Pad> {
    if (index >= count) return std::nullopt;
    return Pad{static_cast<std::uint8_t>(index)};
  }

  /// @brief The seat named by a wire index, keeping its low two bits.
  ///
  /// @param wire_index The index as the console sent it.
  /// @return The seat.
  static constexpr auto masked(std::uint8_t wire_index) noexcept -> Pad {
    return Pad{static_cast<std::uint8_t>(wire_index & (count - 1))};
  }

  /// @brief The host's seat.
  ///
  /// @return Seat 0.
  static constexpr auto host() noexcept -> Pad { return Pad{}; }

  /// @brief The seat's index.
  ///
  /// @return 0 to 3.
  constexpr auto index() const noexcept -> std::uint8_t { return index_; }
  /// @brief Whether it is the host's seat.
  ///
  /// @return True for seat 0.
  constexpr auto primary() const noexcept -> bool { return index_ == 0; }
  /// @brief Whether it is a couch seat.
  ///
  /// @return True for seats 1 to 3.
  constexpr auto couch() const noexcept -> bool { return index_ != 0; }
  /// @brief Compares seats.
  ///
  /// @return True when the same seat.
  constexpr bool operator==(const Pad&) const noexcept = default;

 private:
  /// @brief Makes a seat.
  ///
  /// @param index 0 to 3.
  constexpr explicit Pad(std::uint8_t index) noexcept : index_(index) {}

  /// @brief The seat's index.
  std::uint8_t index_{};
};

/// @brief How many digital buttons ControllerButton names.
inline constexpr unsigned controller_button_count = 16;
/// @brief The bits of ControllerState::buttons that name buttons; any other
/// bit is invalid.
inline constexpr std::uint32_t controller_button_mask = 0xffff;

/// @brief One digital button, as its bit in ControllerState::buttons. The bit
/// number is also the Takion feedback history button code.
enum class ControllerButton : std::uint32_t {
  /// @brief Cross.
  cross = 1u << 0,
  /// @brief Circle.
  moon = 1u << 1,
  /// @brief Square.
  box = 1u << 2,
  /// @brief Triangle.
  pyramid = 1u << 3,
  /// @brief D-pad left.
  dpad_left = 1u << 4,
  /// @brief D-pad right.
  dpad_right = 1u << 5,
  /// @brief D-pad up.
  dpad_up = 1u << 6,
  /// @brief D-pad down.
  dpad_down = 1u << 7,
  /// @brief L1.
  l1 = 1u << 8,
  /// @brief R1.
  r1 = 1u << 9,
  /// @brief L3, the left stick click.
  l3 = 1u << 10,
  /// @brief R3, the right stick click.
  r3 = 1u << 11,
  /// @brief Options.
  options = 1u << 12,
  /// @brief Share on a DualShock 4, Create on a DualSense.
  share = 1u << 13,
  /// @brief The touchpad click.
  touchpad = 1u << 14,
  /// @brief The PS button.
  ps = 1u << 15,
};

/// @brief One finger on the touchpad.
struct ControllerTouch {
  /// @brief The finger's id, 0 to 127, or -1 when no finger is down.
  std::int8_t id{-1};
  /// @brief Horizontal position in touchpad units.
  std::uint16_t x{};
  /// @brief Vertical position in touchpad units.
  std::uint16_t y{};
};

/// @brief Everything one controller is doing at an instant.
///
/// Plain data; defaults to an idle controller at rest (gravity along +y,
/// identity orientation).
struct ControllerState {
  /// @brief The controller's seat, 0 (the host) to 3.
  std::uint8_t controller_id{};
  /// @brief The pressed buttons, as ControllerButton bits.
  std::uint32_t buttons{};
  /// @brief L2 pressure, 0 to 255.
  std::uint8_t l2{};
  /// @brief R2 pressure, 0 to 255.
  std::uint8_t r2{};
  /// @brief Left stick horizontal position, full int16 range.
  std::int16_t left_x{};
  /// @brief Left stick vertical position, full int16 range.
  std::int16_t left_y{};
  /// @brief Right stick horizontal position, full int16 range.
  std::int16_t right_x{};
  /// @brief Right stick vertical position, full int16 range.
  std::int16_t right_y{};
  /// @brief Up to two touchpad fingers.
  std::array<ControllerTouch, 2> touches{};
  /// @brief Angular velocity about x; sent clamped to -30..30.
  float gyro_x{};
  /// @brief Angular velocity about y; sent clamped to -30..30.
  float gyro_y{};
  /// @brief Angular velocity about z; sent clamped to -30..30.
  float gyro_z{};
  /// @brief Acceleration along x in g; sent clamped to -5..5.
  float accel_x{};
  /// @brief Acceleration along y in g; sent clamped to -5..5.
  float accel_y{1.0F};
  /// @brief Acceleration along z in g; sent clamped to -5..5.
  float accel_z{};
  /// @brief Orientation quaternion, x component.
  float orient_x{};
  /// @brief Orientation quaternion, y component.
  float orient_y{};
  /// @brief Orientation quaternion, z component.
  float orient_z{};
  /// @brief Orientation quaternion, w component.
  float orient_w{1.0F};
  /// @brief The left and right adaptive trigger states a DualSense reports,
  /// when known; absent values are sent as zero and flagged emulated.
  std::optional<std::array<std::uint8_t, 2>> trigger_states{};
};

}  // namespace libchiaking::net::protocol::remote_play
