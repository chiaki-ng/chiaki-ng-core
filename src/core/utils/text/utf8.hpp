/// @file
/// @brief Declares the UTF-8 helpers for text that arrives from PS or a
/// console and is shown or logged: validating, measuring in UTF-16, cutting
/// without splitting a character, encoding a code point and repairing
/// malformed input.
#pragma once

#include <simdutf.h>

#include <array>
#include <cstddef>
#include <string_view>

namespace libchiaking::text::utf8 {

/// @brief Tells whether text is well-formed UTF-8.
///
/// @param text The bytes to check.
/// @return True when every sequence is well-formed, with no overlong forms,
///     surrogates or code points above U+10FFFF.
[[nodiscard]] inline auto valid(std::string_view text) noexcept -> bool {
  return simdutf::validate_utf8(text.data(), text.size());
}

/// @brief Measures well-formed UTF-8 text in UTF-16 code units, as
/// JavaScript's `length` measures a string, so a character outside the Basic
/// Multilingual Plane, such as most emoji, counts twice.
///
/// @param text The text; must be valid().
/// @return The number of UTF-16 code units.
[[nodiscard]] inline auto utf16_length(std::string_view text) noexcept
    -> std::size_t {
  return simdutf::utf16_length_from_utf8(text.data(), text.size());
}

/// @brief Cuts text to at most a number of bytes without splitting a
/// character at the cut.
///
/// Only the end is examined: malformed sequences earlier in the text are
/// kept as they are.
///
/// @param text The text.
/// @param limit The most bytes to keep.
/// @return The whole text when it fits; otherwise its first `limit` bytes,
///     less any incomplete sequence they end in. A view into `text`.
[[nodiscard]] inline auto prefix(std::string_view text,
                                 std::size_t limit) noexcept
    -> std::string_view {
  if (text.size() <= limit) return text;
  return text.substr(0, simdutf::trim_partial_utf8(text.data(), limit));
}

/// @brief What a lead byte promises about the sequence it starts, per the
/// well-formed byte sequence table of the Unicode standard.
struct Sequence {
  /// @brief The total length in bytes, 2 to 4, or 0 when the byte cannot
  /// lead a multi-byte sequence.
  std::size_t length;
  /// @brief The lowest value allowed for the second byte.
  unsigned low;
  /// @brief The highest value allowed for the second byte; later bytes are
  /// always 0x80 to 0xBF.
  unsigned high;
};

/// @brief Says what a lead byte of 0x80 or above requires of the bytes after
/// it.
///
/// The narrowed second-byte ranges exclude overlong forms (after 0xE0 and
/// 0xF0), surrogates (after 0xED) and code points above U+10FFFF (after
/// 0xF4).
///
/// @param lead The byte's value.
/// @return The sequence's length and second-byte range; length 0 for
///     continuation bytes, 0xC0, 0xC1 and 0xF5 to 0xFF.
[[nodiscard]] constexpr auto sequence_after(unsigned lead) noexcept
    -> Sequence {
  if (lead >= 0xC2U && lead <= 0xDFU) return {2, 0x80U, 0xBFU};
  if (lead == 0xE0U) return {3, 0xA0U, 0xBFU};
  if (lead == 0xEDU) return {3, 0x80U, 0x9FU};
  if (lead >= 0xE1U && lead <= 0xEFU) return {3, 0x80U, 0xBFU};
  if (lead == 0xF0U) return {4, 0x90U, 0xBFU};
  if (lead >= 0xF1U && lead <= 0xF3U) return {4, 0x80U, 0xBFU};
  if (lead == 0xF4U) return {4, 0x80U, 0x8FU};
  return {0, 0, 0};
}

/// @brief Appends the UTF-8 encoding of one code point.
///
/// @tparam Output A character container with `append(std::string_view)`.
/// @param output The container to grow by one to four bytes.
/// @param codepoint The code point; a surrogate or a value above U+10FFFF
///     appends nothing.
template <class Output>
void append(Output& output, char32_t codepoint) {
  std::array<char, 4> encoded{};
  const auto length =
      simdutf::convert_utf32_to_utf8(&codepoint, 1, encoded.data());
  output.append(std::string_view{encoded.data(), length});
}

/// @brief Appends text, replacing each malformed part with U+FFFD.
///
/// Well-formed text is appended unchanged. Otherwise each maximal subpart of
/// an ill-formed sequence becomes one replacement character, as the Unicode
/// standard recommends, and every well-formed sequence is kept.
///
/// @tparam Output A character container with `append(std::string_view)` and
///     `push_back(char)`.
/// @param output The container to grow.
/// @param text The text, possibly malformed.
template <class Output>
void append_repaired(Output& output, std::string_view text) {
  if (valid(text)) {
    output.append(text);
    return;
  }
  constexpr std::string_view replacement = "\xEF\xBF\xBD";
  const auto byte = [&text](std::size_t index) {
    return static_cast<unsigned char>(text[index]);
  };
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = byte(index);
    if (lead < 0x80U) {
      output.push_back(text[index++]);
      continue;
    }
    const auto [length, low, high] = sequence_after(lead);
    std::size_t taken = 1;
    while (taken < length && index + taken < text.size()) {
      const auto next = byte(index + taken);
      if (next < (taken == 1 ? low : 0x80U) ||
          next > (taken == 1 ? high : 0xBFU))
        break;
      ++taken;
    }
    if (length != 0 && taken == length)
      output.append(text.substr(index, length));
    else
      output.append(replacement);
    index += taken;
  }
}

}  // namespace libchiaking::text::utf8
