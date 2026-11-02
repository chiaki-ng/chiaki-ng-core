#define __STDC_WANT_LIB_EXT1__ 1

#include <libchiaking/detail/secure_zero.hpp>

#include <string.h>

namespace libchiaking::memory {

void secure_zero(std::span<std::byte> bytes) noexcept {
  if (bytes.empty()) return;
  memset_s(bytes.data(), bytes.size(), 0, bytes.size());
}

}  // namespace libchiaking::memory
