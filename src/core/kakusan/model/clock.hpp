/// @file
/// @brief Declares Clock, the one reading of time every Kakusan step is
/// given.
///
/// The Kakusan machines never read a clock themselves. Each command and
/// each turn of the pump is handed one Clock, so a step sees a single,
/// consistent instant and tests can drive time by hand.
#pragma once

#include <cstdint>

namespace libchiaking::kakusan {

/// @brief One instant, read from the monotonic and the wall clock together.
///
/// Plain data, made by Surroundings::now() from the platform's monotonic and
/// wall clocks, or from an injected source in tests, and passed by value.
struct Clock {
  /// @brief Monotonic time in microseconds from the platform's
  /// ClockProvider; measures timeouts, deadlines and ages. Its origin is
  /// arbitrary, so only differences between readings mean anything.
  std::uint64_t now_us{};
  /// @brief Wall-clock time in whole seconds since the Unix epoch, never
  /// below zero; compared with invitation expiry times and stamped on
  /// remembered devices.
  std::uint64_t wall_seconds{};
};

}  // namespace libchiaking::kakusan
