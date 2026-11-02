/// @file
/// @brief Declares cloud_stream::AccessRoute, how the user may reach a PS5
/// cloud game they asked to launch: owned, through Premium, only by buying it,
/// or not yet known.
///
/// The routes are decided as follows: an owned entitlement launches; an
/// unclaimed Premium game is refused with the exact add-to-library offer; a
/// "for sale" tile is refused with buy_to_stream; unknown ownership is refused
/// until it is rechecked; a bundle asked by its own product is refused with
/// choose_a_game, since only its children launch.
#pragma once

#include <libchiaking/detail/catalog/launch_verdict.hpp>
#include <libchiaking/detail/catalog/my_game.hpp>
#include <libchiaking/detail/catalog/owned_title.hpp>
#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/storefront_answer.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace libchiaking::catalog::cloud_stream {

/// @brief The route by which one PS5 game, or one child of a bundle, may be
/// launched, with the refusal it gives when it may not.
///
/// A route is immutable and holds copies of everything it needs; it may be
/// shared between threads.
class AccessRoute {
 public:
  /// @brief Reads the route of a game for the product the launch asked for.
  ///
  /// When the product names a bundle child, the child decides: owned when
  /// its entitlement is owned, unknown when the game's ownership is
  /// unknown, Premium otherwise. Without ownership a child falls back to
  /// Premium. A bundle asked by any other product is a choice of game,
  /// whether or not the bundle is owned. Otherwise the game decides: owned
  /// when it holds a copy,
  /// unknown when ownership is unknown, else Premium when the plan gives
  /// the right to stream and unowned when it does not.
  ///
  /// @param game The listed game.
  /// @param asked The product the launch asked for; the game's own product
  ///     or a child's.
  /// @return The route.
  static auto of(const MyGame& game, const ProductId& asked) -> AccessRoute;

  /// @brief The entitlement a launch goes through.
  ///
  /// @return The owned entitlement (the child's for a bundle child), or
  ///     std::nullopt when the route is not owned.
  auto owned_entitlement() const noexcept -> const std::optional<OwnershipId>& {
    return owned_;
  }
  /// @brief Whether ownership must be asked again before the launch can be
  /// judged.
  ///
  /// @return True when ownership is unknown.
  auto needs_ownership_check() const noexcept -> bool {
    return kind_ == Kind::unknown;
  }
  /// @brief Whether the launch asked for a bundle itself, which only leads
  /// to choosing one of its games.
  ///
  /// @return True for a bundle asked by its own product.
  auto is_a_choice() const noexcept -> bool {
    return kind_ == Kind::choose_a_game;
  }
  /// @brief The entitlement ids a recheck should ask about.
  ///
  /// @return For a bundle child, its entitlement; nothing for a bundle
  ///     asked by its own product; otherwise the game's owned entitlement
  ///     and every parseable entitlement id of its add-to-library offer,
  ///     without repeats. Possibly empty.
  auto rechecked_by() const noexcept -> const std::vector<OwnershipId>& {
    return rechecked_by_;
  }
  /// @brief The refusal the route gives now.
  ///
  /// @return std::nullopt for an owned route;
  ///     LaunchRefusal::add_to_library with the offer for Premium,
  ///     LaunchRefusal::buy_to_stream for unowned,
  ///     LaunchRefusal::ownership_unknown for unknown,
  ///     LaunchRefusal::choose_a_game for a bundle asked by its own product.
  auto refusal() const -> std::optional<LaunchRefusal>;
  /// @brief The refusal the route gives once the user is known not to own
  /// the game.
  ///
  /// @return LaunchRefusal::add_to_library for a Premium or bundle child
  ///     route, LaunchRefusal::choose_a_game for a bundle asked by its own
  ///     product, LaunchRefusal::buy_to_stream otherwise. Never empty.
  auto refusal_when_not_owned() const -> LaunchRefusal;

 private:
  /// @brief The kinds of route.
  enum class Kind : std::uint8_t {
    /// @brief The user owns the entitlement; the launch goes through it.
    owned,
    /// @brief Premium includes the game but the user has not added it to
    /// the library; refused with the add-to-library offer.
    premium,
    /// @brief Neither owned nor included; the game is for sale only.
    unowned,
    /// @brief PS5 ownership could not be read, so the route is not known.
    unknown,
    /// @brief A bundle asked by its own product; only its children
    /// launch, so one has to be chosen.
    choose_a_game,
  };

  /// @brief Keeps a route.
  ///
  /// @param kind The route now.
  /// @param without_ownership The route once the user is known not to own
  ///     the game; premium, unowned or, for a bundle, choose_a_game.
  /// @param rechecked_by The entitlement ids a recheck asks about.
  AccessRoute(Kind kind, Kind without_ownership,
              std::vector<OwnershipId> rechecked_by) noexcept;

  /// @brief The entitlement ids that can prove a game owned.
  ///
  /// @param game The game.
  /// @return The owned entitlement first, then the offer's parseable
  ///     entitlement ids, without repeats.
  static auto ids_of(const MyGame& game) -> std::vector<OwnershipId>;
  /// @brief The refusal a kind of route gives.
  ///
  /// @param kind The kind.
  /// @return std::nullopt for an owned route, else the matching refusal.
  auto refusal_as(Kind kind) const -> std::optional<LaunchRefusal>;

  /// @brief The route now.
  Kind kind_;
  /// @brief The route once the user is known not to own the game.
  Kind without_ownership_;
  /// @brief The entitlement ids a recheck asks about.
  std::vector<OwnershipId> rechecked_by_;
  /// @brief The owned entitlement; set only for an owned route.
  std::optional<OwnershipId> owned_{};
  /// @brief The game's add-to-library offer, copied from the listing.
  std::optional<StorefrontAnswer::LibraryOffer> offer_{};
};

}  // namespace libchiaking::catalog::cloud_stream
