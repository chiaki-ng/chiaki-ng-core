/// @file
/// @brief Declares ps::OldServiceShelves, the adapter that walks the
/// LegacyCloudStream (old service) Kamaji shelves the account's own streaming
/// service lists.
///
/// A Kamaji session gives a store base URL; its root container lists the
/// shelves (categories), and each shelf lists product rows. The walk is what
/// tells the catalog both which titles LegacyCloudStream streams and whether
/// the account may use LegacyCloudStream at all (CloudAccess).
#pragma once

#include "src/core/catalog/adapters/ps/wall_stamp.hpp"
#include "src/core/catalog/model/list/raw_listing.hpp"
#include "src/core/catalog/model/page_count.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/kamaji/kamaji_client.hpp"

#include <libchiaking/detail/catalog/cloud_access.hpp>
#include <libchiaking/detail/catalog/platform.hpp>
#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::ps {

/// @brief Walks the LegacyCloudStream shelves for one user.
///
/// Borrows the user, client and clock, which must outlive it. Used on the
/// thread that runs the catalog call.
class OldServiceShelves {
 public:
  /// @brief The most shelves walked.
  static constexpr std::size_t shelf_limit = 64;
  /// @brief Rows asked of the root container.
  static constexpr int root_page_size = 100;
  /// @brief Rows asked of each shelf.
  static constexpr int shelf_page_size = 500;
  /// @brief The first link index never asked of a shelf; a shelf is read no
  /// further than this.
  static constexpr int shelf_start_limit = 10000;
  /// @brief The pause between shelves.
  static constexpr std::chrono::milliseconds shelf_delay{100};
  /// @brief The longest country or language segment read from a base URL.
  static constexpr std::size_t region_text_limit = 7;

  /// @brief Sets up the walk.
  ///
  /// @param user The signed-in user; opens the Kamaji session. Must outlive
  ///     this object.
  /// @param http Sends the requests. Must outlive this object.
  /// @param wall Stamps the listing. Must outlive this object.
  /// @param maximum_response_bytes The largest reply accepted.
  OldServiceShelves(users::PsUser& user, net::http::Client& http,
                    const platform::ClockProvider& wall,
                    std::size_t maximum_response_bytes =
                        net::http::default_maximum_response_bytes) noexcept;

  /// @brief Walks every shelf.
  ///
  /// Opens a session, reads the store base URL and the root container, then
  /// each shelf in turn with shelf_delay between them; a failed shelf is
  /// asked again as RetryPolicy::shelf says and then skipped, marking the
  /// listing incomplete. Rows are kept once per id. Blocks for the whole walk,
  /// which takes at least 100 ms per shelf.
  ///
  /// @param pages Told the root page, each shelf page read and each shelf's
  ///     page total.
  /// @return The rows from the account's service; the session's failure;
  ///     Unanswered::Reason::region_unsupported when the store base URL is
  ///     refused with a 4xx status; Unanswered::Reason::malformed when the root
  ///     lists no shelf or no region can be told; a cancellation when stopped,
  ///     including during a pause between shelves.
  [[nodiscard]] auto walk(PageCount pages = {})
      -> Expected<RawListing, Unanswered>;
  /// @brief What a walk says about the account's LegacyCloudStream access.
  ///
  /// @param walked The walk's result.
  /// @return Available with the title count, completeness and origin after a
  ///     walk; otherwise without a listing, as access rejected when the service
  ///     refused the account, sign-in expired, region unsupported, or
  ///     unavailable.
  auto access_shown_by(const Expected<RawListing, Unanswered>& walked) const
      -> CloudAccess;

  /// @brief The country and language a store base URL names.
  ///
  /// Reads the two path segments after "/container/", each at most
  /// region_text_limit characters.
  ///
  /// @param base_url The base URL.
  /// @return The region; empty fields when the URL does not name one.
  static auto store_region_of(std::string_view base_url) -> users::Region;

 private:
  /// @brief The shelf URLs the root container lists.
  ///
  /// @param store The open session.
  /// @param base_url The store base URL.
  /// @return Up to shelf_limit shelf URLs; the mapped failure of the root
  ///     request; Unanswered::Reason::malformed when it lists none.
  [[nodiscard]] auto shelves_at(psn::rest::KamajiClient& store,
                                std::string_view base_url)
      -> Expected<std::vector<std::string>, Unanswered>;
  /// @brief Reads one shelf, page after page, until its stated total, an
  /// empty page or shelf_start_limit.
  ///
  /// @param store The open session.
  /// @param shelf The shelf URL.
  /// @param pages Told each page read and the shelf's page total.
  /// @return Every page's links in order, with the first page's total, or
  ///     std::nullopt when a request failed.
  [[nodiscard]] auto titles_on(psn::rest::KamajiClient& store,
                               std::string_view shelf, PageCount& pages)
      -> std::optional<psn::rest::ContainerPage>;
  /// @brief The listing's source for a base URL.
  ///
  /// The region is the user's own when known, else the store's; the settled
  /// locale is the store's, else the region.
  ///
  /// @param base_url The store base URL.
  /// @return The source, complete and from the account's service, or
  ///     std::nullopt when neither the user nor the URL gives a region.
  [[nodiscard]] auto region_of(std::string_view base_url) const
      -> std::optional<RawListing::Source>;

  /// @brief Maps a Kamaji failure to a catalog failure.
  ///
  /// @param error The failure.
  /// @return A cancellation; Unanswered::Reason::rejected for a refused 4xx;
  ///     Unanswered::Reason::malformed for an unreadable reply;
  ///     Unanswered::Reason::unavailable otherwise.
  static auto unanswered(const psn::rest::StoreError& error) noexcept
      -> Unanswered;
  /// @brief Maps a failure to read the store base URL.
  ///
  /// @param error The failure.
  /// @return Unanswered::Reason::region_unsupported for a refused 4xx, else as
  ///     unanswered.
  static auto without_stores(const psn::rest::StoreError& error) noexcept
      -> Unanswered;

  /// @brief The user; not owned.
  users::PsUser& user_;
  /// @brief Sends requests; not owned.
  net::http::Client& http_;
  /// @brief Stamps the listing.
  WallStamp stamp_;
  /// @brief The largest reply accepted, in bytes.
  std::size_t maximum_response_bytes_;
};

}  // namespace libchiaking::catalog::ps
