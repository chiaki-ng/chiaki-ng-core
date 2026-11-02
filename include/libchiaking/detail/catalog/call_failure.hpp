/// @file
/// @brief Declares CallFailure, why one Catalog call could not answer, and the
/// reasons each call can give.
#pragma once

#include "libchiaking/detail/users/ps_user.hpp"

#include <cstdint>
#include <optional>

namespace libchiaking::catalog {

/// @brief Why one Catalog call could not answer.
///
/// Each call names its own Reason enumeration, listing only the failures that
/// call can produce, so a frontend switch over it covers exactly what can
/// happen. A frontend usually branches on CallFailure::is_cancelled and
/// CallFailure::needs_sign_in first and treats the rest as a message to show.
///
/// @tparam R The call's own reasons.
template <class R>
class CallFailure {
 public:
  /// @brief The kinds of failure this call can produce.
  using Reason = R;

  /// @brief Builds a failure.
  ///
  /// @param reason The kind of failure.
  /// @param http_status The HTTP status PS answered with, or 0 when there was
  ///     none.
  /// @param cause The users-context failure behind this one, when there is
  ///     one.
  explicit CallFailure(Reason reason, std::uint16_t http_status = 0,
                       std::optional<users::Unanswered> cause = {}) noexcept
      : reason_(reason), http_status_(http_status), cause_(cause) {}

  /// @brief The kind of failure.
  ///
  /// @return The reason for the failure.
  auto reason() const noexcept -> Reason { return reason_; }
  /// @brief The HTTP status PS answered with; 0 when there was none or it
  /// is not known.
  ///
  /// @return The status, or 0 when there was none or it is not known.
  auto http_status() const noexcept -> std::uint16_t { return http_status_; }
  /// @brief The users-context failure behind this one.
  ///
  /// @return The cause, or nothing when the failure did not come from the
  ///     user's session.
  auto cause() const noexcept -> const std::optional<users::Unanswered>& {
    return cause_;
  }
  /// @brief Whether the caller cancelled, in which case nothing should be
  /// shown.
  ///
  /// @return True when the reason is Reason::cancelled.
  auto is_cancelled() const noexcept -> bool
    requires requires { Reason::cancelled; }
  {
    return reason_ == Reason::cancelled;
  }
  /// @brief Whether the user has to sign in again before the question can be
  /// answered.
  ///
  /// @return True when the cause is that the user is signed out, the sign-in
  ///     has expired, or store access has lapsed.
  auto needs_sign_in() const noexcept -> bool {
    if (!cause_) return false;
    using Cause = users::Unanswered::Reason;
    return cause_->reason == Cause::signed_out ||
           cause_->reason == Cause::sign_in_expired ||
           cause_->reason == Cause::store_access_lapsed;
  }

 private:
  Reason reason_;
  std::uint16_t http_status_{};
  std::optional<users::Unanswered> cause_{};
};

/// @brief Why Catalog::get_streamable_list built no list.
enum class StreamableListReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The user is signed out or must sign in again; see
  /// CallFailure::cause.
  user,
  /// @brief PS could not be reached, reported a server error, or gave no
  /// titles.
  unavailable,
  /// @brief PS refused the request.
  rejected,
  /// @brief The streaming service has no store for the account's region.
  region_unsupported,
  /// @brief PS's reply could not be read, or the titles repeat a product.
  malformed,
};
/// @brief Why Catalog::get_streamable_list failed.
using StreamableListFailure = CallFailure<StreamableListReason>;

/// @brief Why Catalog::library_of read no library.
enum class LibraryReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The user's session could not make the call; see
  /// CallFailure::cause.
  user,
  /// @brief An entitlement reply could not be read.
  malformed,
};
/// @brief Why Catalog::library_of failed.
using LibraryFailure = CallFailure<LibraryReason>;

