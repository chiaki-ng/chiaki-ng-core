/// @file
/// @brief Declares CloudAccess, what the LegacyCloudStream service showed an
/// account when it was last asked.
#pragma once

#include "libchiaking/detail/catalog/streamable_list.hpp"
#include "libchiaking/detail/catalog/wall_time.hpp"
#include "libchiaking/detail/users/account_id.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

namespace libchiaking::catalog {

/// @brief Whether an account can reach the older LegacyCloudStream streaming
/// service, as of one check.
///
/// The value is a snapshot. It is good for CloudAccess::lifetime and belongs to
/// one account.
///
/// @see Catalog::cloud_access_of
class CloudAccess {
 public:
  /// @brief Wall-clock time in milliseconds.
  using Time = catalog::Time;

  /// @brief What the service's answer means for the account.
  enum class Availability : std::uint8_t {
    /// @brief The service answered and its shelves were read.
    available,
    /// @brief The user has to sign in again before the service can be
    /// asked.
    sign_in_expired,
    /// @brief PS refused this account access to the service.
    access_rejected,
    /// @brief The service has no store for the account's region.
    region_unsupported,
    /// @brief The service could not be reached or its reply could not be
    /// read.
    unavailable,
  };

  /// @brief What a successful walk of the service's shelves returned.
  struct Listing {
    /// @brief Entries the walk returned, before any filtering.
    std::size_t title_count{};
    /// @brief Whether every shelf and page was read.
    bool complete{};
    /// @brief Where the entries came from.
    ListOrigin origin{};

    /// @brief Compares every field.
    friend auto operator==(const Listing&, const Listing&) -> bool = default;
  };

  /// @brief How long a check may be reused before the service is asked again.
  static constexpr std::chrono::minutes lifetime{15};

  /// @brief Builds the answer for a walk that succeeded.
  ///
  /// @param account The account that was checked.
  /// @param listing What the walk returned.
  /// @param checked_at When the walk finished.
  /// @return Access with CloudAccess::Availability::available and the
  ///     listing.
  static auto available(AccountId account, Listing listing,
                        Time checked_at) noexcept -> CloudAccess {
    return CloudAccess{account, listing, checked_at};
  }

  /// @brief Builds the answer for a walk that failed.
  ///
  /// @param account The account that was checked.
  /// @param availability Why the walk gave no listing.
  /// @param checked_at When the walk failed.
  /// @return Access without a listing, or nothing when availability is
  ///     CloudAccess::Availability::available, which needs a listing.
  [[nodiscard]] static auto without_listing(AccountId account,
                                            Availability availability,
                                            Time checked_at) noexcept
      -> std::optional<CloudAccess> {
    if (availability == Availability::available) return std::nullopt;
    return CloudAccess{account, availability, checked_at};
  }

  /// @brief The account the check was made for.
  ///
  /// @return The account id given when the check was made.
  auto account() const noexcept -> AccountId { return account_; }
  /// @brief What the service's answer means for the account.
  ///
  /// @return The availability, which is CloudAccess::Availability::available
  /// exactly when
  ///     CloudAccess::listing is non-null.
  auto availability() const noexcept -> Availability {
    if (const auto* refused = std::get_if<Availability>(&answer_))
      return *refused;
    return Availability::available;
  }
  /// @brief Whether the service answered and its shelves were read.
  ///
  /// @return True when the service answered and a listing is held; false when
  /// the
  ///     check failed.
  auto is_available() const noexcept -> bool {
    return std::holds_alternative<Listing>(answer_);
  }
  /// @brief What the walk returned.
  ///
  /// @return The listing, present exactly when the service is available, or
  ///     null otherwise. The pointer lives as long as this value.
  auto listing() const noexcept -> const Listing* {
    return std::get_if<Listing>(&answer_);
  }
  /// @brief Whether the service answered but showed the account no titles at
  /// all.
  ///
  /// @return True when a listing is held and its title count is zero; false
  /// when the
  ///     listing has titles or the service was not available.
  auto has_no_titles() const noexcept -> bool {
    const auto* shown = listing();
    return shown != nullptr && shown->title_count == 0;
  }
  /// @brief When the check was made.
  ///
  /// @return The time given when the check was made.
  auto checked_at() const noexcept -> Time { return checked_at_; }
  /// @brief Whether the check is still recent enough to reuse.
  ///
  /// @param now The current wall-clock time.
  /// @return True from CloudAccess::checked_at until CloudAccess::lifetime
  ///     has passed; false for a time before the check.
  auto is_fresh_at(Time now) const noexcept -> bool {
    return is_fresh(checked_at_, lifetime, now);
  }

  /// @brief Compares account, availability, listing and check time.
  friend auto operator==(const CloudAccess&, const CloudAccess&)
      -> bool = default;

 private:
  using Answer = std::variant<Listing, Availability>;

  CloudAccess(AccountId account, Answer answer, Time checked_at) noexcept
      : account_(account), answer_(answer), checked_at_(checked_at) {}

  AccountId account_;
  Answer answer_;
  Time checked_at_;
};

}  // namespace libchiaking::catalog
