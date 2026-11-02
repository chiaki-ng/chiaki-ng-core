/// @file
/// @brief Declares Uuid, a UUID held in its 36-character textual form, as
/// PS's services send and expect them.
#pragma once

#include "src/core/utils/text/hex.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::text {

/// @brief A UUID in the 8-4-4-4-12 hexadecimal form.
///
/// Always holds exactly text_length characters with hyphens at the four
/// separator positions and hex digits elsewhere. The text is kept as given,
/// so a parsed UUID keeps its letter case and two spellings differing only
/// in case compare unequal. The version and variant are not checked when
/// parsing. A plain value; copy freely.
class Uuid {
 public:
  /// @brief The length of the textual form, in characters.
  static constexpr std::size_t text_length = 36;

  /// @brief Reads a UUID in its textual form.
  ///
  /// @param text The candidate, without braces or a `urn:uuid:` prefix.
  /// @return The UUID, or nothing when the text is not 36 characters, a
  ///     hyphen is missing or misplaced, or another character is not a hex
  ///     digit.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<Uuid> {
    if (text.size() != text_length) return std::nullopt;
    for (std::size_t index = 0; index < text.size(); ++index) {
      const bool separator =
          std::ranges::find(separators, index) != separators.end();
      if (separator ? text[index] != '-' : !hex_digit(text[index]))
        return std::nullopt;
    }
    return Uuid{std::string{text}};
  }

  /// @brief Makes a random (version 4, RFC 9562) UUID from random bytes.
  ///
  /// Sets the version and variant bits and writes the result in lower
  /// case.
  ///
  /// @param random 16 bytes from a cryptographic random source.
  /// @return The UUID.
  [[nodiscard]] static auto version4(std::span<const std::byte, 16> random)
      -> Uuid {
    std::array<std::byte, 16> bytes{};
    std::ranges::copy(random, bytes.begin());
    bytes[6] = (bytes[6] & std::byte{0x0F}) | std::byte{0x40};
    bytes[8] = (bytes[8] & std::byte{0x3F}) | std::byte{0x80};
    std::string text;
    text.reserve(text_length);
    std::size_t written = 0;
    for (const auto group : groups) {
      if (!text.empty()) text.push_back('-');
      append_hex(text,
                 std::span<const std::byte>{bytes}.subspan(written, group));
      written += group;
    }
    return Uuid{std::move(text)};
  }

  /// @brief Returns the textual form.
  ///
  /// @return The 36 characters; valid while this object lives.
  auto view() const noexcept -> std::string_view { return text_; }

  /// @brief Compares the textual forms exactly, case included.
  ///
  /// @return True when both hold the same text.
  friend auto operator==(const Uuid&, const Uuid&) -> bool = default;

 private:
  /// @brief The character positions that hold hyphens.
  static constexpr std::array<std::size_t, 4> separators{8, 13, 18, 23};
  /// @brief The byte counts of the five hyphen-separated groups.
  static constexpr std::array<std::size_t, 5> groups{4, 2, 2, 2, 6};

  /// @brief Wraps text already known to be well-formed.
  ///
  /// @param text The 36-character textual form.
  explicit Uuid(std::string text) noexcept : text_(std::move(text)) {}

  /// @brief The textual form.
  std::string text_;
};

}  // namespace libchiaking::text
