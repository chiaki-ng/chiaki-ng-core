/// @file
/// @brief Declares RequestBudget, the pace each PSN account's requests are
/// held to, shared by every service that asks PSN on the account's behalf.
#pragma once

#include "libchiaking/detail/users/account_id.hpp"

#include <chrono>
#include <cstdint>
#include <memory>

namespace libchiaking::platform {
class ClockProvider;
class ThreadingProvider;
}  // namespace libchiaking::platform

namespace libchiaking::users {

/// @brief How fast each PSN account may send requests, counted once for every
/// service that shares it.
///
/// PSN rate limits an account, not a screen, so services that read PSN for
/// the same account, such as the catalog's play history and the trophy list,
/// should draw on one budget. Each account gets its own lane: at most
/// burst_requests start in any second, a request waits for its turn rather
/// than failing, and at most window_requests start in any window. A request
/// beyond the window's budget is refused as rate limited rather than sent.
/// When PSN answers 429 for an account, its requests through every sharing
/// service are refused for cooldown.
///
/// Safe to share between threads. Must outlive every service it is given to.
/// @ingroup api_users
class RequestBudget {
 public:
  /// @brief Most requests that start for one account in any second.
  static constexpr std::uint32_t burst_requests = 5;
  /// @brief Most requests that start for one account in any window.
  static constexpr std::uint32_t window_requests = 300;
  /// @brief The window window_requests is counted over.
  static constexpr std::chrono::minutes window{15};
  /// @brief How long an account's requests are refused after PSN answered
  /// 429.
  static constexpr std::chrono::minutes cooldown{15};

  /// @brief Builds a budget with no account seen yet.
  ///
  /// @param threading Makes the budget's lock and the sleeper waits happen
  ///     on. Must outlive the budget.
  /// @param clock The monotonic clock the lanes measure on. Must outlive the
  ///     budget.
  RequestBudget(platform::ThreadingProvider& threading,
                const platform::ClockProvider& clock);
  /// @brief Not copyable; services share one budget by reference.
  RequestBudget(const RequestBudget&) = delete;
  /// @brief Not copy-assignable; services share one budget by reference.
  auto operator=(const RequestBudget&) -> RequestBudget& = delete;
  /// @brief Releases the budget.
  ~RequestBudget();

  /// @brief Drops an account's lane, cooldown included, for sign-out or
  /// account removal.
  ///
  /// @param account The account to forget.
  void forget(AccountId account);

  /// @brief The budget's internal state. Defined inside the library; not for
  /// frontends.
  class Core;

  /// @brief Wraps internal state in a budget. For the library's own use.
  ///
  /// @param core The state to own. Must not be null.
  explicit RequestBudget(std::unique_ptr<Core> core) noexcept;

  /// @brief Reaches the internal state. For the library's own use.
  ///
  /// @return The state, which lives as long as this budget.
  auto core() const noexcept -> Core& { return *core_; }

 private:
  /// @brief The internal state; never null.
  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::users
