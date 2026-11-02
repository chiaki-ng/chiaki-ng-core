#include "src/core/net/kakusan/verification_code.hpp"

#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace libchiaking::net::kakusan {

auto Code::parse(std::string_view text) -> Result<Code> {
  if (text.size() != code_size) return fail(Error::pairing_failed);
  if (!std::ranges::all_of(text, text::ascii_digit))
    return fail(Error::pairing_failed);
  SecretArray<code_size, char> storage;
  std::ranges::copy(text, storage.begin());
  return Code{std::move(storage)};
}

auto Code::from_value(std::uint16_t value) -> Result<Code> {
  if (value > code_count - 1) return fail(Error::internal);
  SecretArray<code_size, char> storage;
  std::format_to_n(storage.begin(), code_size, "{:04}", value);
  return Code{std::move(storage)};
}

auto Code::draw(platform::SecureRandomProvider& random) -> std::optional<Code> {
  constexpr std::uint32_t limit = 65536U - 65536U % code_count;
  for (std::size_t attempt = 0; attempt < draw_attempts; ++attempt) {
    SecretArray<2> bytes;
    if (!random.fill_bytes(bytes)) return std::nullopt;
    const auto value =
        bytes::load_be<std::uint16_t>({bytes.data(), bytes.size()});
    if (value >= limit) continue;
    auto code = from_value(static_cast<std::uint16_t>(value % code_count));
    if (!code) return std::nullopt;
    return std::move(code).value();
  }
  return std::nullopt;
}

}  // namespace libchiaking::net::kakusan
