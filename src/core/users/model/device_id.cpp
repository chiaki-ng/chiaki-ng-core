#include "libchiaking/detail/users/device_id.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"

#include "libchiaking/detail/secret_array.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/hex.hpp"

#include <cstddef>
#include <span>
#include <utility>

namespace libchiaking::users {

auto DeviceId::generate(platform::SecureRandomProvider& random)
    -> std::optional<DeviceId> {
  SecretArray<16> bytes;
  if (!random.fill_bytes(std::span<std::byte>{bytes.data(), bytes.size()}))
    return std::nullopt;
  SecretString text;
  text.append(prefix);
  text::append_hex(text,
                   std::span<const std::byte>{bytes.data(), bytes.size()});
  return DeviceId{std::move(text)};
}

auto DeviceId::parse(std::string_view text) -> std::optional<DeviceId> {
  if (text.size() != length) return std::nullopt;
  SecretString normalized;
  for (const char character : text) {
    if (!text::hex_digit(character)) return std::nullopt;
    normalized.push_back(text::ascii_lower(character));
  }
  if (normalized.view().substr(0, prefix.size()) != prefix) return std::nullopt;
  return DeviceId{std::move(normalized)};
}

}  // namespace libchiaking::users
