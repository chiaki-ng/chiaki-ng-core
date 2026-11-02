/// @file
/// @brief Declares AccessCode, the four-digit code a player types to unlock a
/// console profile.
#pragma once

#include "libchiaking/detail/secret_array.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace libchiaking {

/// @brief Holds a four-digit console code in storage that is wiped when it goes
/// away.
///
/// The same kind of code protects signing in to a console profile
/// (LoginPin::submit) and joining as an extra player (Seats::enter_passcode).
/// An AccessCode can only be made from exactly four ASCII digits. It cannot be
/// copied implicitly: it is moved, and the moved-from storage is wiped. Take
/// care not to copy AccessCode::text into an ordinary string, which would leave
/// the digits in memory.
class AccessCode {
 public:
  /// @brief Number of digits in a code.
  static constexpr std::size_t digits = 4;

  /// @brief Validates the digits a player entered.
  ///
  /// @param text Text to check. The caller remains responsible for wiping its
  ///     own copy.
  /// @return The code, or `std::nullopt` unless `text` is exactly
  ///     AccessCode::digits ASCII digits.
  [[nodiscard]] static auto parse(std::string_view text) noexcept
      -> std::optional<AccessCode> {
    const auto digit = [](char value) { return value >= '0' && value <= '9'; };
    if (text.size() != digits || !std::ranges::all_of(text, digit))
      return std::nullopt;
    AccessCode code;
    std::ranges::copy(text, code.digits_.begin());
    return code;
  }

  /// @brief Makes a deliberate second copy of the code.
  ///
  /// @return A code with the same digits, wiped independently of this one.
  auto clone() const noexcept -> AccessCode {
    AccessCode copy;
    std::ranges::copy(digits_, copy.digits_.begin());
    return copy;
  }

  /// @brief Returns the digits as text.
  ///
  /// @return A view of AccessCode::digits characters, not null-terminated,
  ///     valid only while this code is alive and not moved from.
  auto text() const noexcept -> std::string_view {
    return {digits_.data(), digits};
  }

  /// @brief Returns the digits as the bytes that are sent to the console.
  ///
  /// @return A view valid only while this code is alive and not moved from.
  auto bytes() const noexcept -> std::span<const std::byte, digits> {
    return std::as_bytes(std::span<const char, digits>{digits_.data(), digits});
  }

 private:
  AccessCode() noexcept = default;

  SecretArray<digits, char> digits_;
};

}  // namespace libchiaking

namespace libchiaking::play {

/// @brief Makes AccessCode available under the play namespace.
using AccessCode = libchiaking::AccessCode;

}  // namespace libchiaking::play
