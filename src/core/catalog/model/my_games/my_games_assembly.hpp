/// @file
/// @brief Declares MyGamesAssembly, which builds MyGames from the
/// LegacyCloudStream and PS5 cloud streaming listings.
///
/// Each service decides its own rows (legacy_cloud_stream::Games and
/// cloud_stream::Games). The assembly merges them, keeps a product on one
/// service only, notes copies held on the other service, and orders the result:
/// owned first, then by name.
#pragma once

#include "src/core/catalog/model/cloud_stream/for_sale_category.hpp"
#include "src/core/catalog/model/cloud_stream/generation_links.hpp"
#include "src/core/catalog/model/cloud_stream/premium_category.hpp"
#include "src/core/catalog/model/library/owned_ps5_games.hpp"
#include "src/core/catalog/model/my_games/service_listing.hpp"
#include "src/core/users/cloud_streaming.hpp"

#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/catalog/my_games.hpp>
#include <libchiaking/detail/catalog/stream_refusal.hpp>
#include <libchiaking/detail/catalog/streamable_list.hpp>
#include <libchiaking/detail/users/membership.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::catalog {

/// @brief The merged games of both services for one account, before they
/// are handed out as MyGames.
///
/// Built once from the two services' listings and read with games. It owns
/// copies of everything it holds. It is pure computation; build and read it
/// on one thread.
class MyGamesAssembly {
 public:
  /// @brief Wall-clock time in milliseconds.
  using Time = MyGames::Time;
  /// @brief What the games are built from.
  using Inputs = MyGames::Inputs;

  /// @brief The held answers a build reads, all borrowed for the call.
  ///
  /// Every pointer may be null when that answer is not held.
  struct Sources {
    /// @brief The account's library, LegacyCloudStream and PS5 parts together.
    const Library& library;
    /// @brief Whether the library could be read.
    LibraryState library_state{LibraryState::fetched};
    /// @brief The LegacyCloudStream streamable list; null lists no
    /// LegacyCloudStream games.
    const StreamableList* list{};
    /// @brief The PS5 entitlement scan; null when not held.
    const OwnedPs5Games* ps5{};
    /// @brief The Premium category; null when not held.
    const cloud_stream::PremiumCategory* category{};
    /// @brief The public Store streaming category of games for sale; null
    /// when not held.
    const cloud_stream::ForSaleCategory* for_sale{};
    /// @brief The account's store region, for PS5 store pages when no
    /// category is held.
    std::optional<StoreLocale> region{};
    /// @brief The store games whose Play First Trial the signed-in store
    /// no longer offers the account.
    std::span<const ProductId> used_trials{};
    /// @brief The counter that changes when the used trials change.
    std::uint64_t used_trials_revision{};
  };

  /// @brief Builds the user's games from whatever is held.
  ///
  /// The PS5 library state is the scan's state when a scan is held, the
  /// library's own PS5 state when the library was fetched, and unavailable
  /// otherwise. Does not block.
  ///
  /// @param sources The held answers.
  /// @param membership The user's PS Plus membership; its tier is an input.
  /// @param streaming The user's cloud streaming benefits; std::nullopt
  ///     when they could not be read.
  /// @param refusals Every remembered refusal.
  /// @param refusals_revision The counter that changes when the refusals
  ///     change.
  /// @param now The moment refusals expire against.
  /// @return The games, with explanations.
  static auto games_of(const Sources& sources,
                       const users::Membership& membership,
                       const std::optional<users::CloudStreaming>& streaming,
                       std::span<const StreamRefusal> refusals,
                       std::uint64_t refusals_revision, Time now) -> MyGames;

  /// @brief Merges the two services' listings.
  ///
  /// A PS5 row whose product LegacyCloudStream already lists is dropped and its
  /// explanation kept as unlisted; a PS5 alias LegacyCloudStream lists is
  /// removed from the PS5 row. An unlisted explanation that failed on
  /// Rule::service is folded into the listed row known as its product as a
  /// noted step. A LegacyCloudStream row and a PS5 row the store links as two
  /// generations of one game are linked to each other. The rows are then
  /// ordered owned first, then by name ignoring case, with each linked pair
  /// ordered by the version that leads it.
  ///
  /// @param account The account the games are for.
  /// @param inputs What the games were built from.
  /// @param legacy_cloud_stream The LegacyCloudStream listing.
  /// @param cloud_stream The PS5 cloud streaming listing.
  /// @param links The PS4 and PS5 titles the store sells together.
  MyGamesAssembly(AccountId account, Inputs inputs,
                  ServiceGames legacy_cloud_stream, ServiceGames cloud_stream,
                  const cloud_stream::GenerationLinks& links = {});

  /// @brief The merged games.
  ///
  /// Explanations are ordered shown games first, then hidden games, then
  /// unlisted products.
  ///
  /// @return The games.
  auto games() const -> MyGames;

 private:
  /// @brief The store region PS5 pages are linked in.
  ///
  /// @param sources The held answers.
  /// @return The Premium category's locale, else the for-sale category's,
  ///     else the account's region, else English (US).
  static auto ps5_store_of(const Sources& sources) -> StoreLocale;
  /// @brief Drops PS5 rows and aliases whose product LegacyCloudStream lists.
  void keep_legacy_cloud_stream_products_apart();
  /// @brief Folds "held on the other service" explanations into the
  /// listed row they refer to.
  void note_copies_held_on_the_other_service();
  /// @brief Links each LegacyCloudStream row to the PS5 row of the same game.
  ///
  /// A LegacyCloudStream row whose title number, or an alias's, the store links
  /// to the title of a listed PS5 row that is not a bundle is paired with the
  /// first such row. The PS5 version leads the pair.
  ///
  /// @param links The PS4 and PS5 titles the store sells together.
  void link_generations(const cloud_stream::GenerationLinks& links);
  /// @brief Orders rows owned first, then by name ignoring case; stable.
  ///
  /// A linked version sorts by its pair's lead, right after it, so the pair
  /// stays together.
  void order();

  /// @brief The account the games are for.
  AccountId account_;
  /// @brief What the games were built from.
  Inputs inputs_;
  /// @brief Every listed row; the first legacy_cloud_stream_count_ came from
  /// LegacyCloudStream until order runs.
  std::vector<ServiceGames::Listed> listed_;
  /// @brief How many rows LegacyCloudStream listed; set at construction.
  std::size_t legacy_cloud_stream_count_;
  /// @brief Explanations for products that were not listed.
  std::vector<Explanation> unlisted_;
};

}  // namespace libchiaking::catalog
