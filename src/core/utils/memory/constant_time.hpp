/// @file
/// @brief Declares constant_time_equal, the comparison for secrets and
/// authentication tags whose timing must not reveal where they differ.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace libchiaking::memory {

/// @brief Compares two byte sequences in time that depends only on their
/// length, not on their contents.
///
/// Every byte is visited even after a difference is found, so the time
/// taken leaks nothing about which byte differs. The lengths are compared
/// first and are not secret.
///
/// @param left One sequence.
/// @param right The other sequence.
/// @return True when both have the same length and the same bytes; false
///     otherwise, including at once when the lengths differ.
constexpr auto constant_time_equal(std::span<const std::byte> left,
                                   std::span<const std::byte> right) noexcept
    -> bool {
  if (left.size() != right.size()) return false;
  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index)
    difference = static_cast<std::uint8_t>(
        difference | (std::to_integer<std::uint8_t>(left[index]) ^
                      std::to_integer<std::uint8_t>(right[index])));
  return difference == 0;
}

}  // namespace libchiaking::memory
