/// @file
/// @brief Declares parse_integer, the strict whole-string integer parser used
/// for numbers in headers, JSON strings and configuration.
#pragma once

#include <charconv>
#include <concepts>
#include <optional>
#include <string_view>
#include <system_error>

namespace libchiaking::text {

/// @brief Parses a whole string as an integer, with `std::from_chars` rules.
///
/// The entire text must be the number: no leading or trailing white space,
/// no leading `+`, and no base prefix such as `0x`. A leading `-` is
/// accepted only for signed types. Locale-independent.
///
/// @tparam T The integer type to parse into.
/// @param text The text to parse.
/// @param base The radix, from 2 to 36.
/// @return The value, or nothing when the text is empty, has any character
///     that is not part of the number, or the value does not fit in `T`.
template <std::integral T>
[[nodiscard]] auto parse_integer(std::string_view text, int base = 10) noexcept
    -> std::optional<T> {
  T value{};
  const auto* const end = text.data() + text.size();
  const auto [stop, error] = std::from_chars(text.data(), end, value, base);
  if (error != std::errc{} || stop != end) return std::nullopt;
  return value;
}

}  // namespace libchiaking::text
