/// @file
/// @brief Declares SleepWaiter, the stock PollWaiter: sleeps in short slices
/// so that a stop is noticed quickly, and polls sockets through the network
/// provider when it has one.
#pragma once

#include "libchiaking/platform/net/poller.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/transport/http/poll_waiter.hpp"

#include <chrono>
#include <memory>
#include <span>

namespace libchiaking::net::http {

/// @brief Waits on a monotonic clock through a threading provider's
/// platform::Sleeper, optionally watching sockets through a poller of its own.
///
/// Without a poller, because none was asked for or the provider could not
/// make one, socket waits fall back to plain sleeps. One thread may wait at
/// a time; wake() may be called from any thread. The threading provider, the
/// clock and the network provider must outlive the waiter.
class SleepWaiter final : public PollWaiter {
 public:
  /// @brief Makes a waiter without a poller, which only sleeps.
  ///
  /// @param threading Makes the sleeper the waiter sleeps on.
  /// @param clock The monotonic clock sleeps are measured on.
  SleepWaiter(platform::ThreadingProvider& threading, const platform::ClockProvider& clock);

  /// @brief Makes a waiter with a poller from a network provider.
  ///
  /// A failure to make the poller is not reported; the waiter then only
  /// sleeps.
  ///
  /// @param threading Makes the sleeper the waiter sleeps on.
  /// @param clock The monotonic clock sleeps are measured on.
  /// @param network The provider to make the poller with.
  SleepWaiter(platform::ThreadingProvider& threading, const platform::ClockProvider& clock,
              platform::NetworkProvider& network);

  /// @brief Sleeps for a time, in slices of sleep_slice, checking `stop`
  /// between them.
  ///
  /// Not ended early by wake() or by sockets.
  ///
  /// @param milliseconds How long to sleep.
  /// @param stop Ends the sleep early, within sleep_slice.
  /// @return True when the whole time passed; false when `stop` fired or
  ///     `milliseconds` is negative.
  auto wait_milliseconds(int milliseconds, const platform::StopToken& stop)
      -> bool override;

  /// @brief Waits for a time, ending early when a socket becomes readable
  /// or wake() is called.
  ///
  /// Without a poller or sockets, or when the poll fails, sleeps the whole
  /// time instead.
  ///
  /// @param sources The sockets to watch; must belong to the provider the
  ///     waiter was made with.
  /// @param timeout_milliseconds The longest to wait.
  /// @param stop Ends the wait early.
  /// @return True when the wait ended without a stop request; false when
  ///     `stop` fired or the timeout is negative.
  auto wait_readable(std::span<const platform::Pollable* const> sources,
                     int timeout_milliseconds, const platform::StopToken& stop)
      -> bool override;

  /// @brief Waits up to a time for one socket's readiness, or for wake().
  ///
  /// Not cancellable by a stop token; callers keep the timeout short.
  /// Without a poller it sleeps at most idle_slice, however long the
  /// timeout, so callers loop. Poll failures are ignored.
  ///
  /// @param pollable The socket to watch, or null to only wait.
  /// @param interest The readiness to wait for; none only waits.
  /// @param timeout The longest to wait; zero or negative returns at once.
  void wait(const platform::Pollable* pollable, platform::PollEvent interest,
            std::chrono::nanoseconds timeout);

  /// @brief Ends a wait() or wait_readable() in progress on another thread.
  ///
  /// Does nothing without a poller. A wake with no wait in progress makes
  /// the next poll return at once.
  void wake() noexcept;

 private:
  /// @brief Sleeps in slices until a time has passed or `stop` fires.
  ///
  /// @param duration How long to sleep.
  /// @param stop Ends the sleep early.
  /// @return True when the whole time passed; false when `stop` fired.
  [[nodiscard]] auto wait_for(std::chrono::nanoseconds duration,
                              const platform::StopToken& stop) -> bool;

  /// @brief The longest single sleep, 10 ms; bounds how late a stop is
  /// noticed.
  static constexpr std::chrono::milliseconds sleep_slice{10};
  /// @brief The longest wait() sleeps without a poller, 1 ms.
  static constexpr std::chrono::milliseconds idle_slice{1};

  /// @brief The monotonic clock sleeps are measured on.
  const platform::ClockProvider* clock_;
  /// @brief Sleeps for a slice at a time; held by pointer so the waiter
  /// stays movable.
  std::unique_ptr<platform::Sleeper> sleeper_;
  /// @brief The poller, or null when the waiter only sleeps.
  std::unique_ptr<platform::Poller> poller_;
};

}  // namespace libchiaking::net::http
