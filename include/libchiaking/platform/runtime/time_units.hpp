/// @file
/// @brief Conversion factors between the time units used in clock readings.
#pragma once

#include <cstdint>

namespace libchiaking {

/// @brief Number of microseconds in one millisecond.
inline constexpr std::uint64_t microseconds_per_millisecond = 1'000;
/// @brief Number of microseconds in one second.
inline constexpr std::uint64_t microseconds_per_second = 1'000'000;
/// @brief Number of milliseconds in one second.
inline constexpr std::uint64_t milliseconds_per_second = 1'000;

}  // namespace libchiaking
