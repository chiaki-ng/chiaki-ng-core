#include "libchiaking/platform/runtime/random_provider.hpp"

#include <algorithm>
#include <limits>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <bcrypt.h>
#include <windows.h>

namespace libchiaking::platform {
auto SystemSecureRandom::fill_bytes(std::span<std::byte> output)
    -> Expected<void, PlatformError> {
  constexpr std::size_t max_chunk = std::numeric_limits<ULONG>::max();
  while (!output.empty()) {
    const auto chunk = output.first(std::min(output.size(), max_chunk));
    const auto status = ::BCryptGenRandom(
        nullptr, reinterpret_cast<PUCHAR>(chunk.data()),
        static_cast<ULONG>(chunk.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) return unexpected(PlatformError::io_failure);
    output = output.subspan(chunk.size());
  }
  return Expected<void, PlatformError>{};
}

}  // namespace libchiaking::platform
