/// @file
/// @brief Declares ConsoleName, a console's display name together with where it
/// came from.
#pragma once

#include "libchiaking/detail/consoles/firmware.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::consoles {

/// @brief A console's display name, tagged with its source so better names
/// replace worse ones.
///
/// A console can be named by several parties: a placeholder for its generation,
/// PSN, the pairing reply, the console's own search reply, and the user. Each
/// name remembers its Source, and replaces() lets a newly learned name take
/// over only when its source ranks at least as high as the one held.
class ConsoleName {
 public:
  /// @brief Who supplied a name, in ascending order of precedence.
  enum class Source : std::uint8_t {
    /// @brief A placeholder, "PS4" or "PS5", used until a real name is
    /// learned.
    generation,
    /// @brief The name PSN holds for the console on the account.
    account,
    /// @brief The nickname the console stated when it was paired.
    pairing,
    /// @brief The name the console stated when it answered a network
    /// search.
    discovered,
    /// @brief A name chosen by the user through Console::rename. Outranks
    /// all others.
    renamed,
  };

  /// @brief Longest name accepted, in bytes.
  static constexpr std::size_t limit = 255;
  /// @brief Text of the placeholder name of a PS4.
  static constexpr std::string_view ps4_label = "PS4";
  /// @brief Text of the placeholder name of a PS5.
  static constexpr std::string_view ps5_label = "PS5";

  /// @brief Builds a name learned from somewhere other than the generation.
  ///
  /// @param text The name. Kept as given, without trimming.
  /// @param source Who supplied it. Source::generation is refused; use of().
  /// @return The name, or nothing when the source is Source::generation, the
  ///     text is longer than limit, is blank once trimmed, or fails
  ///     printable().
  [[nodiscard]] static auto create(std::string text, Source source)
      -> std::optional<ConsoleName>;
  /// @brief Returns the placeholder name for a generation, with
  /// Source::generation.
  ///
  /// @param generation The console's generation.
  /// @return "PS5" for a PS5 and "PS4" for a PS4.
  static auto of(Generation generation) -> ConsoleName;
  /// @brief Whether text holds no control characters.
  ///
  /// @param text The bytes to examine.
  /// @return False when any byte is below 0x20 or equal to 0x7f. Bytes above
  ///     0x7f pass, so UTF-8 is accepted.
  static auto printable(std::string_view text) noexcept -> bool;

  /// @brief The name as it should be shown.
  ///
  /// @return The name, which lives as long as this object.
  auto text() const noexcept -> const std::string& { return text_; }
  /// @brief Who supplied the name.
  ///
  /// @return The Source that supplied the name.
  auto source() const noexcept -> Source { return source_; }
  /// @brief Whether this name may take the place of the one currently held.
  ///
  /// @param held The name the console has now.
  /// @return True when this name's source ranks at least as high as the held
  ///     name's source. Equal sources replace each other, so a fresher name
  ///     from the same party wins.
  auto replaces(const ConsoleName& held) const noexcept -> bool {
    return source_ >= held.source_;
  }

  /// @brief Compares the text and the source.
  friend auto operator==(const ConsoleName&, const ConsoleName&) noexcept
      -> bool = default;

 private:
  ConsoleName(std::string text, Source source) noexcept
      : text_(std::move(text)), source_(source) {}

  std::string text_;
  Source source_;
};

}  // namespace libchiaking::consoles
