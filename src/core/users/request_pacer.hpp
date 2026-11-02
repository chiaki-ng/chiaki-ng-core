/// @file
/// @brief Declares RequestPacer, which spaces out an account's requests to
/// PSN and holds them back after PSN rate limited it.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/utils/timing/token_bucket.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/users/account_id.hpp>
#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>

namespace libchiaking::users {

/// @brief Paces each account's requests on the monotonic clock.
///
/// Each account has its own lane: a burst bucket that makes a request wait
/// for its turn, a window budget that refuses a request once spent, and a
/// cooldown after PSN answered 429. Safe to share between threads; waits
/// happen outside the lock, each on a waiter of its own.
class RequestPacer {
 public:
  /// @brief The pacing an account gets. Plain data.
  struct Limits {
    /// @brief Most requests started in any second.
    std::uint32_t burst{};
    /// @brief Most requests started in any window.
    std::uint32_t window_requests{};
    /// @brief The window window_requests is counted over; whole seconds.
    std::chrono::seconds window{};
    /// @brief How long requests are refused after PSN answered 429.
    std::chrono::seconds cooldown{};
  };

  /// @brief Builds a pacer with no account seen yet.
  ///
  /// @param threading Makes the lock guarding the lanes. Must outlive the
  ///     pacer.
  /// @param clock The monotonic clock the lanes measure on. Must outlive
  ///     the pacer.
  /// @param waiters Makes the waiter one admit() pauses on until its turn,
  ///     so no two threads ever wait on the same one.
  /// @param limits The pacing each account gets.
  RequestPacer(platform::ThreadingProvider& threading,
               const platform::ClockProvider& clock,
               net::http::WaiterFactory waiters, Limits limits);

  /// @brief Waits for the account's next turn and takes it.
  ///
  /// Blocks while the burst is spent, through a waiter made for this call;
  /// returns at once otherwise.
  ///
  /// @param account The account the request is for.
  /// @param stop Cancels the wait.
  /// @return Nothing once the request may be sent.
  /// @retval users::Unanswered::Reason::rate_limited The account is cooling
  ///     down after a 429, or its window budget is spent.
  /// @retval users::Unanswered::Reason::cancelled The stop token fired.
  [[nodiscard]] auto admit(AccountId account, const platform::StopToken& stop)
      -> Expected<void, users::Unanswered>;
  /// @brief Records that PSN answered 429 for the account, starting its
  /// cooldown now.
  ///
  /// @param account The account PSN rate limited.
  void throttled(AccountId account);
  /// @brief Drops the account's lane, cooldown included.
  ///
  /// @param account The account to forget.
  void forget(AccountId account);

 private:
  /// @brief One account's pacing state.
  struct Lane {
    /// @brief The per-second burst.
    timing::TokenBucket burst;
    /// @brief The window budget; a request costs window_cost() tokens.
    timing::TokenBucket window;
    /// @brief When the cooldown ends, in monotonic microseconds; empty when
    /// not cooling down.
    std::optional<std::uint64_t> cooling_until_us;
  };

  /// @brief Finds the account's lane, starting a full one when new.
  ///
  /// @param account The account.
  /// @param now_us The current monotonic time.
  /// @return The lane; valid while the lock is held.
  auto lane_of(AccountId account, std::uint64_t now_us) -> Lane&;
  /// @brief Takes a turn if one is free.
  ///
  /// @param account The account.
  /// @return 0 when the turn was taken, else the microseconds to wait.
  /// @retval users::Unanswered::Reason::rate_limited The account is cooling
  ///     down or its window budget is spent.
  [[nodiscard]] auto turn(AccountId account)
      -> Expected<std::uint64_t, users::Unanswered>;
  /// @brief The window tokens one request costs.
  ///
  /// @return The window's length in seconds; the window refills
  ///     window_requests tokens a second.
  auto window_cost() const noexcept -> std::uint64_t;

  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief Makes the waiter each waiting admit() pauses on.
  net::http::WaiterFactory waiters_;
  /// @brief The pacing each account gets.
  Limits limits_;
  /// @brief Guards lanes_.
  platform::Mutex mutex_;
  /// @brief Each account's lane, by account number.
  std::map<std::uint64_t, Lane> lanes_;
};

}  // namespace libchiaking::users
