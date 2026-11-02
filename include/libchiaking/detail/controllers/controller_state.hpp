/// @file
/// @brief Declares the state of one controller and the model it reports:
/// ControllerState, its buttons and touches, ControllerInput and
/// ControllerKind.
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace libchiaking::controllers {

/// @brief The controller model a seat tells the console it is using.
enum class ControllerKind : std::uint8_t {
  /// @brief Do not name a model; the console uses its own default.
  console_default,
  /// @brief A DualShock 4.
  dualshock4,
  /// @brief A DualSense.
  dualsense,
};

/// @brief The digital buttons of a controller, as bits of
/// ControllerState::buttons.
///
/// Combine values with bitwise or. The face buttons are named after their
/// shapes.
enum class ControllerButton : std::uint32_t {
  /// @brief The cross button.
  cross = 1u << 0,
  /// @brief The circle button.
  moon = 1u << 1,
  /// @brief The square button.
  box = 1u << 2,
  /// @brief The triangle button.
  pyramid = 1u << 3,
  /// @brief Directional pad left.
  dpad_left = 1u << 4,
  /// @brief Directional pad right.
  dpad_right = 1u << 5,
  /// @brief Directional pad up.
  dpad_up = 1u << 6,
  /// @brief Directional pad down.
  dpad_down = 1u << 7,
  /// @brief The left shoulder button.
  l1 = 1u << 8,
  /// @brief The right shoulder button.
  r1 = 1u << 9,
  /// @brief Pressing the left stick.
  l3 = 1u << 10,
  /// @brief Pressing the right stick.
  r3 = 1u << 11,
  /// @brief The Options button.
  options = 1u << 12,
  /// @brief The Share or Create button.
  share = 1u << 13,
  /// @brief Pressing the touchpad.
  touchpad = 1u << 14,
  /// @brief The PS button.
  ps = 1u << 15,
};

/// @brief One finger on the touchpad.
struct ControllerTouch {
  /// @brief Identifier of the touch, 0 to 127, or -1 when this slot holds no
  /// finger.
  std::int8_t id{-1};
  /// @brief Horizontal position of the finger on the touchpad.
  std::uint16_t x{};
  /// @brief Vertical position of the finger on the touchpad.
  std::uint16_t y{};
};

/// @brief The complete state of one controller at one moment.
///
/// This is what Controllers::send takes. Every send carries the whole state,
/// not a change, so a button stays pressed for as long as its bit stays set. A
/// default-constructed state is a controller at rest: nothing pressed, sticks
/// centred, no touches, lying flat and still.
struct ControllerState {
  /// @brief Number of buttons defined by ControllerButton.
  static constexpr unsigned button_count = 16;
  /// @brief The bits of buttons that may be set; any other bit makes a state
  /// invalid.
  static constexpr std::uint32_t button_mask = 0xffff;

  /// @brief Pressed buttons, as ControllerButton values combined with bitwise
  /// or.
  std::uint32_t buttons{};
  /// @brief Left trigger, 0 released to 255 fully pressed.
  std::uint8_t l2{};
  /// @brief Right trigger, 0 released to 255 fully pressed.
  std::uint8_t r2{};
  /// @brief Left stick horizontal axis, 0 at the centre.
  std::int16_t left_x{};
  /// @brief Left stick vertical axis, 0 at the centre.
  std::int16_t left_y{};
  /// @brief Right stick horizontal axis, 0 at the centre.
  std::int16_t right_x{};
  /// @brief Right stick vertical axis, 0 at the centre.
  std::int16_t right_y{};
  /// @brief The two touchpad contacts; a slot with identifier -1 is unused.
  std::array<ControllerTouch, 2> touches{};
  /// @brief Angular velocity about the x axis, in radians per second.
  float gyro_x{};
  /// @brief Angular velocity about the y axis, in radians per second.
  float gyro_y{};
  /// @brief Angular velocity about the z axis, in radians per second.
  float gyro_z{};
  /// @brief Acceleration along the x axis, in g.
  float accel_x{};
  /// @brief Acceleration along the y axis, in g; 1 for a controller lying
  /// flat and still.
  float accel_y{1.0F};
  /// @brief Acceleration along the z axis, in g.
  float accel_z{};
  /// @brief Orientation quaternion, x component.
  float orient_x{};
  /// @brief Orientation quaternion, y component.
  float orient_y{};
  /// @brief Orientation quaternion, z component.
  float orient_z{};
  /// @brief Orientation quaternion, w component; 1 with the others at 0 means
  /// no rotation.
  float orient_w{1.0F};
  /// @brief Adaptive trigger effect states the controller reports, left then
  /// right, or empty to let the console work them out.
  ///
  /// Each value is 0 off, 1 feedback standby, 2 feedback active, 3 weapon
  /// standby, 4 weapon pulling, 5 weapon fired, 6 vibration standby, 7
  /// vibration active, or 0xff unknown.
  std::optional<std::array<std::uint8_t, 2>> trigger_states{};
};

/// @brief A ControllerState together with the bookkeeping needed to edit it
/// over time.
///
/// It hands out touch identifiers, and can compare and combine inputs, which a
/// frontend needs when several physical devices feed one seat. It does not talk
/// to a session; pass state() to Controllers::send.
class ControllerInput {
 public:
  /// @brief Touch identifiers wrap to 0 after this value.
  static constexpr std::uint8_t touch_id_mask = 0x7f;
  /// @brief Difference below which two motion readings count as the same in
  /// matches.
  static constexpr float motion_epsilon = 0.0000001F;

  /// @brief Constructs an input at rest.
  ControllerInput() noexcept = default;
  /// @brief Wraps an existing state.
  ///
  /// @param state The state to start from.
  /// @param next_touch_id The identifier the next start_touch will hand out.
  explicit ControllerInput(const ControllerState& state,
                           std::uint8_t next_touch_id = 0) noexcept;

  /// @brief The state held, for reading.
  ///
  /// @return The state held, for reading.
  auto state() const noexcept -> const ControllerState& { return state_; }
  /// @brief The state held, for editing in place.
  ///
  /// @return The state held, for editing in place.
  auto state() noexcept -> ControllerState& { return state_; }
  /// @brief The identifier the next start_touch will hand out.
  ///
  /// @return The identifier the next start_touch will hand out.
  auto next_touch_id() const noexcept -> std::uint8_t { return next_touch_id_; }

  /// @brief Resets to a controller at rest and restarts touch identifiers at
  /// 0.
  void set_idle() noexcept;

  /// @brief Puts a new finger on the touchpad.
  ///
  /// @param x Horizontal position.
  /// @param y Vertical position.
  /// @return The identifier given to the touch, for use with move_touch and
  ///     stop_touch; nothing when both touch slots are in use.
  auto start_touch(std::uint16_t x, std::uint16_t y) noexcept
      -> std::optional<std::uint8_t>;
  /// @brief Lifts a finger from the touchpad.
  ///
  /// @param id The identifier returned by start_touch. An unknown identifier
  ///     is ignored.
  void stop_touch(std::uint8_t id) noexcept;
  /// @brief Moves a finger that is on the touchpad.
  ///
  /// @param id The identifier returned by start_touch. An unknown identifier
  ///     is ignored.
  /// @param x New horizontal position.
  /// @param y New vertical position.
  void move_touch(std::uint8_t id, std::uint16_t x, std::uint16_t y) noexcept;

  /// @brief Reports whether another input would look the same to a game.
  ///
  /// Buttons, triggers, sticks and active touches must be equal. Motion
  /// values may differ by up to motion_epsilon, and the positions of unused
  /// touch slots are ignored.
  ///
  /// @param other The input to compare with.
  /// @return True when the two are equivalent.
  auto matches(const ControllerInput& other) const noexcept -> bool;
  /// @brief Reports whether another input holds exactly the same state, field
  /// for field.
  ///
  /// @param other The input to compare with.
  /// @return True when every field is equal.
  auto identical(const ControllerInput& other) const noexcept -> bool;
  /// @brief Combines this input with another into one, as when two devices
  /// drive the same seat.
  ///
  /// Buttons are united, each trigger takes the larger value, each stick axis
  /// takes the value further from the centre, and each touch slot takes this
  /// input's touch if it has one, otherwise the other's. Motion comes from
  /// this input when it shows any movement, otherwise from the other.
  ///
  /// @param other The input to combine with.
  /// @return The combined input. Its touch identifier counter starts at 0.
  auto merged(const ControllerInput& other) const noexcept -> ControllerInput;

 private:
  static auto outside(float value, float reference) noexcept -> bool;
  static auto larger_magnitude(std::int16_t a, std::int16_t b) noexcept
      -> std::int16_t;
  static auto holds(const ControllerTouch& touch, std::uint8_t id) noexcept
      -> bool;
  static auto in_motion(const ControllerState& state) noexcept -> bool;

  ControllerState state_{};
  std::uint8_t next_touch_id_{};
};

}  // namespace libchiaking::controllers
