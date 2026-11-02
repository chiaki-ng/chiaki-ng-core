/// @file
/// @brief Declares the CRC-32 checksum that STUN's FINGERPRINT attribute and
/// the peer link's short blocks carry, computable at compile time.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace libchiaking::bytes {

/// @brief The reflected IEEE 802.3 CRC-32 polynomial, as zlib and Ethernet
/// use it.
inline constexpr std::uint32_t crc32_polynomial = 0xedb88320;
/// @brief The register's starting value, also XORed into the final result.
inline constexpr std::uint32_t crc32_initial = 0xffffffff;

/// @brief The 256-entry lookup table for crc32_polynomial, built at compile
/// time; entry i is the CRC register after shifting byte i through.
inline constexpr auto crc32_table = [] {
  std::array<std::uint32_t, 256> entries{};
  for (std::uint32_t index = 0; index < entries.size(); ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit)
      value =
          (value & 1U) != 0 ? crc32_polynomial ^ (value >> 1U) : value >> 1U;
    entries[index] = value;
  }
  return entries;
}();

/// @brief Computes the standard CRC-32 of a byte sequence, the same value as
/// zlib's `crc32()`.
///
/// @param bytes The bytes to check; may be empty.
/// @return The checksum; zero for no bytes.
constexpr auto crc32(std::span<const std::byte> bytes) noexcept
    -> std::uint32_t {
  std::uint32_t crc = crc32_initial;
  for (const auto byte : bytes)
    crc = crc32_table[(crc ^ std::to_integer<std::uint32_t>(byte)) & 0xffU] ^
          (crc >> 8U);
  return crc ^ crc32_initial;
}

}  // namespace libchiaking::bytes
