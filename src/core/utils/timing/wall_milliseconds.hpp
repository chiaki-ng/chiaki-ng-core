/// @file
/// @brief Declares the one stored form of a wall-clock time: whole
/// milliseconds since the Unix epoch, as caches write them to disk.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace libchiaking::timing {

/// @brief Wall-clock time in milliseconds since the Unix epoch.
using WallMilliseconds = std::chrono::sys_time<std::chrono::milliseconds>;

/// @brief The stored form of a time.
///
/// @param time A wall-clock time.
/// @return Milliseconds since the Unix epoch; negative before 1970.
constexpr auto milliseconds_of(WallMilliseconds time) noexcept -> std::int64_t {
  return time.time_since_epoch().count();
}

/// @brief The stored form of an optional time.
///
/// @param time A wall-clock time, or std::nullopt.
/// @return Milliseconds since the Unix epoch, or std::nullopt when there was
///     no time.
constexpr auto milliseconds_of(std::optional<WallMilliseconds> time) noexcept
    -> std::optional<std::int64_t> {
  if (!time) return std::nullopt;
  return milliseconds_of(*time);
}

/// @brief A stored time read back.
///
/// @param milliseconds Milliseconds since the Unix epoch.
/// @return The wall-clock time; every value is accepted, 0 included.
constexpr auto time_of(std::int64_t milliseconds) noexcept -> WallMilliseconds {
  return WallMilliseconds{std::chrono::milliseconds{milliseconds}};
}

/// @brief An optional stored time read back.
///
/// @param milliseconds Milliseconds since the Unix epoch, or std::nullopt.
/// @return The wall-clock time, or std::nullopt when none was stored.
constexpr auto time_of(std::optional<std::int64_t> milliseconds) noexcept
    -> std::optional<WallMilliseconds> {
  if (!milliseconds) return std::nullopt;
  return time_of(*milliseconds);
}

}  // namespace libchiaking::timing
