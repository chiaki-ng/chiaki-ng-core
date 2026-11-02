/// @file
/// @brief Declares PercentEncoding, the URL and form percent-encoder and its
/// strict decoder, and the two rule sets it applies.
#pragma once

#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/hex.hpp"

#include <libchiaking/detail/secret_string.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace libchiaking::net::http {

/// @brief Which percent-encoding rule set to apply.
enum class Escape : std::uint8_t {
  /// @brief RFC 3986 URI component: letters, digits and `-_.~` stay, every
  /// other byte becomes `%XX` (upper-case hex). Input stops at the first NUL.
  component,
  /// @brief `application/x-www-form-urlencoded` as browsers write it:
  /// letters,
  /// digits and `-_.*` stay, space becomes `+`, every other byte `%XX`.
  form,
};

/// @brief The library's one percent-encoder, for URL query values, form bodies
/// and log scrubbing, and its one decoder, for credentials read back from a
/// URL.
///
/// Stateless; every member is static and safe to call from any thread.
class PercentEncoding {
 public:
  /// @brief Bytes an escaped byte takes: `%XX`.
  static constexpr std::size_t escape_width = 3;

  /// @brief Size limits on text to decode.
  struct DecodeLimits {
    /// @brief Largest size before decoding, in bytes.
    std::size_t encoded_bytes{};
    /// @brief Largest size after decoding, in bytes.
    std::size_t decoded_bytes{};
  };

  /// @brief Encodes a URI component.
  ///
  /// @param value The text; read up to its first NUL.
  /// @return The encoded text.
  static auto component(std::string_view value) -> std::string;
  /// @brief Encodes a URI component into a bounded size.
  ///
  /// Bytes are encoded whole until the result reaches `capacity` less 4, so
  /// the
  /// result never splits an escape and stays below `capacity`.
  ///
  /// @param value The text; read up to its first NUL.
  /// @param capacity The size of the buffer the result must fit, including a
  ///     terminator.
  /// @return The encoded prefix of `value`; empty when `capacity` is 4 or
  ///     less.
  static auto component_truncated(std::string_view value, std::size_t capacity)
      -> std::string;
  /// @brief Encodes a form field name or value.
  ///
  /// @param value The text, all of it, NULs included.
  /// @return The encoded text.
  static auto form(std::string_view value) -> std::string;

  /// @brief Decodes percent-encoded text strictly, for a value that must be
  /// visible ASCII, such as an authorisation code.
  ///
  /// Not the inverse of Escape::form: `+` is refused rather than read as a
  /// space.
  ///
  /// @param input The encoded text.
  /// @param limits Size limits before and after decoding.
  /// @return The decoded text, in wiping memory; nothing when it is empty or
  ///     too long, holds `+` or a broken escape, or decodes to anything other
  ///     than visible ASCII. Escapes may use either case of hex digit.
  [[nodiscard]] static auto decode_visible(std::string_view input,
                                           DecodeLimits limits)
      -> std::optional<SecretString>;

  /// @brief Appends the encoding of a value to an output.
  ///
  /// @tparam Output A container of `char` with `push_back`, such as
  ///     std::string
  ///     or SecretString.
  /// @param output Where the encoded text goes.
  /// @param value The text; with Escape::component it is read up to its first
  ///     NUL.
  /// @param escape The rule set.
  template <class Output>
  static void append(Output& output, std::string_view value,
                     Escape escape = Escape::component) {
    if (escape == Escape::component) value = text::c_string(value);
    for (const char raw : value) {
      if (unreserved(raw, escape)) {
        output.push_back(raw);
      } else if (escape == Escape::form && raw == ' ') {
        output.push_back('+');
      } else {
        output.push_back('%');
        text::append_hex(output, std::as_bytes(std::span{&raw, 1}),
                         text::HexCase::upper);
      }
    }
  }

 private:
  /// @brief Room component_truncated() keeps free at the end: an escape and a
  /// terminator.
  static constexpr std::size_t escape_reserve = 4;

  /// @brief Whether a byte is written as itself.
  ///
  /// @param value The byte.
  /// @param escape The rule set.
  /// @return True for ASCII letters, digits, `-`, `_` and `.`, plus `~` for
  ///     components or `*` for forms.
  static auto unreserved(char value, Escape escape) noexcept -> bool {
    if (text::ascii_alnum(value) || value == '-' || value == '_' ||
        value == '.')
      return true;
    return escape == Escape::component ? value == '~' : value == '*';
  }
};

}  // namespace libchiaking::net::http
