/// @file
/// @brief Declares the library's one hexadecimal encoder and decoder, used
/// for keys, nonces, console identifiers and digests written as text.
#pragma once

#include "libchiaking/detail/secure_zero.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace libchiaking::text {

/// @brief Which letters append_hex() writes for the digits 10 to 15.
enum class HexCase : std::uint8_t { lower, upper };

/// @var HexCase::lower
/// @brief `a` to `f`.
/// @var HexCase::upper
/// @brief `A` to `F`.

/// @brief Reads one hexadecimal digit of either case.
///
/// @param value Any character.
/// @return The digit's value, 0 to 15, or nothing when it is not a hex
///     digit.
[[nodiscard]] constexpr auto hex_digit(char value) noexcept
    -> std::optional<unsigned> {
  if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
  if (value >= 'a' && value <= 'f')
    return static_cast<unsigned>(value - 'a') + 10U;
  if (value >= 'A' && value <= 'F')
    return static_cast<unsigned>(value - 'A') + 10U;
  return std::nullopt;
}

/// @brief Tells whether a character is a lower-case hexadecimal digit, for
/// formats that accept only lower case.
///
/// @param value Any character.
/// @return True for `0` to `9` and `a` to `f`.
constexpr auto lower_hex_digit(char value) noexcept -> bool {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

/// @brief Appends bytes as hexadecimal text, two digits per byte, high
/// nibble first.
///
/// @tparam Output A container of `char` with `push_back`, such as
///     `std::string`.
/// @param output The container to grow by twice the byte count.
/// @param bytes The bytes to encode.
/// @param letters The case of the letter digits.
template <class Output>
void append_hex(Output& output, std::span<const std::byte> bytes,
                HexCase letters = HexCase::lower) {
  const std::string_view digits =
      letters == HexCase::upper ? "0123456789ABCDEF" : "0123456789abcdef";
  for (const auto byte : bytes) {
    const auto value = std::to_integer<unsigned>(byte);
    output.push_back(digits[value >> 4U]);
    output.push_back(digits[value & 0x0FU]);
  }
}

/// @brief Decodes hexadecimal text of exactly the output's length.
///
/// Accepts digits of either case and nothing else; no prefix, separators or
/// white space.
///
/// @param text The text; must be exactly twice the output size.
/// @param output Receives the bytes. On failure at run time it is wiped
///     with memory::secure_zero, so no partial secret is left in it.
/// @return True when the whole text was decoded; false when the length is
///     wrong or a character is not a hex digit.
[[nodiscard]] constexpr auto decode_hex(std::string_view text,
                                        std::span<std::byte> output) noexcept
    -> bool {
  const auto decoded = [text, output]() constexpr noexcept {
    if (text.size() != output.size() * 2U) return false;
    for (std::size_t index = 0; index < output.size(); ++index) {
      const auto high = hex_digit(text[2U * index]);
      const auto low = hex_digit(text[2U * index + 1U]);
      if (!high || !low) return false;
      output[index] = static_cast<std::byte>((*high << 4U) | *low);
    }
    return true;
  }();
  if !consteval {
    if (!decoded) memory::secure_zero(output);
  }
  return decoded;
}

/// @brief Decodes a hexadecimal constant at compile time, such as a fixed
/// key from a protocol specification.
///
/// Text of the wrong length or with a bad digit fails to compile.
///
/// @tparam Size The number of bytes; the text must be twice as long.
/// @param text The hexadecimal text.
/// @return The bytes.
template <std::size_t Size>
consteval auto hex_bytes(std::string_view text) -> std::array<std::byte, Size> {
  std::array<std::byte, Size> bytes{};
  if (!decode_hex(text, bytes)) std::unreachable();
  return bytes;
}

/// @brief Decodes hexadecimal text of any even length into the start of a
/// buffer that may be larger.
///
/// @param text The text; its length must be even.
/// @param output Receives the bytes at its start; must hold at least half
///     the text's length. On failure at run time the whole buffer is wiped
///     with memory::secure_zero.
/// @return The number of bytes written, or nothing when the length is odd,
///     the buffer is too small, or a character is not a hex digit.
[[nodiscard]] constexpr auto decode_hex_into(
    std::string_view text, std::span<std::byte> output) noexcept
    -> std::optional<std::size_t> {
  if (text.size() % 2U == 0 && text.size() / 2U <= output.size() &&
      decode_hex(text, output.first(text.size() / 2U)))
    return text.size() / 2U;
  if !consteval {
    memory::secure_zero(output);
  }
  return std::nullopt;
}

}  // namespace libchiaking::text
