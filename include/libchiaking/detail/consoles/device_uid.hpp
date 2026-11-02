/// @file
/// @brief Declares DeviceUid, PSN's identifier for a console on an account.
#pragma once

#include "libchiaking/detail/wire_sizes.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace libchiaking::consoles {

/// @brief PSN's identifier for a console registered to an account.
///
/// It is what addresses a console over the internet. It is unrelated to the
/// HardwareId, and a console only gains one here once PSN's list of the
/// account's consoles has been matched to it or it has been paired over the
/// internet.
///
/// @see AccountConsole
class DeviceUid {
 public:
  /// @brief Size of the identifier in bytes.
  static constexpr std::size_t size = psn_device_uid_size;
  /// @brief Length of the identifier written as hexadecimal digits.
  static constexpr std::size_t text_length = size * 2;
  /// @brief The byte the main PS4 placeholder is filled with.
  static constexpr std::byte main_ps4_byte{0x41};

  /// @brief Reads an identifier from hexadecimal text.
  ///
  /// @param hex Exactly text_length hexadecimal digits.
  /// @return The identifier, or nothing when the text is not valid
  ///     hexadecimal of that length or decodes to all zero bytes.
  [[nodiscard]] static auto parse(std::string_view hex) noexcept
      -> std::optional<DeviceUid>;
  /// @brief Adopts an identifier from raw bytes.
  ///
  /// @param bytes The identifier's bytes.
  /// @return The identifier, or nothing when every byte is zero.
  [[nodiscard]] static auto from_bytes(
      std::span<const std::byte, size> bytes) noexcept
      -> std::optional<DeviceUid>;
  /// @brief Returns the placeholder that stands for the account's main PS4.
  ///
  /// PSN does not list PS4 consoles, so a PS4 is addressed over the internet
  /// with this fixed value, every byte of which is main_ps4_byte.
  ///
  /// @return The placeholder identifier; never all zero.
  static auto main_ps4() noexcept -> DeviceUid;

  /// @brief The identifier's raw bytes.
  ///
  /// @return The raw bytes, which live as long as this object.
  auto bytes() const noexcept -> const std::array<std::byte, size>& {
    return bytes_;
  }
  /// @brief The identifier as text_length hexadecimal digits.
  ///
  /// @return The hexadecimal digits, never empty.
  auto text() const -> std::string;
  /// @brief Whether this is the main PS4 placeholder.
  ///
  /// @return True when every byte equals main_ps4_byte, false otherwise.
  auto is_main_ps4() const noexcept -> bool;

  /// @brief Compares the bytes.
  friend auto operator==(const DeviceUid&, const DeviceUid&) noexcept
      -> bool = default;

 private:
  explicit DeviceUid(const std::array<std::byte, size>& bytes) noexcept
      : bytes_(bytes) {}

  std::array<std::byte, size> bytes_;
};

}  // namespace libchiaking::consoles
