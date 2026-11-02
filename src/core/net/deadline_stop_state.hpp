/// @file
/// @brief Declares the deadlines that bound waits on a remote party (PSN push
/// notifications, signalling, hole punching) and the stop state that turns
/// such a deadline into cancellation.
///
/// Remote waits are cancelled through ordinary stop tokens. DeadlineStopState
/// wraps a caller's token and also stops when a monotonic deadline passes,
/// remembering that the stop came from time running out so the caller can
/// report a timeout rather than a cancellation.
#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace libchiaking::net {

/// @brief How long a wait on a remote party may last: a fixed instant, a
/// fresh timeout for every wait, or both.
///
/// Built implicitly from an absolute instant, or with per_wait() for a timeout
/// that restarts at each wait. arm() resolves it to the instant one wait must
/// end by. Plain data, cheap to copy, safe to share read-only between threads.
struct RemoteWaitDeadline {
  /// @brief Bounds every wait by one absolute instant.
  ///
  /// @param absolute_microseconds The instant waits must end by, in
  ///     microseconds on the platform's monotonic clock
  ///     (platform::ClockProvider::now_microseconds); the maximum value
  ///     means no fixed bound.
  constexpr RemoteWaitDeadline(std::uint64_t absolute_microseconds) noexcept
      : absolute(absolute_microseconds) {}

  /// @brief Bounds each wait by a timeout that starts when that wait is
  /// armed, with no fixed instant.
  ///
  /// @param timeout_microseconds Length of each wait, in microseconds; 0
  ///     leaves waits unbounded.
  /// @return A deadline whose absolute instant is the maximum value and whose
  ///     wait_timeout is `timeout_microseconds`.
  static constexpr auto per_wait(std::uint64_t timeout_microseconds) noexcept
      -> RemoteWaitDeadline {
    RemoteWaitDeadline deadline{std::numeric_limits<std::uint64_t>::max()};
    deadline.wait_timeout = timeout_microseconds;
    return deadline;
  }

  /// @brief Resolves the deadline for one wait starting now.
  ///
  /// @param clock The monotonic clock the instants are read on.
  /// @return `absolute` when wait_timeout is 0; otherwise the earlier of
  ///     `absolute` and now plus wait_timeout, saturating at the maximum
  ///     value. Microseconds on `clock`.
  auto arm(const platform::ClockProvider& clock) const noexcept
      -> std::uint64_t;

  /// @brief The instant every wait must end by, in microseconds on the
  /// monotonic clock; the maximum value means none.
  std::uint64_t absolute;
  /// @brief Length of each individual wait, in microseconds, counted from
  /// arm(); 0 means each wait is bounded by `absolute` alone.
  std::uint64_t wait_timeout{};
};

/// @brief A stop state that stops when its parent token does or when a
/// monotonic deadline passes, whichever comes first.
///
/// Callers make one per bounded wait, share it through a StopToken, and
/// afterwards ask timed_out() whether a stop was the deadline's doing. It holds
/// a pointer to the clock, which must outlive it. Every query only reads, so
/// tokens on any thread may poll it concurrently.
class DeadlineStopState final : public platform::StopState {
 public:
  /// @brief Watches a parent token and a deadline.
  ///
  /// @param parent The caller's token; stopping it stops this state.
  /// @param clock The monotonic clock the deadline is read on; must outlive
  ///     this object.
  /// @param deadline_microseconds The instant at which the state stops, in
  ///     microseconds on `clock`.
  DeadlineStopState(platform::StopToken parent,
                    const platform::ClockProvider& clock,
                    std::uint64_t deadline_microseconds) noexcept;

  /// @brief Whether waits watching this state should stop.
  ///
  /// @return True once the parent token stops or the deadline has passed.
  bool stop_requested() const noexcept override;

  /// @brief Whether the stop, if any, was caused by a deadline.
  ///
  /// A parent whose own stop came from a deadline counts as timed out. A
  /// parent cancelled by its caller does not, even after this deadline has
  /// also passed.
  ///
  /// @return True when the parent reports an expired deadline, or when the
  ///     parent has not stopped and this deadline has passed.
  bool timed_out() const noexcept;

  /// @brief Carries the timeout provenance through tokens composed on top
  /// of this state.
  ///
  /// @return The same answer as timed_out().
  bool deadline_expired() const noexcept override { return timed_out(); }

 private:
  /// @brief The caller's token, whose stop is passed through.
  platform::StopToken parent_;
  /// @brief The monotonic clock the deadline is read on; never null.
  const platform::ClockProvider* clock_;
  /// @brief The instant at which this state stops; fixed at construction.
  timing::Deadline deadline_;
};

}  // namespace libchiaking::net
