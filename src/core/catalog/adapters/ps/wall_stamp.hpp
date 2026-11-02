/// @file
/// @brief Declares ps::WallStamp, which stamps adapter answers with the
/// device's wall-clock time.
#pragma once

#include "libchiaking/detail/catalog/wall_time.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <chrono>

namespace libchiaking::catalog::ps {

/// @brief Reads the wall clock as catalog time.
///
/// Holds a pointer to the clock, which must outlive the stamp. Safe to use
/// from any thread when the clock is.
class WallStamp {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = catalog::Time;

  /// @brief Uses a clock.
  ///
  /// @param wall The wall clock. Must outlive the stamp.
  explicit WallStamp(const platform::ClockProvider& wall) noexcept
      : wall_(&wall) {}

  /// @brief The wall clock now.
  ///
  /// @return The clock's microseconds truncated to milliseconds.
  auto now() const noexcept -> Time {
    return Time{std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::microseconds{wall_->now_microseconds()})};
  }

 private:
  /// @brief The wall clock; not owned, never null.
  const platform::ClockProvider* wall_;
};

}  // namespace libchiaking::catalog::ps
