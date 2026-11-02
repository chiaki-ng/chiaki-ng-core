/// @file
/// @brief Declares MyGames, every title one user can see or is denied, with the
/// reasons.
#pragma once

#include "libchiaking/detail/catalog/explanation.hpp"
#include "libchiaking/detail/catalog/library.hpp"
#include "libchiaking/detail/catalog/my_game.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/stream_refusal.hpp"
#include "libchiaking/detail/catalog/streamable_list.hpp"
#include "libchiaking/detail/catalog/wall_time.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/detail/users/membership.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::catalog {

/// @brief Every game considered for one user, split into those shown and those
/// hidden.
///
/// This is the result a library screen renders. It is immutable; views and
/// pointers it hands out are valid while it lives. As built by the Catalog,
/// owned games come first and games are otherwise ordered by name without
/// regard to case.
///
/// @see Catalog::process_library_and_list
class MyGames {
 public:
  /// @brief Wall-clock time in milliseconds.
  using Time = catalog::Time;

  /// @brief Why a set of games is not consistent.
  enum class Invalid : std::uint8_t {
    /// @brief A game's product id is the id or an alias of an earlier game.
    duplicate_product,
    /// @brief A game's ownership contradicts the library state: unknown
    /// although the library was fetched, or not owned although it was
    /// unavailable.
    ownership_knowledge
  };

  /// @brief What a MyGames was computed from, used to tell whether it is
  /// still current.
  struct Inputs {
    /// @brief When the streamable list was built.
    Time list_fetched_at{};
    /// @brief When the library was fetched or last amended.
    Time library_fetched_at{};
    /// @brief When the premium streaming category was fetched; empty when
    /// none was used.
    std::optional<Time> included_fetched_at{};
    /// @brief The user's tier at the time; empty when it was not known.
    std::optional<users::Tier> tier{};
    /// @brief A counter that changes whenever the remembered refusals
    /// change.
    std::uint64_t refusals_revision{};
    /// @brief A counter that changes whenever the remembered used trials
    /// change.
    std::uint64_t used_trials_revision{};
    /// @brief Whether the library could be read.
    LibraryState library{LibraryState::fetched};
    /// @brief Whether the account's PS5 entitlements could be read, which
    /// decides whether ownership of PS5 cloud games is known. PS5
    /// ownership comes from its own source, so it can be
    /// LibraryState::unavailable or LibraryState::partial while the rest
    /// of the library was fetched; it is LibraryState::unavailable
    /// whenever the library itself was not fetched.
    LibraryState ps5_library{LibraryState::fetched};

    /// @brief Compares every input.
    friend auto operator==(const Inputs&, const Inputs&) -> bool = default;
  };

  /// @brief Builds the games without explanations.
  ///
  /// @param account The account the games are for.
  /// @param ordered The games in display order; shown games are moved ahead
  ///     of hidden ones, each group keeping its order.
  /// @param inputs What the games were computed from.
  /// @return The games.
  /// @retval MyGames::Invalid::duplicate_product A game's product id is the
  ///     id or an alias of an earlier game.
  /// @retval MyGames::Invalid::ownership_knowledge A game's ownership
  ///     contradicts Inputs::library, or Inputs::ps5_library for a PS5 cloud
  ///     game.
  [[nodiscard]] static auto of(AccountId account,
                               std::vector<MyGame> ordered, Inputs inputs)
      -> Expected<MyGames, Invalid>;
  /// @brief Builds the games together with the explanations of how each was
  /// decided.
  ///
  /// @param account The account the games are for.
  /// @param ordered The games in display order; shown games are moved ahead
  ///     of hidden ones, each group keeping its order.
  /// @param inputs What the games were computed from.
  /// @param explanations One explanation per listed game or dropped copy, in
  ///     any order.
  /// @return The games.
  /// @retval MyGames::Invalid::duplicate_product A game's product id is the
  ///     id or an alias of an earlier game.
  /// @retval MyGames::Invalid::ownership_knowledge A game's ownership
  ///     contradicts Inputs::library, or Inputs::ps5_library for a PS5 cloud
  ///     game.
  [[nodiscard]] static auto of(AccountId account,
                               std::vector<MyGame> ordered, Inputs inputs,
                               std::vector<Explanation> explanations)
      -> Expected<MyGames, Invalid>;

