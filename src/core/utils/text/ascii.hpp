/// @file
/// @brief Declares the locale-independent ASCII character tests and string
/// helpers that protocol text (HTTP headers, Remote Play messages, PS
/// identifiers) is handled with.
///
/// Unlike `<cctype>` these never consult the C locale and accept any `char`,
/// including bytes above 0x7f, which are never letters, digits or spaces.
#pragma once

#include <compare>
#include <cstddef>
#include <string>
#include <string_view>

namespace libchiaking::text {

/// @brief Tells whether a character is an ASCII lower-case letter.
///
/// @param value Any character.
/// @return True for `a` to `z`.
constexpr auto ascii_lower_letter(char value) noexcept -> bool {
  return value >= 'a' && value <= 'z';
}
/// @brief Tells whether a character is an ASCII upper-case letter.
///
/// @param value Any character.
/// @return True for `A` to `Z`.
constexpr auto ascii_upper_letter(char value) noexcept -> bool {
  return value >= 'A' && value <= 'Z';
}
/// @brief Tells whether a character is an ASCII letter.
///
/// @param value Any character.
/// @return True for `a` to `z` and `A` to `Z`.
constexpr auto ascii_alpha(char value) noexcept -> bool {
  return ascii_lower_letter(value) || ascii_upper_letter(value);
}
/// @brief Lower-cases an ASCII letter.
///
/// @param value Any character.
/// @return The lower-case letter for `A` to `Z`; any other value unchanged.
constexpr auto ascii_lower(char value) noexcept -> char {
  return ascii_upper_letter(value) ? static_cast<char>(value - 'A' + 'a')
                                   : value;
}
/// @brief Upper-cases an ASCII letter.
///
/// @param value Any character.
/// @return The upper-case letter for `a` to `z`; any other value unchanged.
constexpr auto ascii_upper(char value) noexcept -> char {
  return ascii_lower_letter(value) ? static_cast<char>(value - 'a' + 'A')
                                   : value;
}
/// @brief Tells whether a character is an ASCII decimal digit.
///
/// @param value Any character.
/// @return True for `0` to `9`.
constexpr auto ascii_digit(char value) noexcept -> bool {
  return value >= '0' && value <= '9';
}
/// @brief Tells whether a character is an ASCII letter or digit.
///
/// @param value Any character.
/// @return True for `0` to `9`, `a` to `z` and `A` to `Z`.
constexpr auto ascii_alnum(char value) noexcept -> bool {
  return ascii_digit(value) || ascii_alpha(value);
}
/// @brief Tells whether a character is ASCII white space.
///
/// @param value Any character.
/// @return True for space, tab, line feed, vertical tab, form feed and
///     carriage return.
constexpr auto ascii_space(char value) noexcept -> bool {
  return value == ' ' || value == '\t' || value == '\n' || value == '\v' ||
         value == '\f' || value == '\r';
}
/// @brief Tells whether a character is JSON white space, as RFC 8259 defines
/// it.
///
/// @param value Any character.
/// @return True for space, tab, line feed and carriage return; false for
///     vertical tab and form feed, which JSON does not allow between tokens.
constexpr auto json_space(char value) noexcept -> bool {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}
/// @brief Tells whether a character is HTTP optional white space, as RFC 9110
/// defines it.
///
/// @param value Any character.
/// @return True for space and tab only.
constexpr auto http_space(char value) noexcept -> bool {
  return value == ' ' || value == '\t';
}
/// @brief Tells whether a character is an ASCII control character.
///
/// @param value Any character.
/// @return True for bytes 0x00 to 0x1f and 0x7f (DEL).
constexpr auto ascii_control(char value) noexcept -> bool {
  const auto byte = static_cast<unsigned char>(value);
  return byte < 0x20 || byte == 0x7f;
}
/// @brief Tells whether a character is visible ASCII, excluding space.
///
/// @param value Any character.
/// @return True for 0x21 to 0x7e.
constexpr auto ascii_visible(char value) noexcept -> bool {
  return value >= 0x21 && value <= 0x7e;
}
/// @brief Tells whether a character is printable ASCII, including space.
///
/// @param value Any character.
/// @return True for 0x20 to 0x7e.
constexpr auto ascii_printable(char value) noexcept -> bool {
  return value >= 0x20 && value <= 0x7e;
}

/// @brief Compares two strings ignoring ASCII case, like `strcasecmp` but
/// without the locale and with embedded NULs compared as bytes.
///
/// @param left One string.
/// @param right The other string.
/// @return Equivalent when equal ignoring case. Otherwise the order of the
///     lower-cased bytes, as unsigned values, at the first that differ; when
///     one is a prefix of the other, the shorter orders first.
auto casecmp(std::string_view left, std::string_view right) noexcept
    -> std::weak_ordering;
/// @brief Tells whether two strings are equal ignoring ASCII case.
///
/// @param left One string.
/// @param right The other string.
/// @return True when casecmp() finds them equivalent.
auto ieq(std::string_view left, std::string_view right) noexcept -> bool;
/// @brief Cuts a string to what fits in a NUL-terminated C buffer of a
/// given size, without splitting a UTF-8 sequence at the cut.
///
/// @param value The string.
/// @param capacity The buffer size in bytes, counting the terminator.
/// @return At most the first `capacity - 1` bytes of `value`, less any
///     incomplete UTF-8 sequence they end in, or all of it when shorter;
///     empty when `capacity` is zero.
auto truncate(std::string value, std::size_t capacity) -> std::string;
/// @brief Reads a fixed-size field as a C string, stopping at its first NUL.
///
/// @param value The field's bytes.
/// @return The part before the first NUL, or the whole field when it has
///     none; a view into `value`.
auto c_string(std::string_view value) noexcept -> std::string_view;
/// @brief Removes ASCII white space from both ends.
///
/// @param value The text.
/// @return A view into `value` without leading or trailing ascii_space()
///     characters; empty when it is all white space.
auto trim(std::string_view value) noexcept -> std::string_view;
/// @brief Removes HTTP optional white space from both ends.
///
/// @param value The text.
/// @return A view into `value` without leading or trailing spaces and tabs;
///     other white space, such as a vertical tab, is kept.
auto trim_http_space(std::string_view value) noexcept -> std::string_view;

}  // namespace libchiaking::text
