/// @file
/// @brief Declares the identifiers of the trophies context: NpCommunicationId
/// for a title's trophy set, TrophyGroupId for a group within a set and
/// TrophyId for one trophy.
#pragma once

#include "libchiaking/detail/strong_text.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::trophies {

/// @brief The id PSN gives a title's trophy set, such as `NPWR12345_00`.
///
/// Four capital letters, five digits, an underscore and two digits. A game
/// released for several consoles may carry one set for each, so a game can
/// have more than one id.
class NpCommunicationId {
 public:
  /// @brief Reads an id.
  ///
  /// @param text The id as PSN writes it.
  /// @return The id, or nothing when the text is not spelled as one.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<NpCommunicationId>;

  /// @brief The id as PSN writes it.
  ///
  /// @return The text, valid while this id lives.
  auto text() const noexcept -> const std::string& { return text_.text(); }

  /// @brief Compares the ids byte for byte.
  friend auto operator==(const NpCommunicationId&, const NpCommunicationId&)
      -> bool = default;
  /// @brief Orders by text, so ids can key a map or set.
  friend auto operator<=>(const NpCommunicationId&, const NpCommunicationId&)
      -> std::strong_ordering = default;
  /// @brief Compares the id with plain text.
  friend auto operator==(const NpCommunicationId& id, std::string_view text)
      -> bool {
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
  using Text = StrongText<NpCommunicationId, Spelled>;

  /// @brief Wraps checked text.
  ///
  /// @param text The id.
  explicit NpCommunicationId(Text text) noexcept : text_(std::move(text)) {}

  /// @brief The id as PSN writes it.
  Text text_;
};

/// @brief The id of a group within a trophy set: `default` for the base
/// game, and a number such as `001` for each added group.
class TrophyGroupId {
 public:
  /// @brief Longest id accepted, in bytes.
  static constexpr std::size_t text_limit = 16;

  /// @brief Reads an id.
  ///
  /// @param text The id as PSN writes it.
  /// @return The id, or nothing when the text is empty, longer than
  ///     text_limit, or holds anything but lower-case letters and digits.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<TrophyGroupId>;
  /// @brief The id of the base game's group.
  ///
  /// @return `default`.
  static auto base_game() -> TrophyGroupId;

  /// @brief The id as PSN writes it.
  ///
  /// @return The text, valid while this id lives.
  auto text() const noexcept -> const std::string& { return text_.text(); }

  /// @brief Compares the ids byte for byte.
  friend auto operator==(const TrophyGroupId&, const TrophyGroupId&)
      -> bool = default;
  /// @brief Orders by text, so ids can key a map or set.
  friend auto operator<=>(const TrophyGroupId&, const TrophyGroupId&)
      -> std::strong_ordering = default;
  /// @brief Compares the id with plain text.
  friend auto operator==(const TrophyGroupId& id, std::string_view text)
      -> bool {
    return id.text_ == text;
  }

 private:
  /// @brief The accepted spelling of a group id.
  struct Spelled {
    /// @brief Checks id text against the accepted spelling.
    ///
    /// @param text The candidate id.
    /// @return True for one to text_limit lower-case letters and digits;
    ///     false otherwise.
    static auto admits(std::string_view text) noexcept -> bool;
  };
  /// @brief The checked text.
  using Text = StrongText<TrophyGroupId, Spelled>;

  /// @brief Wraps checked text.
  ///
  /// @param text The id.
  explicit TrophyGroupId(Text text) noexcept : text_(std::move(text)) {}

  /// @brief The id as PSN writes it.
  Text text_;
};

/// @brief The number of one trophy within its set, counting from 0.
///
/// A set's platinum trophy, when it has one, is usually trophy 0.
class TrophyId {
 public:
  /// @brief Takes a trophy number.
  ///
  /// @param number The number PSN gave.
  /// @return The id, or nothing for a negative number or one beyond
  ///     `UINT32_MAX`.
  [[nodiscard]] static auto of(std::int64_t number) -> std::optional<TrophyId>;

  /// @brief The trophy's number.
  ///
  /// @return The number.
  constexpr auto value() const noexcept -> std::uint32_t { return value_; }

  /// @brief Compares the numbers.
  friend constexpr auto operator==(TrophyId, TrophyId) noexcept
      -> bool = default;
  /// @brief Orders by number, which is the set's own order.
  friend constexpr auto operator<=>(TrophyId, TrophyId) noexcept
      -> std::strong_ordering = default;

 private:
  /// @brief Wraps a checked number.
  ///
  /// @param value The number.
  constexpr explicit TrophyId(std::uint32_t value) noexcept : value_(value) {}

  /// @brief The trophy's number.
  std::uint32_t value_;
};

}  // namespace libchiaking::trophies
