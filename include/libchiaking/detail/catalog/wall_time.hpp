/// @file
/// @brief Declares the wall-clock time every catalog answer is stamped with,
/// and the one rule for when a stamped answer is still fresh.
#pragma once

#include <chrono>

namespace libchiaking::catalog {

/// @brief Wall-clock time in milliseconds since the Unix epoch.
using Time = std::chrono::sys_time<std::chrono::milliseconds>;

/// @brief Whether an answer stamped at one time may still be reused.
///
/// Pure; safe from any thread.
///
/// @param since When the answer was fetched or checked.
/// @param lifetime How long the answer may be reused.
/// @param now The current wall-clock time.
/// @return True from since until lifetime has passed; false for a time before
///     since, so a clock that stepped back never revives an answer.
constexpr auto is_fresh(Time since, std::chrono::milliseconds lifetime,
                        Time now) noexcept -> bool {
  return now >= since && now - since < lifetime;
}

}  // namespace libchiaking::catalog
