/// @file
/// @brief Declares ps::PublicStoreWalk, the anonymous walk of the public
/// store's APOLLOROOT containers.
///
/// The account's own country store lists what its LegacyCloudStream service
/// lists, so a complete walk of it is kept like the service's list. Only when
/// that country has no public store does the walk read the US or GB store, and
/// that listing stays a fallback that is never persisted.
#pragma once

#include "src/core/catalog/adapters/ps/wall_stamp.hpp"
#include "src/core/catalog/model/list/raw_listing.hpp"
#include "src/core/catalog/model/page_count.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/kamaji/kamaji_client.hpp"

#include <libchiaking/detail/catalog/platform.hpp>
#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::ps {

/// @brief Walks the public store's root container and its direct children.
///
/// Borrows the client and clock, which must outlive it.
class PublicStoreWalk {
 public:
  /// @brief Rows asked per page.
  static constexpr int page_size = 100;
  /// @brief The most child containers walked.
  static constexpr int child_limit = 40;
  /// @brief The most pages read across the whole walk.
  static constexpr int page_budget = 200;
  /// @brief The highest row offset read in one container.
  static constexpr int start_limit = 10000;
  /// @brief The language the fallback store is asked in.
  static constexpr std::string_view fallback_language = "en";
  /// @brief The root container walked for the US store.
  static constexpr std::string_view americas_root =
      "STORE-MSF192018-APOLLOROOT";
  /// @brief The root container walked for the GB store.
  static constexpr std::string_view other_root = "STORE-MSF192014-APOLLOROOT";

  /// @brief Sets up the walk.
  ///
  /// @param http Sends the requests. Must outlive this object.
  /// @param wall Stamps the listing. Must outlive this object.
  /// @param maximum_response_bytes The largest reply accepted per page.
  PublicStoreWalk(net::http::Client& http, const platform::ClockProvider& wall,
                  std::size_t maximum_response_bytes =
                      net::http::default_maximum_response_bytes) noexcept;

  /// @brief Walks the root and each child container it lists.
  ///
  /// The account's own country store is walked first, in the account's
  /// language, and its rows are marked ListOrigin::regional_store. When that
  /// country has no store root, the US store (Americas) or GB store (anywhere
  /// else) is walked in English instead, marked ListOrigin::public_store.
  /// Children's own nested containers are not walked. Any failed page, budget
  /// or cap makes the listing incomplete rather than failing it. Blocks for
  /// up to page_budget requests per store walked.
  ///
  /// @param region The account's region.
  /// @param pages Told each page read and each container's page total.
  /// @return The rows from the store walked; a cancellation when stopped.
  /// @retval Unanswered::Reason::unavailable Neither store has a root.
  [[nodiscard]] auto walk(const StoreLocale& region, PageCount pages = {})
      -> Expected<RawListing, Unanswered>;

  /// @brief The root container of a country's store.
  ///
  /// @param country The store's country code, in any case.
  /// @return americas_root for the Americas, other_root otherwise.
  static auto root_for(std::string_view country) -> std::string_view;

 private:
  /// @brief One country store to walk. Plain data.
  struct Storefront {
    /// @brief The store's country code.
    std::string country;
    /// @brief The language the store is asked in.
    std::string language;
    /// @brief The origin its rows are marked with.
    ListOrigin origin{};
  };

  /// @brief How walking one container ended.
  enum class Walked : std::uint8_t {
    /// @brief Every page was read and nothing was dropped.
    read,
    /// @brief Rows were read but the container may hold more.
    incomplete,
    /// @brief The store has no such container.
    missing,
  };

  /// @brief What to do with containers nested in a walked container.
  enum class Nested : std::uint8_t {
    /// @brief Collect them as children to walk next (the root).
    collected,
    /// @brief Do not walk them and mark the listing incomplete (a child).
    refused
  };

  /// @brief What the walk has gathered so far.
  struct Progress {
    /// @brief Counts the pages read and expected.
    PageCount& pages;
    /// @brief The product rows read.
    std::vector<ListedEntry> entries;
    /// @brief Product ids already read.
    std::set<std::string, std::less<>> seen;
    /// @brief Child containers to walk, in the order found.
    std::vector<std::string> children;
    /// @brief Pages that may still be read.
    int pages_left{page_budget};
  };

  /// @brief A container page started but not yet read.
  struct Asked {
    /// @brief Index of the page's first link.
    int start{};
    /// @brief The started request.
    psn::rest::KamajiClient::PendingPage page;
  };

  /// @brief Walks one container, reading its pages in order.
  ///
  /// The first page is read alone. Once a page states the total, every
  /// remaining page up to it, the start limit and the budget is started at
  /// once; without a total, or with a negative one, pages are asked one at a
  /// time and no page count is expected.
  ///
  /// @param store The anonymous store client.
  /// @param front The store being walked.
  /// @param container The container id.
  /// @param nested What to do with nested containers.
  /// @param progress What the walk has gathered; extended in place.
  /// @param first The container's first page, when already started.
  /// @return Walked::missing when the first page is refused as not found;
  ///     Walked::incomplete when a page failed, the budget or cap ran out,
  ///     the end could not be confirmed, or a nested container was dropped;
  ///     Walked::read otherwise.
  auto walked(psn::rest::KamajiClient& store, const Storefront& front,
              std::string_view container, Nested nested, Progress& progress,
              std::optional<Asked> first) const -> Walked;
  /// @brief Starts one container page, spending one page of the budget.
  ///
  /// @param store The anonymous store client.
  /// @param front The store being walked.
  /// @param container The container id.
  /// @param start Index of the page's first link.
  /// @param progress The walk; its budget is spent.
  /// @return The started page, or nothing when the budget is spent.
  static auto ask(psn::rest::KamajiClient& store, const Storefront& front,
                  std::string_view container, int start, Progress& progress)
      -> std::optional<Asked>;
  /// @brief Walks one country store's root and children.
  ///
  /// @param store The anonymous store client.
  /// @param front The store to walk.
  /// @param region The account's region, recorded on the listing.
  /// @param pages Counts the pages read and expected.
  /// @return The listing, or std::nullopt when the store has no root.
  auto walked_store(psn::rest::KamajiClient& store, const Storefront& front,
                    const StoreLocale& region, PageCount& pages) const
      -> std::optional<RawListing>;
  /// @brief Logs a warning about the walk.
  ///
  /// @param message The text after the walk's prefix.
  void warn(const std::string& message) const;

  /// @brief Sends requests; not owned.
  net::http::Client& http_;
  /// @brief Stamps the listing.
  WallStamp stamp_;
  /// @brief The largest reply accepted, in bytes.
  std::size_t maximum_response_bytes_;
};

}  // namespace libchiaking::catalog::ps
