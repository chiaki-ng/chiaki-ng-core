/// @file
/// @brief Declares KeyboardState, the keys a frontend holds down on a keyboard
/// it forwards to the console.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace libchiaking::play {

/// @brief Everything a USB keyboard reports at one moment: its modifier keys,
/// its lock lights and the other keys held down.
///
/// The console reads it the way it reads a USB keyboard plugged into it, so it
/// types into the console's text fields and reaches games that take a keyboard.
/// Each state replaces the previous one, so a frontend sends a new state
/// whenever a key goes down or up, or a lock changes, and an empty state to
/// release everything.
///
/// Keys are USB HID usage codes from the keyboard page, such as 0x04 for A or
/// 0x28 for Return. The modifier keys are not keys here but bits of modifiers.
///
/// @see Session::send_keyboard
class KeyboardState {
 public:
  /// @brief Most keys, other than modifiers, a state can hold down at once.
  static constexpr std::size_t max_keys = 16;
  /// @brief Largest value of the lock-light bits.
  static constexpr std::uint8_t max_leds = 7;

  /// @brief Left Control in modifiers.
  static constexpr std::uint8_t left_control = 0x01;
  /// @brief Left Shift in modifiers.
  static constexpr std::uint8_t left_shift = 0x02;
  /// @brief Left Alt (Option) in modifiers.
  static constexpr std::uint8_t left_alt = 0x04;
  /// @brief Left GUI (Command) in modifiers.
  static constexpr std::uint8_t left_gui = 0x08;
  /// @brief Right Control in modifiers.
  static constexpr std::uint8_t right_control = 0x10;
  /// @brief Right Shift in modifiers.
  static constexpr std::uint8_t right_shift = 0x20;
  /// @brief Right Alt (Option) in modifiers.
  static constexpr std::uint8_t right_alt = 0x40;
  /// @brief Right GUI (Command) in modifiers.
  static constexpr std::uint8_t right_gui = 0x80;

  /// @brief Num Lock in leds.
  static constexpr std::uint8_t num_lock = 0x01;
  /// @brief Caps Lock in leds.
  static constexpr std::uint8_t caps_lock = 0x02;
  /// @brief Scroll Lock in leds.
  static constexpr std::uint8_t scroll_lock = 0x04;

  /// @brief A state with nothing held and every lock off.
  constexpr KeyboardState() noexcept = default;

  /// @brief Makes a state from its parts.
  ///
  /// @param modifiers The modifier keys held, as left_control and the other
  ///     modifier bits.
  /// @param leds The locks that are on, as num_lock, caps_lock and
  ///     scroll_lock.
  /// @param keys The other keys held, in the order they went down. Each is a
  ///     nonzero HID usage code that appears once.
  /// @return The state, or nothing when leds is above max_leds, keys holds
  ///     more than max_keys codes, or a code is zero or repeated.
  [[nodiscard]] static constexpr auto create(
      std::uint8_t modifiers, std::uint8_t leds,
      std::span<const std::uint16_t> keys) noexcept
      -> std::optional<KeyboardState> {
    if (leds > max_leds || keys.size() > max_keys) return std::nullopt;
    KeyboardState state;
    for (const auto key : keys) {
      const auto held = state.keys();
      if (key == 0 || std::find(held.begin(), held.end(), key) != held.end())
        return std::nullopt;
      state.keys_[state.count_++] = key;
    }
    state.modifiers_ = modifiers;
    state.leds_ = leds;
    return state;
  }

  /// @brief The modifier keys held.
  ///
  /// @return The modifier keys held.
  constexpr auto modifiers() const noexcept -> std::uint8_t {
    return modifiers_;
  }
  /// @brief The locks that are on.
  ///
  /// @return The locks that are on.
  constexpr auto leds() const noexcept -> std::uint8_t { return leds_; }
  /// @brief The other keys held, in the order they went down.
  ///
  /// @return The other keys held, in the order they went down.
  constexpr auto keys() const noexcept -> std::span<const std::uint16_t> {
    return {keys_.data(), count_};
  }

  /// @brief Compares modifiers, locks and keys, including the key order.
  friend constexpr auto operator==(const KeyboardState& left,
                                   const KeyboardState& right) noexcept
      -> bool {
    return left.modifiers_ == right.modifiers_ && left.leds_ == right.leds_ &&
           std::ranges::equal(left.keys(), right.keys());
  }

 private:
  std::array<std::uint16_t, max_keys> keys_{};
  std::uint8_t count_{};
  std::uint8_t modifiers_{};
  std::uint8_t leds_{};
};

}  // namespace libchiaking::play
