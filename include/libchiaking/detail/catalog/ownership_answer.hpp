/// @file
/// @brief Declares OwnershipAnswer, the result of asking PS again whether a
/// title is owned.
#pragma once

#include "libchiaking/detail/catalog/my_game.hpp"
#include "libchiaking/detail/catalog/owned_title.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace libchiaking::catalog {

/// @brief What a fresh ownership check found for one listed title.
///
/// @see Catalog::recheck_ownership
class OwnershipAnswer {
 public:
  /// @brief The finding.
  enum class Kind : std::uint8_t {
    /// @brief The user holds a copy.
    owned,
    /// @brief No copy was found.
    not_owned,
    /// @brief The record found is not a game.
    not_a_game,
  };

  /// @brief Builds the answer for a title the user owns.
  ///
  /// @param game The game as rebuilt with the copy that was found.
  /// @return The answer.
  static auto owned(MyGame game) -> OwnershipAnswer {
    return OwnershipAnswer{Owned{std::move(game)}};
  }

  /// @brief Builds the answer for a title the user does not own.
  ///
  /// @return The answer, whose kind is OwnershipAnswer::Kind::not_owned.
  static auto not_owned() -> OwnershipAnswer {
    return OwnershipAnswer{NotOwned{}};
  }

  /// @brief Builds the answer for a record that is not a game.
  ///
  /// @param why Why the record does not count as a game.
  /// @return The answer.
  static auto not_a_game(NotAGame why) -> OwnershipAnswer {
    return OwnershipAnswer{why};
  }

  /// @brief The finding.
  ///
  /// @return The kind of this answer.
  auto kind() const noexcept -> Kind {
    return static_cast<Kind>(finding_.index());
  }
  /// @brief Whether the user holds a copy.
  ///
  /// @return True when the kind is OwnershipAnswer::Kind::owned; false
  /// otherwise.
  auto is_owned() const noexcept -> bool { return kind() == Kind::owned; }
  /// @brief The rebuilt game.
  ///
  /// @return The game, or null unless the title is owned. The pointer lives
  ///     as long as this answer.
  auto game() const noexcept -> const MyGame* {
    const auto* owned = std::get_if<Owned>(&finding_);
    return owned != nullptr ? &owned->game : nullptr;
  }
  /// @brief Why the record does not count as a game; set only for
  /// OwnershipAnswer::Kind::not_a_game.
  ///
  /// @return The reason when the kind is OwnershipAnswer::Kind::not_a_game;
  ///     nothing for the other kinds.
  auto why_not_a_game() const noexcept -> std::optional<NotAGame> {
    if (const auto* why = std::get_if<NotAGame>(&finding_)) return *why;
    return std::nullopt;
  }

  /// @brief Compares the finding and its details.
  friend auto operator==(const OwnershipAnswer&, const OwnershipAnswer&)
      -> bool = default;

 private:
  struct Owned {
    MyGame game;
    friend auto operator==(const Owned&, const Owned&) -> bool = default;
  };
  struct NotOwned {
    friend auto operator==(NotOwned, NotOwned) -> bool = default;
  };
  using Finding = std::variant<Owned, NotOwned, NotAGame>;

  static_assert(
      std::is_same_v<std::variant_alternative_t<
                         static_cast<std::size_t>(Kind::owned), Finding>,
                     Owned>);
  static_assert(
      std::is_same_v<std::variant_alternative_t<
                         static_cast<std::size_t>(Kind::not_owned), Finding>,
                     NotOwned>);
  static_assert(
      std::is_same_v<std::variant_alternative_t<
                         static_cast<std::size_t>(Kind::not_a_game), Finding>,
                     NotAGame>);

  explicit OwnershipAnswer(Finding finding) : finding_(std::move(finding)) {}

  Finding finding_;
};

}  // namespace libchiaking::catalog