/// @brief Why Catalog::process_library_and_list built no games.
enum class GamesReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The user is signed out or must sign in again; see
  /// CallFailure::cause.
  user,
  /// @brief No streamable list could be built: PS could not be reached,
  /// reported a server error, or gave no titles.
  unavailable,
  /// @brief No streamable list could be built because PS refused the request.
  rejected,
  /// @brief No streamable list could be built because the streaming service
  /// has no store for the account's region.
  region_unsupported,
  /// @brief No streamable list could be built because PS's reply could not
  /// be read.
  malformed,
};
/// @brief Why Catalog::process_library_and_list failed; also what
/// FetchState::failure reports.
using GamesFailure = CallFailure<GamesReason>;

/// @brief Why Catalog::recently_played listed nothing.
enum class RecentlyPlayedReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The user is signed out, must sign in again, or PSN did not return
  /// the played titles; see CallFailure::cause.
  user,
  /// @brief The user's games could not be built; see GamesReason::unavailable.
  unavailable,
  /// @brief The user's games could not be built; see GamesReason::rejected.
  rejected,
  /// @brief The user's games could not be built; see
  /// GamesReason::region_unsupported.
  region_unsupported,
  /// @brief The user's games could not be built; see GamesReason::malformed.
  malformed,
};
/// @brief Why Catalog::recently_played failed.
using RecentlyPlayedFailure = CallFailure<RecentlyPlayedReason>;

/// @brief Why Catalog::played_games read no history.
enum class PlayedGamesReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The user is signed out, must sign in again, or PSN did not return
  /// a page; see CallFailure::cause.
  user,
  /// @brief A page's next offset did not move past the one asked for, or
  /// fifty pages were read without an end.
  malformed,
};
/// @brief Why Catalog::played_games failed.
using PlayedGamesFailure = CallFailure<PlayedGamesReason>;

/// @brief Why Catalog::cloud_capture found no capture.
enum class CloudCaptureReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The user is signed out, must sign in again, or PSN did not name
  /// the capture; see CallFailure::cause.
  user,
};
/// @brief Why Catalog::cloud_capture failed.
using CloudCaptureFailure = CallFailure<CloudCaptureReason>;

/// @brief Why Catalog::cloud_capture_download read no setting.
enum class CaptureDownloadReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The user is signed out, must sign in again, or PSN did not
  /// answer; see CallFailure::cause.
  user,
};
/// @brief Why Catalog::cloud_capture_download failed.
using CaptureDownloadFailure = CallFailure<CaptureDownloadReason>;

/// @brief Why Catalog::check_title_against_storefront got no answer.
enum class StorefrontReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The user is signed out, or store access has lapsed; see
  /// CallFailure::cause.
  user,
  /// @brief The store could not be reached.
  unavailable,
  /// @brief The title streams on the older PS Now service, which the
  /// storefront is not asked about, or the store refused the question.
  rejected,
  /// @brief The store's reply could not be read.
  malformed,
  /// @brief The store does not know the product.
  not_listed,
};
/// @brief Why Catalog::check_title_against_storefront failed.
using StorefrontFailure = CallFailure<StorefrontReason>;

/// @brief Why Catalog::recheck_ownership got no answer.
enum class OwnershipRecheckReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The user's session could not make the call; see
  /// CallFailure::cause.
  user,
  /// @brief The user's games could not be built; see GamesReason::unavailable.
  unavailable,
  /// @brief The user's games could not be built; see GamesReason::rejected.
  rejected,
  /// @brief The user's games could not be built; see
  /// GamesReason::region_unsupported.
  region_unsupported,
  /// @brief PS's entitlement reply, or the reply the games are built from,
  /// could not be read.
  malformed,
  /// @brief The product is not among the user's games.
  not_listed,
  /// @brief A LegacyCloudStream title was asked about while the library could
  /// not be read.
  ownership_unknown,
};
/// @brief Why Catalog::recheck_ownership failed.
using OwnershipRecheckFailure = CallFailure<OwnershipRecheckReason>;

/// @brief Why Catalog::store_page_of found no page.
enum class StorePageReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The user is signed out or must sign in again; see
  /// CallFailure::cause.
  user,
  /// @brief Neither a product page nor a concept page exists;
  /// CallFailure::http_status carries the store's status when it refused.
  not_listed,
};
/// @brief Why Catalog::store_page_of failed.
using StorePageFailure = CallFailure<StorePageReason>;