  /// @brief The account the games are for.
  ///
  /// @return The account id the games were built for.
  auto account() const noexcept -> AccountId { return account_; }
  /// @brief What the games were computed from.
  ///
  /// @return The inputs, valid while these games live.
  auto inputs() const noexcept -> const Inputs& { return inputs_; }
  /// @brief Whether the library could be read when the games were built.
  ///
  /// @return Whether the library was fetched or could not be read.
  auto library_state() const noexcept -> LibraryState {
    return inputs_.library;
  }
  /// @brief Whether the library was read, so that a game without a copy is
  /// known not to be owned.
  ///
  /// @return True when the library was fetched; false when it could not be
  /// read.
  auto ownership_is_known() const noexcept -> bool {
    return inputs_.library == LibraryState::fetched;
  }
  /// @brief Whether a game on one service that has no copy is known not to
  /// be owned.
  ///
  /// @param service The service the game streams on.
  /// @return For PS5 cloud streaming, true only when every PS5 entitlement
  ///     was read (Inputs::ps5_library is LibraryState::fetched); for the
  ///     older service, the same as MyGames::ownership_is_known.
  auto ownership_is_known_on(ServiceGeneration service) const noexcept -> bool {
    return service == ServiceGeneration::ps5_cloud
               ? inputs_.ps5_library == LibraryState::fetched
               : ownership_is_known();
  }
  /// @brief Says whether the user holds a product, in the form a
  /// StreamRefusal records.
  ///
  /// @param product The product, by its listed id or an alias.
  /// @return The game's holding when it is listed; otherwise not held when
  ///     the library was read, and unknown when it was not.
  auto holding_of(const ProductId& product) const -> StreamRefusal::Holding;
  /// @brief Whether these games were built from exactly the given inputs.
  ///
  /// @param inputs The inputs to compare against.
  /// @return True when the stored inputs equal the given inputs; false
  /// otherwise.
  auto was_computed_from(const Inputs& inputs) const noexcept -> bool {
    return inputs_ == inputs;
  }

  /// @brief The games to show, in display order.
  ///
  /// @return The shown games in display order, valid while these games live;
  ///     possibly empty.
  auto visible() const noexcept -> std::span<const MyGame>;
  /// @brief The games that are listed but not shown, each with a HideReason.
  ///
  /// @return The hidden games, valid while these games live; possibly empty.
  auto hidden() const noexcept -> std::span<const MyGame>;
  /// @brief Number of games, shown and hidden together.
  ///
  /// @return The count of shown and hidden games together, zero when there are
  /// none.
  auto size() const noexcept -> std::size_t { return games_.size(); }
  /// @brief Finds a game, shown or hidden, by product.
  ///
  /// @param product The listed product id or one of the title's aliases.
  /// @return The game, or null when the product is not among the games.
  auto find(const ProductId& product) const noexcept -> const MyGame*;
  /// @brief Says which rules listed, hid or dropped a product.
  ///
  /// @param product The product, by its listed id or an alias; a dropped
  ///     owned copy is found by its own product id.
  /// @return The recorded explanation. A listed game without one gets an
  ///     explanation with no steps; an unknown product gets
  ///     Explanation::not_listed.
  auto explain(const ProductId& product) const -> Explanation;

  /// @brief Compares account, games, inputs and explanations.
  friend auto operator==(const MyGames&, const MyGames&) -> bool = default;

 private:
  MyGames(AccountId account, std::vector<MyGame> games,
          std::size_t visible, Inputs inputs) noexcept;

  AccountId account_;
  std::vector<MyGame> games_;
  std::size_t visible_;
  Inputs inputs_;
  std::vector<Explanation> explanations_{};
};

}  // namespace libchiaking::catalog
