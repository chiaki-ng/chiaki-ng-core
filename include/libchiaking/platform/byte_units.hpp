/// @file
/// @brief Byte-size multipliers shared by buffer and capture limits.
#pragma once

#include <cstddef>

namespace libchiaking {

/// @brief Number of bytes in one kibibyte.
inline constexpr std::size_t bytes_per_kib = 1024;
/// @brief Number of bytes in one mebibyte.
inline constexpr std::size_t bytes_per_mib = bytes_per_kib * bytes_per_kib;

}  // namespace libchiaking
