#include "libchiaking/detail/consoles/device_uid.hpp"

#include "src/core/utils/text/hex.hpp"

#include <algorithm>

namespace libchiaking::consoles {

auto DeviceUid::parse(std::string_view hex) noexcept
    -> std::optional<DeviceUid> {
  std::array<std::byte, size> bytes{};
  if (!text::decode_hex(hex, bytes)) return std::nullopt;
  return from_bytes(bytes);
}

auto DeviceUid::from_bytes(std::span<const std::byte, size> bytes) noexcept
    -> std::optional<DeviceUid> {
  if (std::ranges::all_of(
          bytes, [](std::byte value) { return value == std::byte{0}; }))
    return std::nullopt;
  std::array<std::byte, size> held{};
  std::ranges::copy(bytes, held.begin());
  return DeviceUid{held};
}

auto DeviceUid::main_ps4() noexcept -> DeviceUid {
  std::array<std::byte, size> bytes{};
  bytes.fill(main_ps4_byte);
  return DeviceUid{bytes};
}

auto DeviceUid::text() const -> std::string {
  std::string text;
  text.reserve(text_length);
  text::append_hex(text, bytes_);
  return text;
}

auto DeviceUid::is_main_ps4() const noexcept -> bool {
  return std::ranges::all_of(
      bytes_, [](std::byte value) { return value == main_ps4_byte; });
}

}  // namespace libchiaking::consoles
