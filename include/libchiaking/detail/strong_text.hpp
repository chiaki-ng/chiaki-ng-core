/// @file
/// @brief Declares StrongText, a piece of text that a rule has admitted.
#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking {

/// @brief A piece of text that a rule has admitted, kept apart from every other
/// kind of text by the type that owns it.
///
/// It is valid from construction: the only way to make one is parse(), and it
/// has no setters. Two values of the same kind compare and order by their
/// bytes.
///
/// @tparam Tag The type the text belongs to, so that texts of different kinds
///     cannot be mixed even when they share a rule.
/// @tparam Rule A type with a static `admits (std::string_view)` that returns
///     true for exactly the texts this kind accepts.
template <class Tag, class Rule>
class StrongText {
 public:
  /// @brief Checks text against the rule and wraps it.
  ///
  /// @param text The candidate text.
  /// @return The admitted text, or nothing when the rule refuses it.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<StrongText> {
    if (!Rule::admits(text)) return std::nullopt;
    return StrongText{text};
  }

  /// @brief The admitted text.
  ///
  /// @return A reference into this value.
  auto text() const noexcept -> const std::string& { return text_; }

  /// @brief Compares the texts byte for byte.
  friend auto operator==(const StrongText&, const StrongText&)
      -> bool = default;
  /// @brief Orders by text, so values can key a map or set.
  friend auto operator<=>(const StrongText&, const StrongText&)
      -> std::strong_ordering = default;
  /// @brief Compares the admitted text with plain text.
  ///
  /// @param strong The admitted text.
  /// @param text The plain text.
  /// @return True when the two are byte for byte equal.
  friend auto operator==(const StrongText& strong, std::string_view text)
      -> bool {
    return strong.text_ == text;
  }

 private:
  explicit StrongText(std::string_view text) : text_(text) {}

  std::string text_;
};

}  // namespace libchiaking
