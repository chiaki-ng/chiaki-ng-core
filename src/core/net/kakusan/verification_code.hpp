/// @file
/// @brief Declares Code, the four-digit verification code each side of a
/// Kakusan pairing reads to the other.
#pragma once

#include "libchiaking/detail/secret_array.hpp"
#include "src/core/net/kakusan/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace libchiaking::platform {
class SecureRandomProvider;
}

namespace libchiaking::net::kakusan {

/// @brief Digits in a verification code.
inline constexpr std::size_t code_size = 4;
/// @brief How many codes there are: 0000 to 9999.
inline constexpr std::uint16_t code_count = 10000;

/// @brief One side's four-digit verification code, read to the other person and
/// entered on their device.
///
/// Held as four ASCII digits in wiped storage. Movable; clone() copies.
class Code {
 public:
  /// @brief Draws tried before draw() gives up on rejection sampling.
  static constexpr std::size_t draw_attempts = 16;

  /// @brief Reads an entered code.
  ///
  /// @param text The text.
  /// @return The code.
  /// @retval Error::pairing_failed Not exactly four ASCII digits.
  static auto parse(std::string_view text) -> Result<Code>;
  /// @brief Writes a number as a code, with leading zeros.
  ///
  /// @param value 0 to 9999.
  /// @return The code.
  /// @retval Error::internal The value is above 9999.
  static auto from_value(std::uint16_t value) -> Result<Code>;
  /// @brief Draws an unbiased random code.
  ///
  /// @param random The random source.
  /// @return The code; nothing when the source failed or 16 draws were all
  ///     rejected.
  static auto draw(platform::SecureRandomProvider& random)
      -> std::optional<Code>;

  /// @brief Copies the code.
  ///
  /// @return The copy.
  auto clone() const noexcept -> Code { return Code{storage_.clone()}; }
  /// @brief The four digits.
  ///
  /// @return A view valid while the code lives.
  auto view() const noexcept -> std::string_view { return {data(), size()}; }
  /// @brief The first digit.
  ///
  /// @return A pointer to four characters, not terminated.
  const char* data() const noexcept { return storage_.data(); }
  /// @brief How many digits.
  ///
  /// @return 4.
  std::size_t size() const noexcept { return storage_.size(); }
  /// @brief Start of the digits.
  ///
  /// @return A pointer to the first.
  const char* begin() const noexcept { return storage_.begin(); }
  /// @brief End of the digits.
  ///
  /// @return A pointer past the last.
  const char* end() const noexcept { return storage_.end(); }

 private:
  /// @brief Holds checked digits.
  ///
  /// @param storage The four digits.
  explicit Code(SecretArray<code_size, char> storage) noexcept
      : storage_(std::move(storage)) {}

  /// @brief The four ASCII digits.
  SecretArray<code_size, char> storage_;
};

}  // namespace libchiaking::net::kakusan
