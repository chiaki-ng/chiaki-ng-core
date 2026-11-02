/// @file
/// @brief Declares TitleName, the reading of a game's display name that the
/// catalog uses for demo and trial marks and for loose name matching.
///
/// PS's names carry trademark signs, edition words and inconsistent
/// spacing. LegacyCloudStream matching and the LegacyCloudStream ownership
/// heuristics need several canonical forms of a name; PS5 identity never relies
/// on them and uses names for diagnostics only.
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace libchiaking::catalog {

/// @brief A view of a game's name with the readings the catalog compares.
///
/// TitleName does not own the text: the string it was built from must
/// outlive it. It holds no other state, so it is cheap to build on the spot
/// and safe to use from any thread while the text is not modified.
class TitleName {
 public:
  /// @brief The longest normalized form returned, in bytes.
  static constexpr std::size_t normalized_limit = 255;
  /// @brief The longest squashed form returned, in bytes.
  static constexpr std::size_t squashed_limit = 191;
  /// @brief Lower-case words that name an edition rather than the game,
  /// dropped by without_edition_words.
  static constexpr std::array<std::string_view, 11> edition_words{
      "gold",    "deluxe",  "ultimate", "standard",  "premium", "complete",
      "edition", "digital", "goty",     "legendary", "special"};

  /// @brief Views a name.
  ///
  /// @param raw The name as PS wrote it, in UTF-8. Must outlive this
  ///     object.
  explicit TitleName(std::string_view raw) noexcept : raw_(raw) {}

  /// @brief Whether the name holds a word on its own, ignoring ASCII case.
  ///
  /// The match must be bounded on both sides by the ends of the name or by
  /// a character that is not an ASCII letter or digit, so "demo" matches
  /// "Demo Version" but not "Demolition".
  ///
  /// @param word The word to look for, in any case.
  /// @return True when it appears as a whole word; false for an empty word.
  auto has_word(std::string_view word) const -> bool;
  /// @brief Whether the name marks a demo.
  ///
  /// @return True when "demo" appears as a whole word.
  auto is_demo() const -> bool;
  /// @brief Whether the name marks a trial.
  ///
  /// @return True when "trial" appears as a whole word.
  auto is_trial() const -> bool;
  /// @brief Whether the name marks a digital extra rather than a game.
  ///
  /// An extra is an artbook, a name starting "the art of ", a soundtrack,
  /// a content viewer or bonus content, read case-insensitively and with
  /// spaces ignored. A "master collection" whose name later mentions bonus
  /// content is a playable collection, not an extra.
  ///
  /// @return True for an extra.
  auto is_digital_extra() const -> bool;
  /// @brief The name lower-cased with marks and spacing evened out.
  ///
  /// Drops the trademark, service mark and registered signs, folds runs of
  /// spaces, tabs and line breaks into one space, lower-cases ASCII letters,
  /// trims the ends, and stops at TitleName::normalized_limit bytes.
  /// Non-ASCII letters are kept as they are.
  ///
  /// @return The normalized name; empty for an empty or all-space name.
  auto normalized() const -> std::string;
  /// @brief The ASCII letters and digits of the name, lower-cased and run
  /// together.
  ///
  /// Everything else, including every non-ASCII byte, is dropped. Stops at
  /// TitleName::squashed_limit bytes.
  ///
  /// @return The squashed name; empty when the name has no ASCII letter or
  ///     digit.
  auto squashed() const -> std::string;
  /// @brief The name's words without the edition words, run together.
  ///
  /// The name is lower-cased (ASCII only), split into words at anything
  /// that is not a letter or digit (non-ASCII letters count as letters),
  /// every word in TitleName::edition_words is dropped and the rest are
  /// joined with no separator, so "Game X Deluxe Edition" and "Game X" give
  /// the same text.
  ///
  /// @return The joined words; empty when only edition words remain.
  auto without_edition_words() const -> std::string;

 private:
  /// @brief Decodes one UTF-8 code point and moves past it.
  ///
  /// A byte that does not start a sequence, or a sequence cut short by the
  /// end of the text, is read as one code point equal to that byte.
  /// Continuation bytes are not validated.
  ///
  /// @param text The text to read; at must be inside it.
  /// @param at The byte offset to read from; advanced past the code point.
  /// @return The decoded code point.
  static auto code_point(std::string_view text, std::size_t& at) -> char32_t;
  /// @brief Whether a code point belongs inside a word.
  ///
  /// ASCII letters and digits do. Latin-1 symbols, general punctuation,
  /// currency signs, most letterlike symbols, arrows and technical signs,
  /// CJK punctuation, fullwidth punctuation and emoji do not. Every other
  /// code point, including other letters of any script, does.
  ///
  /// @param value The code point.
  /// @return True when it is part of a word.
  static auto letter_or_number(char32_t value) -> bool;

  /// @brief The name as given; not owned.
  std::string_view raw_;
};

}  // namespace libchiaking::catalog
