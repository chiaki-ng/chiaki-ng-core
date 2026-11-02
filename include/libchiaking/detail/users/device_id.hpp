/// @file
/// @brief Declares DeviceId, the identifier an installation presents to PSN
/// when signing in.
#pragma once

#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/detail/wire_sizes.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace libchiaking::platform {
class SecureRandomProvider;
}

namespace libchiaking::users {

/// @brief The device identifier (DUID) this installation presents to PSN.
///
/// It is sent when a user signs in and is kept with the saved sign-in, so the
/// same id is presented for that account from then on. The text is 48
/// lower-case hexadecimal digits beginning with a fixed prefix. It is held in
/// wiping memory and is duplicated only through clone().
///
/// @see SignedInUsers::sign_in
class DeviceId {
 public:
  /// @brief The digits every device id begins with.
  static constexpr std::string_view prefix = "0000000700410080";
  /// @brief The length of a device id's text, in characters.
  static constexpr std::size_t length = duid_text_length;

  /// @brief Creates a new random device id.
  ///
  /// @param random Source of the 16 random bytes that follow the fixed
  ///     prefix.
  /// @return The id, or nothing when the provider could not supply random
  ///     bytes.
  [[nodiscard]] static auto generate(platform::SecureRandomProvider& random)
      -> std::optional<DeviceId>;
  /// @brief Reads a device id from text, for example one saved earlier.
  ///
  /// @param text 48 hexadecimal digits in either case, beginning with the
  ///     fixed prefix.
  /// @return The id with its digits lower-cased, or nothing when the length,
  ///     the digits or the prefix are wrong.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<DeviceId>;

  /// @brief Makes a deliberate second copy of the id.
  ///
  /// @return An independent copy of the id.
  auto clone() const -> DeviceId { return DeviceId{value_.clone()}; }
  /// @brief The id as text. The view is valid while this object lives and is
  /// not moved from.
  ///
  /// @return The id text, which lives as long as this object.
  auto view() const noexcept -> std::string_view { return value_.view(); }

 private:
  explicit DeviceId(SecretString value) noexcept : value_(std::move(value)) {}

  SecretString value_;
};

}  // namespace libchiaking::users
