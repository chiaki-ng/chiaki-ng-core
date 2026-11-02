/// @file
/// @brief Declares MembershipCache, the per-user memory of the last
/// PlayStation Plus membership read from PSN.
#pragma once

#include "src/core/psn/graphql/check.hpp"
#include "src/core/psn/result_cache.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/users/membership.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <chrono>
#include <optional>
#include <utility>

namespace libchiaking::users {

/// @brief The outcome of reading a user's membership from PSN's GraphQL
/// subscriptions query: the membership, or the check that failed.
using MembershipCheck = Expected<MembershipAnswer, psn::graphql::CheckError>;

/// @brief Holds one user's last membership answer for an hour and makes
/// concurrent readers share one request.
///
/// A thin wrapper over a single-entry psn::ResultCache. A reused answer comes
/// back with MembershipAnswer::cached set; a freshly fetched one with it clear
/// and MembershipAnswer::fetched_at_ms stamped by the wall clock. Failures are
/// never kept. Ages are measured on the monotonic clock.
///
/// Owned by PsUser::Core and cleared when the user leaves the registry or when
/// a cloud server reservation is refused for want of PlayStation Plus
/// (MembershipCalls::membership_rejected_by_service).
/// Every member is safe to call from several threads.
class MembershipCache {
 public:
  /// @brief The request that reads the membership from PSN when nothing
  /// usable is held.
  using Fetch = psn::ResultCache<MembershipAnswer>::Fetch;

  /// @brief How long a fetched membership is reused: one hour.
  static constexpr std::chrono::microseconds lifetime{std::chrono::hours{1}};

  /// @brief Builds an empty cache.
  ///
  /// @param threading Provider of the cache's lock and of the waits readers
  ///     share a request through. Must outlive the cache.
  /// @param clock Monotonic clock that ages the held answer. Must outlive the
  ///     cache.
  /// @param wall Wall clock that stamps when an answer was fetched. Must
  ///     outlive the cache.
  MembershipCache(platform::ThreadingProvider& threading,
                  const platform::ClockProvider& clock,
                  const platform::ClockProvider& wall)
      : cache_(threading, clock, wall, lifetime) {}

  /// @brief Returns the held membership when it will do, or fetches it.
  ///
  /// A fresh held answer is returned when `remembered_from` is Tier::none or
  /// the held tier is at least `remembered_from`. Otherwise, when another
  /// thread is already fetching, this one waits for that fetch; else it runs
  /// `fetch` itself and keeps a successful answer, unless the cache was
  /// cleared meanwhile.
  ///
  /// Blocks for as long as `fetch` takes, its own or another thread's.
  ///
  /// @param stop Cancels the wait or the fetch.
  /// @param remembered_from The lowest tier a held answer must show to be
  ///     reused; Tier::none reuses any fresh answer.
  /// @param fetch Reads the membership from PSN.
  /// @return The membership, held or fetched.
  /// @retval psn::graphql::CheckError The fetch failed, or a cancellation
  ///     when `stop` fired first.
  [[nodiscard]] auto get(const platform::StopToken& stop, Tier remembered_from,
                         const Fetch& fetch) -> MembershipCheck {
    const auto accept = [remembered_from](const MembershipAnswer& answer) {
      return remembered_from == Tier::none ||
             answer.membership.tier() >= remembered_from;
    };
    return cache_.get({}, stop, accept, fetch);
  }

  /// @brief Returns the held membership while it is fresh, without asking
  /// PSN.
  ///
  /// @return A copy with MembershipAnswer::cached set, or nothing when none
  ///     is held or it is older than lifetime.
  [[nodiscard]] auto remembered() -> std::optional<MembershipAnswer> {
    return cache_.peek({});
  }
  /// @brief Returns the held membership however old it is, for diagnostics.
  ///
  /// @return A copy of the held answer and whether it is still fresh, or
  ///     nothing when none is held.
  [[nodiscard]] auto held()
      -> std::optional<std::pair<MembershipAnswer, bool>> {
    return cache_.held({});
  }
  /// @brief Forgets the held membership; a fetch still in flight is not
  /// kept when it lands.
  void clear() { cache_.clear(); }

 private:
  /// @brief The single-entry cache, keyed by the empty string.
  psn::ResultCache<MembershipAnswer> cache_;
};

}  // namespace libchiaking::users
