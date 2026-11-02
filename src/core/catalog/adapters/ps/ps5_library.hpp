/// @file
/// @brief Declares ps::Ps5Library, the adapter that reads the account's PS5
/// entitlements from the mobile internal entitlement list.
///
/// GET /api/entitlement/v2/users/me/internal/entitlements with the mobile
/// app token, active PSGD streaming entitlements only, paged; a changed
/// revisionId restarts the scan a bounded number of times. A launch re-reads
/// one entitlement through the singular endpoint.
#pragma once

#include "src/core/catalog/adapters/ps/wall_stamp.hpp"
#include "src/core/catalog/model/library/ps5_entitlement.hpp"
#include "src/core/catalog/model/page_count.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/entitlement/library_entitlements.hpp"

#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::ps {

/// @brief Reads one user's PS5 entitlements, all of them or one at a time.
///
/// Borrows the user, client and clock, which must outlive it. Used on the
/// thread that runs the catalog call.
class Ps5Library {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = WallStamp::Time;

  /// @brief The most distinct entitlements one_of asks about.
  static constexpr std::size_t lookup_limit = 3;
  /// @brief The longest cover URL kept, in bytes; longer ones are cut.
  static constexpr std::size_t image_url_limit = 1023;

  /// @brief The result of a full scan.
  struct Scanned {
    /// @brief Every entitlement that parsed, games or not, in PS's order.
    std::vector<Ps5Entitlement> records;
    /// @brief LibraryState::partial when the revision kept changing or the page
    /// cap was reached; LibraryState::fetched otherwise.
    LibraryState ownership{LibraryState::fetched};
    /// @brief How many entitlements PS returned.
    std::size_t read{};
    /// @brief How many times the scan restarted on a changed revisionId.
    int revision_restarts{};
    /// @brief When the scan finished.
    Time fetched_at{};
  };

  /// @brief Sets up the reads for a user.
  ///
  /// @param user The signed-in user; supplies the app token. Must
  ///     outlive this object.
  /// @param http Sends the requests. Must outlive this object.
  /// @param wall Stamps the scan. Must outlive this object.
  /// @param maximum_response_bytes The largest reply accepted per page.
  Ps5Library(users::PsUser& user, net::http::Client& http,
             const platform::ClockProvider& wall,
             std::size_t maximum_response_bytes =
                 net::http::default_maximum_response_bytes) noexcept;

  /// @brief Reads every PS5 entitlement.
  ///
  /// Blocks for every page in turn; each request is bounded by the HTTP
  /// provider's timeouts and retries and abandoned when the client's stop token
  /// fires. Logs how many records were read and why records were dropped.
  ///
  /// @param pages Told each page read and the page total, again on each
  ///     restart.
  /// @return The scan; the read's failure (including a lapsed app token, which
  ///     makes PS5 ownership unknown) otherwise.
  [[nodiscard]] auto scan(PageCount pages = {})
      -> Expected<Scanned, Unanswered>;
  /// @brief Re-reads one entitlement through the singular endpoint.
  ///
  /// Blocks on one request.
  ///
  /// @param id The entitlement id.
  /// @return The record; std::nullopt when PS says the account does not hold
  ///     it; Unanswered::Reason::malformed when the reply does not parse;
  ///     otherwise the request's failure.
  [[nodiscard]] auto one(const OwnershipId& id)
      -> Expected<std::optional<Ps5Entitlement>, Unanswered>;
  /// @brief Re-reads the first of several entitlements the account holds.
  ///
  /// Asks about each distinct id in turn, at most lookup_limit of them, and
  /// stops at the first held one or the first failure.
  ///
  /// @param ids The entitlement ids, in the order to ask.
  /// @return The first held record; std::nullopt when none of those asked is
  ///     held; otherwise the first failure.
  [[nodiscard]] auto one_of(std::span<const OwnershipId> ids)
      -> Expected<std::optional<Ps5Entitlement>, Unanswered>;

  /// @brief Whether an entitlement id may be a PS5 entitlement.
  ///
  /// @param id The entitlement id.
  /// @return True when it holds no title number or a PS5 one; false for a PS4
  /// or
  ///     PS3 title number.
  static auto asks_about(const OwnershipId& id) -> bool;

 private:
  /// @brief Reads one entitlement.
  ///
  /// @param game The entitlement as PS sent it.
  /// @return The record, or std::nullopt when its id does not parse; other ids
  ///     that do not parse are left empty.
  static auto record_of(const psn::rest::GameEntitlement& game)
      -> std::optional<Ps5Entitlement>;
  /// @brief The entitlement's display name.
  ///
  /// @param game The entitlement.
  /// @return The first non-empty of the names PS sent; empty when none.
  static auto name_of(const psn::rest::GameEntitlement& game) -> std::string;
  /// @brief The entitlement's cover URL.
  ///
  /// @param game The entitlement.
  /// @return The concept icon, else the game icon, else the product image, cut
  ///     to image_url_limit; empty when none.
  static auto cover_of(const psn::rest::GameEntitlement& game) -> std::string;
  /// @brief Logs what a scan read, how many records were not games and why,
  /// and the games named like extras.
  ///
  /// @param scanned The scan.
  /// @param found What PS returned.
  void note(const Scanned& scanned,
            const psn::rest::GameEntitlements& found) const;
  /// @brief Logs a trial or Play First Trial the scan dropped, with the
  /// flags PS sent for it, so an ended trial can be told from one PS
  /// withdrew.
  ///
  /// @param record The dropped record; records that are not trials are
  ///     ignored.
  /// @param why Why the record is not a game.
  void note_dropped_trial(const Ps5Entitlement& record, NotAGame why) const;
  /// @brief An optional flag as log text.
  ///
  /// @param flag The flag as read.
  /// @return "true", "false" or "absent".
  static auto flag_text(const std::optional<bool>& flag) noexcept
      -> std::string_view;

  /// @brief The user; not owned.
  users::PsUser& user_;
  /// @brief Sends requests; not owned.
  net::http::Client& http_;
  /// @brief Stamps the scan.
  WallStamp stamp_;
  /// @brief The largest reply accepted, in bytes.
  std::size_t maximum_response_bytes_;
};

}  // namespace libchiaking::catalog::ps
