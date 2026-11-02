#define _DEFAULT_SOURCE

#include "libchiaking/platform/runtime/random_provider.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>

#include <sys/random.h>

#include <unistd.h>

namespace libchiaking::platform {
auto SystemSecureRandom::fill_bytes(std::span<std::byte> output)
    -> Expected<void, PlatformError> {
  constexpr std::size_t max_chunk = 256;
  while (!output.empty()) {
    const auto chunk = output.first(std::min(output.size(), max_chunk));
    if (::getentropy(chunk.data(), chunk.size()) != 0)
      return unexpected(errno == ENOMEM ? PlatformError::out_of_memory
                                        : PlatformError::io_failure);
    output = output.subspan(chunk.size());
  }
  return Expected<void, PlatformError>{};
}

}  // namespace libchiaking::platform
