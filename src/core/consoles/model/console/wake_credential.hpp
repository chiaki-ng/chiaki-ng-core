/// @file
/// @brief Declares WakeCredential, the number a wake-up datagram proves the
/// sender's pairing with, taken from the pairing's registration key.
#pragma once

#include <libchiaking/detail/secret_array.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace libchiaking::consoles {

/// @brief The `user-credential` a console checks before it leaves rest
/// mode.
///
/// The registration key the console issued at pairing begins with up to
/// eight hexadecimal digits, ended by a zero byte; their value, written in
/// decimal, is the credential. The value is held in a wiping buffer. A
/// plain value; safe to share between threads for reading.
class WakeCredential {
 public:
  /// @brief Most hexadecimal digits a credential may have.
  static constexpr std::size_t maximum_digits = 8;

  /// @brief Holds a credential value.
  ///
  /// @param value The value.
  explicit WakeCredential(std::uint64_t value) noexcept;

  /// @brief Reads the credential from a registration key.
  ///
  /// @param key The registration key; the digits run up to its first zero
  ///     byte or its end.
  /// @return The credential, or nothing when there are no digits, more than
  ///     maximum_digits, or a byte that is not a hexadecimal digit.
  [[nodiscard]] static auto from_registration_key(
      std::span<const std::byte> key) noexcept -> std::optional<WakeCredential>;

  /// @brief The credential's value.
  ///
  /// @return The value.
  auto value() const noexcept -> std::uint64_t;

 private:
  /// @brief The value, stored big-endian.
  SecretArray<sizeof(std::uint64_t)> value_;
};

}  // namespace libchiaking::consoles
