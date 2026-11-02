/// @file
/// @brief Declares ps::Ps5StoreLists, the adapter for the imagic PS5
/// store-app lists.
///
/// These lists are no longer a PS5 source; they only lend presentation to owned
/// PS4 copies that the LegacyCloudStream shelves do not carry.
#pragma once

#include "src/core/catalog/adapters/ps/store_listing.hpp"
#include "src/core/catalog/adapters/ps/wall_stamp.hpp"
#include "src/core/catalog/model/list/raw_listing.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "src/core/net/transport/http/client.hpp"

#include <libchiaking/detail/catalog/platform.hpp>
#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::catalog::ps {

/// @brief Reads the six imagic lists for one region.
///
/// Borrows the client and clock, which must outlive it.
class Ps5StoreLists {
 public:
  /// @brief The imagic list names and the shelf each fills, in the order read.
  static constexpr std::array<std::pair<std::string_view, Shelf>, 6> lists{{
      {"all-ps5-list", Shelf::all_ps5},
      {"plus-games-list", Shelf::plus_games},
      {"ubisoft-classics-list", Shelf::ubisoft_classics},
      {"plus-classics-list", Shelf::plus_classics},
      {"plus-monthly-games-list", Shelf::plus_monthly_games},
      {"free-to-play-list", Shelf::free_to_play},
  }};

  /// @brief Sets up the reads.
  ///
  /// @param http Sends the requests. Must outlive this object.
  /// @param wall Stamps the listing. Must outlive this object.
  /// @param maximum_response_bytes The largest reply accepted per list.
  Ps5StoreLists(net::http::Client& http, const platform::ClockProvider& wall,
                std::size_t maximum_response_bytes =
                    net::http::default_maximum_response_bytes) noexcept;

  /// @brief Reads every list in the first locale that answers.
  ///
  /// Tries the region's locale and its fallbacks in turn; the first locale for
  /// which any list loads is used, and lists that fail there are skipped.
  /// Blocks for one request per list per locale tried.
  ///
  /// @param region The store region.
  /// @return The rows, marked as from the public store and complete only when
  ///     all-ps5-list loaded; a cancellation when stopped;
  ///     Unanswered::Reason::unavailable with the last HTTP status when no
  ///     locale answered.
  [[nodiscard]] auto fetch(const StoreLocale& region)
      -> Expected<RawListing, Unanswered>;

 private:
  /// @brief One group of an imagic list reply.
  struct Group {
    /// @brief The products in the group.
    json::ps::Tolerant<std::vector<json::ps::Tolerant<ListedProduct>>> games;
  };
  /// @brief An imagic list reply: a list of groups.
  using Reply = std::vector<json::ps::Tolerant<Group>>;

  /// @brief The readable rows of one list.
  ///
  /// @param list The reply.
  /// @param shelf The list's shelf.
  /// @return The entries; unreadable rows are skipped.
  static auto listed(const Reply& list, Shelf shelf)
      -> std::vector<ListedEntry>;

  /// @brief Sends requests; not owned.
  net::http::Client& http_;
  /// @brief Stamps the listing.
  WallStamp stamp_;
  /// @brief The largest reply accepted, in bytes.
  std::size_t maximum_response_bytes_;
};

}  // namespace libchiaking::catalog::ps
