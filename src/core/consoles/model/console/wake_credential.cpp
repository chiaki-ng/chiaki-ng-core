#include "src/core/consoles/model/console/wake_credential.hpp"

#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/text/hex.hpp"

#include <algorithm>

namespace libchiaking::consoles {

auto WakeCredential::from_registration_key(
    std::span<const std::byte> key) noexcept -> std::optional<WakeCredential> {
  const auto digits = key.first(static_cast<std::size_t>(
      std::ranges::find(key, std::byte{0}) - key.begin()));
  if (digits.empty() || digits.size() > maximum_digits) return std::nullopt;
  std::uint64_t value = 0;
  for (const auto digit : digits) {
    const auto nibble = text::hex_digit(static_cast<char>(digit));
    if (!nibble) return std::nullopt;
    value = (value << 4U) | *nibble;
  }
  return WakeCredential{value};
}

WakeCredential::WakeCredential(std::uint64_t value) noexcept {
  bytes::store_be<std::uint64_t>({value_.data(), value_.size()}, value);
}

auto WakeCredential::value() const noexcept -> std::uint64_t {
  return bytes::load_be<std::uint64_t>({value_.data(), value_.size()});
}

}  // namespace libchiaking::consoles
