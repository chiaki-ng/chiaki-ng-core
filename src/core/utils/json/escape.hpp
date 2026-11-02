/// @file
/// @brief Declares the library's one JSON string escaper and unescaper, used
/// by the hand-built canonical forms and by secret-holding strings that must
/// not pass through glaze's own buffers.
#pragma once

#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/hex.hpp"
#include "src/core/utils/text/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace libchiaking::json {

/// @brief Appends text as a quoted JSON string with the minimal escaping.
///
/// Escapes `"` and `\`, writes the short escapes for backspace, form feed,
/// line feed, carriage return and tab, and `\u00XX` (lower-case hex) for
/// the other control characters below 0x20. Every other byte, DEL and
/// non-ASCII included, is copied unchanged; UTF-8 is not checked.
///
/// @tparam Output A character container with `push_back(char)` and
///     `append`.
/// @param output The container to grow.
/// @param text The raw string value.
template <class Output>
void append_quoted(Output& output, std::string_view text) {
  output.push_back('"');
  for (const char raw : text) {
    switch (raw) {
      case '"':
        output.append("\\\"");
        break;
      case '\\':
        output.append("\\\\");
        break;
      case '\b':
        output.append("\\b");
        break;
      case '\f':
        output.append("\\f");
        break;
      case '\n':
        output.append("\\n");
        break;
      case '\r':
        output.append("\\r");
        break;
      case '\t':
        output.append("\\t");
        break;
      default:
        if (static_cast<unsigned char>(raw) < 0x20U) {
          output.append("\\u00");
          text::append_hex(output, std::as_bytes(std::span{&raw, 1}));
        } else {
          output.push_back(raw);
        }
    }
  }
  output.push_back('"');
}

/// @brief Decodes the body of a JSON string, between its quotes, and appends
/// the raw value.
///
/// Handles the eight short escapes and `\uXXXX`, joining a high and low
/// surrogate pair into one code point and turning a lone surrogate into
/// U+FFFD. Unescaped bytes, control characters and invalid UTF-8 included,
/// are copied as they are.
///
/// @tparam Output A character container with `push_back(char)` and
///     `append(std::string_view)`.
/// @param output The container to grow; on failure it keeps what was
///     decoded before the bad escape.
/// @param escaped The string body, without the surrounding quotes.
/// @return True when every escape was valid; false on an unknown escape, a
///     `\u` without four hex digits, or a trailing lone backslash.
template <class Output>
[[nodiscard]] auto append_unescaped(Output& output, std::string_view escaped)
    -> bool {
  constexpr char32_t replacement = 0xFFFD;
  constexpr std::string_view names = "\"\\/bfnrt";
  constexpr std::string_view meaning = "\"\\/\b\f\n\r\t";
  const auto code_unit = [&escaped]() -> std::optional<char32_t> {
    if (escaped.size() < 6U || !escaped.starts_with("\\u")) return std::nullopt;
    char32_t unit = 0;
    for (const char digit : escaped.substr(2U, 4U)) {
      const auto nibble = text::hex_digit(digit);
      if (!nibble) return std::nullopt;
      unit = (unit << 4U) | *nibble;
    }
    escaped.remove_prefix(6U);
    return unit;
  };
  while (!escaped.empty()) {
    if (escaped.front() != '\\') {
      output.push_back(escaped.front());
      escaped.remove_prefix(1U);
      continue;
    }
    if (escaped.size() < 2U) return false;
    if (escaped[1] != 'u') {
      const auto found = names.find(escaped[1]);
      if (found == std::string_view::npos) return false;
      output.push_back(meaning[found]);
      escaped.remove_prefix(2U);
      continue;
    }
    const auto unit = code_unit();
    if (!unit) return false;
    char32_t codepoint = *unit;
    if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
      codepoint = replacement;
    } else if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
      const auto resume = escaped;
      const auto low = code_unit();
      if (low && *low >= 0xDC00U && *low <= 0xDFFFU) {
        codepoint =
            0x10000U + ((codepoint - 0xD800U) << 10U) + (*low - 0xDC00U);
      } else {
        escaped = resume;
        codepoint = replacement;
      }
    }
    text::utf8::append(output, codepoint);
  }
  return true;
}

}  // namespace libchiaking::json
