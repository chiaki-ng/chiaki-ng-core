#include "libchiaking/platform/runtime/random_provider.hpp"

#include <switch.h>

namespace libchiaking::platform {

auto SystemSecureRandom::fill_bytes(std::span<std::byte> output)
    -> Expected<void, PlatformError> {
  if (output.empty()) return Expected<void, PlatformError>{};
  if (R_FAILED(::csrngInitialize()))
    return unexpected(PlatformError::io_failure);
  const Result filled = ::csrngGetRandomBytes(output.data(), output.size());
  ::csrngExit();
  if (R_FAILED(filled)) return unexpected(PlatformError::io_failure);
  return Expected<void, PlatformError>{};
}

}  // namespace libchiaking::platform
