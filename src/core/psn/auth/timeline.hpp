/// @file
/// @brief Declares Timeline, the pair of clocks the credential caches measure
/// time with.
///
/// Token lifetimes are measured on the monotonic clock so that a change to the
/// device's wall clock neither expires nor revives a token; the wall clock is
/// consulted only to report times in diagnostics and to compare against expiry
/// instants that PS or the app states in wall-clock time.
#pragma once

#include "libchiaking/platform/runtime/time_units.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <cstdint>

namespace libchiaking::psn::auth {

/// @brief A monotonic clock paired with a wall clock, with the conversion
/// between them.
///
/// It holds two references and no state of its own; copies are cheap and it is
/// safe to use from any thread as long as both clocks are.
class Timeline {
 public:
  /// @brief Pairs two clocks.
  ///
  /// @param clock The monotonic clock; must outlive this object.
  /// @param wall The wall clock, counting from the Unix epoch; must outlive
  ///     this object.
  Timeline(const platform::ClockProvider& clock,
           const platform::ClockProvider& wall) noexcept
      : clock_(&clock), wall_(&wall) {}

  /// @brief Reads the monotonic clock.
  ///
  /// @return Monotonic time now, in microseconds from an arbitrary origin.
  auto now_us() const noexcept -> std::uint64_t {
    return clock_->now_microseconds();
  }

  /// @brief Reads the wall clock.
  ///
  /// @return Wall-clock time now, in milliseconds since the Unix epoch.
  auto wall_ms() const noexcept -> std::int64_t {
    return static_cast<std::int64_t>(wall_->now_microseconds() /
                                     microseconds_per_millisecond);
  }

  /// @brief Converts a monotonic instant to wall-clock time, as if the wall
  /// clock had kept pace with the monotonic one since then.
  ///
  /// @param monotonic_us A monotonic instant, in microseconds, as now_us()
  ///     returned it; it may be in the past or the future.
  /// @return The matching wall-clock time, in milliseconds since the Unix
  ///     epoch.
  auto unix_ms(std::uint64_t monotonic_us) const noexcept -> std::int64_t {
    const auto now = static_cast<std::int64_t>(clock_->now_microseconds());
    const auto wall = static_cast<std::int64_t>(wall_->now_microseconds());
    return (wall - (now - static_cast<std::int64_t>(monotonic_us))) /
           static_cast<std::int64_t>(microseconds_per_millisecond);
  }

 private:
  /// @brief The monotonic clock; never null.
  const platform::ClockProvider* clock_;
  /// @brief The wall clock; never null.
  const platform::ClockProvider* wall_;
};

}  // namespace libchiaking::psn::auth
