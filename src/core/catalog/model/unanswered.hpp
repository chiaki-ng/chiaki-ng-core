/// @file
/// @brief Declares Unanswered, why a catalog adapter or use case could not
/// answer a question, before a Catalog call narrows it to its own failure.
#pragma once

#include "libchiaking/detail/catalog/call_failure.hpp"
#include "libchiaking/detail/catalog/cloud_access.hpp"
#include "libchiaking/detail/users/ps_user.hpp"

#include <magic_enum/magic_enum.hpp>

#include <cstdint>
#include <optional>

namespace libchiaking::catalog {

/// @brief Why a catalog question went unanswered inside the catalog.
///
/// The adapters and use cases report one of these. Each Catalog call hands
/// its caller a CallFailure naming only the reasons that call can produce;
/// Unanswered::as builds it.
class Unanswered {
 public:
  /// @brief The kind of failure.
  enum class Reason : std::uint8_t {
    /// @brief The caller's stop token fired.
    cancelled,
    /// @brief The user's session could not make the call; Unanswered::cause
    /// says why.
    user,
    /// @brief PS could not be reached or reported a server error.
    unavailable,
    /// @brief PS refused the request, or the question does not apply to
    /// the title.
    rejected,
    /// @brief The streaming service has no store for the account's region.
    region_unsupported,
    /// @brief PS's reply could not be understood.
    malformed,
    /// @brief The product, page or image does not exist, or the product is
    /// not among the user's games.
    not_listed,
    /// @brief The record store failed to read, write, list or erase.
    storage,
    /// @brief The library could not be read, so ownership cannot be told.
    ownership_unknown,
    /// @brief The restriction list of excluded PS5 titles was never loaded,
    /// or is older than 24 hours and could not be refreshed, so no PS5
    /// title may launch; carries the HTTP status of the failed fetch, if
    /// any.
    restriction_list_unavailable,
  };

  /// @brief Builds a failure the catalog found itself.
  ///
  /// @param reason The kind of failure.
  /// @param http_status The HTTP status PS answered with, or 0 when there
  ///     was none.
  explicit Unanswered(Reason reason, std::uint16_t http_status = 0) noexcept
      : reason_(reason), http_status_(http_status) {}

  /// @brief Builds a failure passed on from the users context.
  ///
  /// The reason becomes Unanswered::Reason::cancelled when the cause is a
  /// cancellation and Unanswered::Reason::user otherwise; the HTTP status is
  /// taken from the cause.
  ///
  /// @param cause Why the user's session could not make the call.
  explicit Unanswered(users::Unanswered cause) noexcept
      : reason_(cause.reason == users::Unanswered::Reason::cancelled
                    ? Reason::cancelled
                    : Reason::user),
        http_status_(cause.http_status),
        cause_(cause) {}

  /// @brief Builds the failure for a stop token that fired.
  ///
  /// @return A failure whose reason is Unanswered::Reason::cancelled.
  static auto cancelled() noexcept -> Unanswered {
    return Unanswered{Reason::cancelled};
  }

  /// @brief The kind of failure.
  ///
  /// @return The reason for the failure.
  auto reason() const noexcept -> Reason { return reason_; }
  /// @brief Narrows this failure to the reasons one Catalog call can give.
  ///
  /// The reason of the same name is kept, with the HTTP status and the cause.
  ///
  /// @tparam Narrow The call's reason enumeration.
  /// @param otherwise The reason given when the call's enumeration has none
  ///     of this name.
  /// @return The call's failure.
  template <class Narrow>
  auto as(Narrow otherwise) const -> CallFailure<Narrow> {
    const auto named =
        magic_enum::enum_cast<Narrow>(magic_enum::enum_name(reason_));
    return CallFailure<Narrow>{named.value_or(otherwise), http_status_, cause_};
  }
  /// @brief The HTTP status PS answered with; 0 when there was none or it
  /// is not known.
  ///
  /// @return The status, or 0 when there was none or it is not known.
  auto http_status() const noexcept -> std::uint16_t { return http_status_; }
  /// @brief The users-context failure behind this one; set only when it was
  /// built from one.
  ///
  /// @return The cause, or nothing when this failure was not built from one.
  auto cause() const noexcept -> const std::optional<users::Unanswered>& {
    return cause_;
  }
  /// @brief Whether the caller cancelled, in which case nothing should be
  /// shown.
  ///
  /// @return True when the reason is Unanswered::Reason::cancelled; false
  /// otherwise.
  auto is_cancelled() const noexcept -> bool {
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

  /// @brief What this failure of a LegacyCloudStream walk says about the
  /// account's access to the service.
  ///
  /// @return access_rejected when the service refused the account,
  ///     sign_in_expired when the user has to sign in again,
  ///     region_unsupported when the region has no store, and unavailable
  ///     otherwise.
  auto cloud_availability() const noexcept -> CloudAccess::Availability {
    using Availability = CloudAccess::Availability;
    using Cause = users::Unanswered::Reason;
    if (cause_ && cause_->reason == Cause::old_service_refused)
      return Availability::access_rejected;
    if (needs_sign_in()) return Availability::sign_in_expired;
    if (reason_ == Reason::region_unsupported)
      return Availability::region_unsupported;
    return Availability::unavailable;
  }

 private:
  /// @brief The kind of failure.
  Reason reason_;
  /// @brief The HTTP status PS answered with; 0 when there was none.
  std::uint16_t http_status_{};
  /// @brief The users-context failure behind this one, when there is one.
  std::optional<users::Unanswered> cause_{};
};

}  // namespace libchiaking::catalog