/// @brief Why Catalog::details_of a product found no details.
enum class ProductDetailsReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The user is signed out or must sign in again; see
  /// CallFailure::cause.
  user,
  /// @brief The store could not be reached.
  unavailable,
  /// @brief The store refused the question.
  rejected,
  /// @brief The store's reply could not be read.
  malformed,
  /// @brief The store does not know the product.
  not_listed,
};
/// @brief Why Catalog::details_of a product failed.
using ProductDetailsFailure = CallFailure<ProductDetailsReason>;

/// @brief Why Catalog::details_of a concept found no details.
enum class ConceptDetailsReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The store could not be reached.
  unavailable,
  /// @brief The store refused the question.
  rejected,
  /// @brief The store's reply could not be read.
  malformed,
  /// @brief The store does not know the concept.
  not_listed,
};
/// @brief Why Catalog::details_of a concept failed.
using ConceptDetailsFailure = CallFailure<ConceptDetailsReason>;

/// @brief Why Catalog::image_of returned no image.
enum class ImageReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The image host could not be reached.
  unavailable,
  /// @brief PS refused the request, or the image is empty or larger than
  /// ImageHost::largest_bytes.
  rejected,
  /// @brief The reply was empty.
  malformed,
  /// @brief The image does not exist.
  not_listed,
};
/// @brief Why Catalog::image_of failed.
using ImageFailure = CallFailure<ImageReason>;

/// @brief Why Catalog::cloud_access_of gave no answer.
enum class CloudAccessReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The user is signed out or must sign in again; see
  /// CallFailure::cause.
  user,
};
/// @brief Why Catalog::cloud_access_of failed.
using CloudAccessFailure = CallFailure<CloudAccessReason>;

/// @brief Why a launch check could not be completed; carried by
/// LaunchRefusal::Kind::check_unavailable.
enum class LaunchCheckReason : std::uint8_t {
  /// @brief The caller's stop token fired.
  cancelled,
  /// @brief The user's session could not make the call; see
  /// CallFailure::cause.
  user,
  /// @brief PS could not be reached or reported a server error.
  unavailable,
  /// @brief PS refused the request.
  rejected,
  /// @brief The streaming service has no store for the account's region.
  region_unsupported,
  /// @brief PS's reply could not be understood.
  malformed,
  /// @brief The product is not among the user's games.
  not_listed,
  /// @brief The library could not be read, so ownership cannot be told.
  ownership_unknown,
  /// @brief The restriction list of excluded PS5 titles was never loaded, or
  /// is older than 24 hours and could not be refreshed, so no PS5 title may
  /// launch; carries the HTTP status of the failed fetch, if any.
  restriction_list_unavailable,
};
/// @brief Why a launch check could not be completed.
using LaunchCheckFailure = CallFailure<LaunchCheckReason>;

/// @brief Why Catalog::caches listed nothing.
enum class CachesReason : std::uint8_t {
  /// @brief The record store could not be listed.
  storage,
};
/// @brief Why Catalog::caches failed.
using CachesFailure = CallFailure<CachesReason>;

/// @brief Why Catalog::cache_entries listed nothing.
enum class CacheEntriesReason : std::uint8_t {
  /// @brief The record store could not be read.
  storage,
};
/// @brief Why Catalog::cache_entries failed.
using CacheEntriesFailure = CallFailure<CacheEntriesReason>;

/// @brief Why Catalog::clear_cache did not empty the cache.
enum class ClearCacheReason : std::uint8_t {
  /// @brief A record could not be listed or erased.
  storage,
};
/// @brief Why Catalog::clear_cache failed.
using ClearCacheFailure = CallFailure<ClearCacheReason>;

/// @brief Why Catalog::clear_caches did not empty every cache.
enum class ClearCachesReason : std::uint8_t {
  /// @brief A record could not be listed or erased.
  storage,
};
/// @brief Why Catalog::clear_caches failed.
using ClearCachesFailure = CallFailure<ClearCachesReason>;

}  // namespace libchiaking::catalog
