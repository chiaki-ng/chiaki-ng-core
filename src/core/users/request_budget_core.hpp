/// @file
/// @brief Defines RequestBudget::Core, the pacer and waiter behind the public
/// RequestBudget.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/request_pacer.hpp"

#include <libchiaking/detail/users/request_budget.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <memory>

namespace libchiaking::users {

/// @brief Owns the pacer every sharing service waits on. Each wait gets a
/// waiter of its own, so threads of different services never share one.
class RequestBudget::Core {
 public:
  /// @brief Builds the state with a stock sleeping waiter for each wait.
  ///
  /// @param threading Makes the lock and the sleeper. Must outlive the core.
  /// @param clock The monotonic clock. Must outlive the core.
  Core(platform::ThreadingProvider& threading,
       const platform::ClockProvider& clock);
  /// @brief Builds the state around waiters of the caller's, as tests do.
  ///
  /// @param threading Makes the lock. Must outlive the core.
  /// @param clock The monotonic clock. Must outlive the core.
  /// @param waiters Makes the waiter each wait pauses on.
  Core(platform::ThreadingProvider& threading,
       const platform::ClockProvider& clock, net::http::WaiterFactory waiters);

  /// @brief The pacer every sharing service waits on.
  ///
  /// @return The pacer, living as long as the core.
  auto pacer() noexcept -> RequestPacer& { return pacer_; }

  /// @brief The pacing RequestBudget documents.
  ///
  /// @return burst_requests, window_requests, window and cooldown.
  static constexpr auto limits() noexcept -> RequestPacer::Limits {
    return {.burst = burst_requests,
            .window_requests = window_requests,
            .window = window,
            .cooldown = cooldown};
  }

 private:
  /// @brief Each account's lane.
  RequestPacer pacer_;
};

}  // namespace libchiaking::users
