/// @file
/// @brief Declares PollWaiter, the wait the cloud and console code pauses
/// with between retries, which may also end early when a socket becomes
/// readable.
#pragma once

#include "libchiaking/platform/net/poller.hpp"
#include "src/core/net/transport/http/client.hpp"

#include <span>

namespace libchiaking::net::http {

/// @brief A cancellable wait that can also watch sockets.
///
/// Tests substitute their own to make waits instant. Implementations decide
/// their own thread rules; SleepWaiter's are the usual ones.
class PollWaiter : public Waiter {
 public:
  /// @brief Waits for a time, or until one of the sockets is readable.
  ///
  /// The base implementation ignores the sockets and waits the whole time
  /// with wait_milliseconds().
  ///
  /// @param sources The sockets to watch; may be empty.
  /// @param timeout_milliseconds The longest to wait, in milliseconds.
  /// @param stop Ends the wait early.
  /// @return True when the wait ended without a stop request; false when
  ///     `stop` fired or the timeout is negative.
  [[nodiscard]] virtual auto wait_readable(
      std::span<const platform::Pollable* const> sources,
      int timeout_milliseconds, const platform::StopToken& stop) -> bool {
    static_cast<void>(sources);
    return wait_milliseconds(timeout_milliseconds, stop);
  }
};

}  // namespace libchiaking::net::http
