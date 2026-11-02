/// @file
/// @brief Declares Language, the language code a GK launch specification
/// names, taken from the user's locale.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace libchiaking::psn::gk {

/// @brief The two-letter (or longer) language code of a locale, lower-cased, as
/// the launch specification's `language` wants it; `en` when the locale names
/// none. An immutable value.
class Language {
 public:
  /// @brief Takes the language from a locale.
  ///
  /// Leading blanks are skipped and the text up to the first `-` or `_`, at
  /// most 15 characters, is lower-cased; anything after a NUL is ignored.
  ///
  /// @param locale The locale, such as `en-GB`.
  explicit Language(std::string_view locale);

  /// @brief Reads the code.
  ///
  /// @return The code, never empty.
  auto view() const noexcept -> std::string_view { return code_; }

 private:
  /// @brief The longest code kept, in characters.
  static constexpr std::size_t language_length_limit = 15;

  /// @brief The code.
  std::string code_;
};

}  // namespace libchiaking::psn::gk
