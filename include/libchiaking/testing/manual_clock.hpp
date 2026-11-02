/// @file
/// @brief Declares ManualClock, a clock that moves only when a test moves it.
#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"

#include <chrono>
#include <cstdint>

namespace libchiaking::testing {

/// @brief Stands in for platform::ClockProvider with time a test controls.
///
/// The clock starts at five seconds and never advances by itself, so timeouts
/// and deadlines in the code under test fire exactly when the test calls
/// advance(). Hand it to whatever takes a platform::ClockProvider, run the
/// code, advance, and run it again.
///
/// @note The clock does no locking and never blocks. Advance it from the
///     thread that drives the code under test, or while that code is idle.
class ManualClock final : public platform::ClockProvider {
 public:
  /// @brief Reports the scripted time.
  ///
  /// @return The current value of now_us, in microseconds. Two calls return
  ///     the same value unless the test moved the clock in between.
  std::uint64_t now_microseconds() const noexcept override { return now_us; }

  /// @brief Moves the clock forward.
  ///
  /// @param duration How far to move, in whole milliseconds.
  /// @pre `duration` is not negative. A negative value wraps round to a very
  ///     large jump forward.
  void advance(std::chrono::milliseconds duration) noexcept {
    now_us += static_cast<std::uint64_t>(duration.count()) *
              microseconds_per_millisecond;
  }

  /// @brief The scripted time in microseconds, five seconds at construction.
  ///
  /// A test may assign it directly, for example to start from zero or to set
  /// the clock back, which a real monotonic clock never does.
  std::uint64_t now_us{5'000'000};
};

}  // namespace libchiaking::testing
