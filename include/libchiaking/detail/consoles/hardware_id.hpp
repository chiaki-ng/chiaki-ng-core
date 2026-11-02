/// @file
/// @brief Declares HardwareId, a console's MAC address used as its identity.
#pragma once

#include "libchiaking/detail/wire_sizes.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace libchiaking::consoles {

/// @brief A console's MAC address, the identity the library keys consoles by.
///
/// The console states it when it answers a network search and when it is
/// paired. It is ordered so it can key sorted containers.
class HardwareId {
 public:
  /// @brief Size of the identifier in bytes.
  static constexpr std::size_t size = mac_address_size;
  /// @brief Number of hexadecimal digits in the identifier.
  static constexpr std::size_t digit_count = size * 2;
  /// @brief Longest text parse() accepts: the digits with a separator between
  /// each pair.
  static constexpr std::size_t text_limit = digit_count + size - 1;
  /// @brief Characters parse() skips between digits.
  static constexpr std::string_view separators = ":-.";

  /// @brief Reads an identifier from text such as "aa:bb:cc:dd:ee:ff" or
  /// "aabbccddeeff".
  ///
  /// @param text digit_count hexadecimal digits in either case. Any character
  ///     from separators is skipped wherever it appears.
  /// @return The identifier, or nothing when the text is longer than
  ///     text_limit, does not hold exactly digit_count hexadecimal digits, or
  ///     decodes to all zero bytes.
  [[nodiscard]] static auto parse(std::string_view text) noexcept
      -> std::optional<HardwareId>;
  /// @brief Adopts an identifier from raw bytes.
  ///
  /// @param bytes The MAC address bytes in transmission order.
  /// @return The identifier, or nothing when every byte is zero.
  [[nodiscard]] static auto from_bytes(
      std::span<const std::byte, size> bytes) noexcept
      -> std::optional<HardwareId>;

  /// @brief The MAC address bytes.
  ///
  /// @return The six MAC bytes, which live as long as this object.
  auto bytes() const noexcept -> const std::array<std::byte, size>& {
    return bytes_;
  }
  /// @brief The identifier as digit_count hexadecimal digits with no
  /// separators.
  ///
  /// @return The digits, never empty.
  auto text() const -> std::string;

  /// @brief Compares the bytes.
  friend auto operator==(const HardwareId&, const HardwareId&) noexcept
      -> bool = default;
  /// @brief Orders identifiers byte by byte.
  friend auto operator<=>(const HardwareId&, const HardwareId&) noexcept
      -> std::strong_ordering = default;

 private:
  explicit HardwareId(const std::array<std::byte, size>& bytes) noexcept
      : bytes_(bytes) {}

  std::array<std::byte, size> bytes_;
};

}  // namespace libchiaking::consoles
