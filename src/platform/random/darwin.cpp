#include "libchiaking/platform/runtime/random_provider.hpp"

#include <cstddef>

#include <CommonCrypto/CommonCryptoError.h>
#include <CommonCrypto/CommonRandom.h>

namespace libchiaking::platform {
auto SystemSecureRandom::fill_bytes(std::span<std::byte> output)
    -> Expected<void, PlatformError> {
  switch (::CCRandomGenerateBytes(output.data(), output.size())) {
    case kCCSuccess:
      return Expected<void, PlatformError>{};
    case kCCMemoryFailure:
      return unexpected(PlatformError::out_of_memory);
    default:
      return unexpected(PlatformError::io_failure);
  }
}

}  // namespace libchiaking::platform
