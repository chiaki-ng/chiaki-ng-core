/// @file
/// @brief Declares NpTitleId, the id PSN gives one console release of a game.
#pragma once

#include "libchiaking/detail/strong_text.hpp"

#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking {

/// @brief The id PSN gives one console release of a game, such as
/// `PPSA01234_00` or `CUSA01234_00`.
///
/// Four capital letters, five digits, an underscore and two digits. PSN's
/// play history names the games an account played by it, and its trophy
/// service maps it to the game's trophy sets.
class NpTitleId {
 public:
  /// @brief Reads an id.
  ///
  /// @param text The id as PSN writes it.
  /// @return The id, or nothing when the text is not spelled as one.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<NpTitleId>;

  /// @brief The id as PSN writes it.
  ///
  /// @return The text, valid while this id lives.
  auto text() const noexcept -> const std::string& { return text_.text(); }

  /// @brief Compares the ids byte for byte.
  friend auto operator==(const NpTitleId&, const NpTitleId&) -> bool = default;
  /// @brief Orders by text, so ids can key a map or set.
  friend auto operator<=>(const NpTitleId&, const NpTitleId&)
      -> std::strong_ordering = default;
  /// @brief Compares the id with plain text.
  friend auto operator==(const NpTitleId& id, std::string_view text) -> bool {
    return id.text_ == text;
  }

 private:
  /// @brief The accepted spelling of an id.
  struct Spelled {
    /// @brief Checks id text against the accepted spelling.
    ///
    /// @param text The candidate id.
    /// @return True for four capital letters, five digits, an underscore
    ///     and two digits; false otherwise.
    static auto admits(std::string_view text) noexcept -> bool;
  };
  /// @brief The checked text.
  using Text = StrongText<NpTitleId, Spelled>;

  /// @brief Wraps checked text.
  ///
  /// @param text The id.
  explicit NpTitleId(Text text) noexcept : text_(std::move(text)) {}

  /// @brief The id as PSN writes it.
  Text text_;
};

}  // namespace libchiaking
