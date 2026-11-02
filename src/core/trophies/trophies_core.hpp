/// @file
/// @brief Defines Trophies::Core, the working parts behind the public
/// Trophies service.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/trophies/usecases/read_trophies.hpp"
#include "src/core/users/request_budget_core.hpp"

#include <libchiaking/detail/trophies/trophies.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <utility>

namespace libchiaking::trophies {

/// @brief Holds the use case Trophies forwards to, paced on the budget it
/// is given.
///
/// Built by Trophies with a factory of net::http::SleepWaiter; tests build one
/// directly with waiters of their own.
class Trophies::Core {
 public:
  /// @brief Builds the working parts.
  ///
  /// @param http Sends every request. Must outlive the core.
  /// @param records Where answers are kept. Must outlive the core.
  /// @param threading Makes locks and waits. Must outlive the core.
  /// @param clock The monotonic clock requests are paced on. Must outlive
  ///     the core.
  /// @param wall_clock Device time answers are stamped with. Must outlive
  ///     the core.
  /// @param diagnostics Receives request logs; may be null.
  /// @param waiters Makes a waiter for each fetch's retries.
  /// @param budget The request budget every request is paced on. Must
  ///     outlive the core.
  Core(platform::HttpProvider& http, platform::RecordStore& records,
       platform::ThreadingProvider& threading,
       const platform::ClockProvider& clock,
       const platform::ClockProvider& wall_clock,
       platform::DiagnosticsProvider* diagnostics,
       net::http::WaiterFactory waiters, users::RequestBudget& budget)
      : read_({http, records, threading, clock, wall_clock, std::move(waiters),
               diagnostics},
              budget.core().pacer(), Freshness{kept_for}) {}

  /// @brief The use case every call goes to.
  ///
  /// @return The use case, living as long as the core.
  auto read() noexcept -> usecases::ReadTrophies& { return read_; }

 private:
  /// @brief The use case.
  usecases::ReadTrophies read_;
};

}  // namespace libchiaking::trophies
