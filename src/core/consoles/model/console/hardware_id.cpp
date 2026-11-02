#include "libchiaking/detail/consoles/hardware_id.hpp"

#include "src/core/utils/text/hex.hpp"

#include <algorithm>

namespace libchiaking::consoles {

auto HardwareId::parse(std::string_view text) noexcept
    -> std::optional<HardwareId> {
  if (text.size() < digit_count || text.size() > text_limit)
    return std::nullopt;
  std::array<char, digit_count> digits{};
  std::size_t count = 0;
  for (const char value : text) {
    if (separators.find(value) != std::string_view::npos) continue;
    if (count == digits.size()) return std::nullopt;
    digits[count++] = value;
  }
  std::array<std::byte, size> bytes{};
  if (count != digits.size() ||
      !text::decode_hex(std::string_view{digits.data(), digits.size()}, bytes))
    return std::nullopt;
  return from_bytes(bytes);
}

auto HardwareId::from_bytes(std::span<const std::byte, size> bytes) noexcept
    -> std::optional<HardwareId> {
  if (std::ranges::all_of(
          bytes, [](std::byte value) { return value == std::byte{0}; }))
    return std::nullopt;
  std::array<std::byte, size> held{};
  std::ranges::copy(bytes, held.begin());
  return HardwareId{held};
}

auto HardwareId::text() const -> std::string {
  std::string text;
  text.reserve(digit_count);
  text::append_hex(text, bytes_);
  return text;
}

}  // namespace libchiaking::consoles
