/// @file
/// @brief Declares FetchReport, a diagnostics summary of where the held catalog
/// answers came from.
#pragma once

#include "libchiaking/detail/catalog/my_games.hpp"
#include "libchiaking/detail/catalog/streamable_list.hpp"
#include "libchiaking/detail/catalog/trusted_clock.hpp"
#include "libchiaking/detail/catalog/wall_time.hpp"
#include "libchiaking/detail/users/membership.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace libchiaking::catalog {

/// @brief Where the answers held for one user came from and how much they
/// contain.
///
/// The report is a snapshot for a diagnostics screen. It describes the
/// streamable list held for the user's region, the library held for the account
/// and the plan data.
///
/// @see Catalog::how_fetched
struct FetchReport {
  /// @brief Wall-clock time in milliseconds.
  using Time = catalog::Time;

  /// @brief The part of the streamable list that came from the older
  /// LegacyCloudStream service.
  struct LegacyCloudStream {
    /// @brief Whether the list was walked through the account's service or
    /// the public store.
    ListOrigin origin{};
    /// @brief Whether every shelf and page of the whole streamable list was
    /// read.
    bool complete{};
    /// @brief When the streamable list was built.
    Time fetched_at{};
    /// @brief Titles in the list that stream on the older service.
    std::size_t titles{};
    /// @brief How many PS4 products were asked about a streaming copy and
    /// how many were answered; empty when the question was not asked.
    std::optional<StreamableList::CopiesChecked> copies_checked{};
    /// @brief Titles on the older service that the list marks as not
    /// streamable.
    std::size_t without_a_covered_copy{};
  };

  /// @brief Who the PS5 plan data was asked for.
  struct Audience {
    /// @brief Two-letter country of the account, upper case.
    std::string country;
    /// @brief Language of the account, lower case.
    std::string language;
    /// @brief The account holder's age in years, as PS reports it.
    int age{};
  };

  /// @brief The held premium streaming category, which says which PS5 titles
  /// a plan includes.
  struct PremiumCategory {
    /// @brief When the category was fetched.
    Time fetched_at{};
    /// @brief Titles in the category.
    std::size_t titles{};
    /// @brief Products the server returned across every page read, before
    /// any was dropped; the offset the next page would be asked from.
    std::size_t products_read{};
    /// @brief The category's size as the server reported it on the last
    /// page read.
    std::size_t server_total{};
    /// @brief Whether the walk reached the end of the category; false when
    /// it stopped early on a failed page, a repeated page or the page cap.
    bool complete{};
  };

  /// @brief The held public Store streaming category, the only source of PS5
  /// titles that can be bought but are not owned or included in a plan.
  ///
  /// Category membership grants no access; a product there becomes a "for
  /// sale" tile only when it is not already owned or included.
  struct ForSaleCategory {
    /// @brief When the category was fetched, by device time.
    Time fetched_at{};
    /// @brief Products in the category with an entitlement that is a full
    /// game that streams; owned or included ones among them do not become
    /// "for sale" tiles.
    std::size_t titles{};
    /// @brief Products the server returned across every page read, before
    /// any was dropped; the offset the next page would be asked from.
    std::size_t products_read{};
    /// @brief The category's size as the server reported it on the last
    /// page read.
    std::size_t server_total{};
    /// @brief Whether the walk reached the end of the category; false when
    /// it stopped early on a failed page, a repeated page or the page cap.
    bool complete{};
  };

  /// @brief The held scan of the account's owned PS5 entitlements.
  struct OwnedGames {
    /// @brief Whether PS5 ownership was read in full, in part, or could not
    /// be read; LibraryState::partial when PS's revision kept changing
    /// or the page cap was reached.
    LibraryState state{};
    /// @brief Entitlement records PS returned in the final pass, before
    /// those that are not streamable full games were dropped; zero when no
    /// PS5 scan is held.
    std::size_t records_read{};
    /// @brief How many times the scan started over because PS's revision
    /// changed while it was paging.
    int revision_restarts{};
    /// @brief When the PS5 scan was made, by device time; empty when only
    /// the library's PS5 ownership state is known and no scan is held.
    std::optional<Time> fetched_at{};
  };

  /// @brief The held restriction list of PS5 titles that may not be
  /// launched.
  struct Exclusions {
    /// @brief When the list was fetched, by device time.
    Time fetched_at{};
    /// @brief How old the list was when the report was made, by device
    /// time; negative when the device clock has stepped back.
    std::chrono::milliseconds age{};
    /// @brief Whether the list is younger than 24 hours by device time. A
    /// stale list is fetched again before it is used, and PS5 launches are
    /// refused while that fetch fails.
    bool fresh{};
    /// @brief Entries on the list.
    std::size_t titles{};
  };

  /// @brief The part of the held data that concerns PS5 cloud streaming.
  struct CloudStream {
    /// @brief Titles in the list that stream through PS5 cloud streaming.
    std::size_t titles{};
    /// @brief Who the plan data is asked for; empty when the account does
    /// not state a country, language and age.
    std::optional<Audience> audience{};
    /// @brief The held premium streaming category; empty when none is held
    /// for the audience.
    std::optional<PremiumCategory> premium_category{};
    /// @brief The held Store streaming category; empty when none is held
    /// for the audience.
    std::optional<ForSaleCategory> for_sale_category{};
    /// @brief The held PS5 ownership scan; empty when nothing is held for
    /// the account.
    std::optional<OwnedGames> owned{};
    /// @brief The sync behind the trusted time used for release dates;
    /// empty when no trusted time is held or it has expired.
    std::optional<TrustedClock::Sync> trusted_time{};
    /// @brief The held restriction list; empty when none has been fetched.
    std::optional<Exclusions> exclusions{};
  };

  /// @brief What is held for the older LegacyCloudStream service.
  LegacyCloudStream legacy_cloud_stream;
  /// @brief What is held for PS5 cloud streaming.
  CloudStream cloud_stream;
  /// @brief Whether the library was fetched; empty when nothing is held for
  /// the account.
  std::optional<LibraryState> library{};
  /// @brief When the held library was fetched or last amended.
  std::optional<Time> library_fetched_at{};
  /// @brief Owned games in the held library.
  std::size_t library_records{};
  /// @brief The user's PlayStation Plus tier; empty when it is not known.
  std::optional<users::Tier> tier{};
};

}  // namespace libchiaking::catalog
