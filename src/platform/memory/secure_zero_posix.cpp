#define _DEFAULT_SOURCE

#include <libchiaking/detail/secure_zero.hpp>

#include <string.h>

namespace libchiaking::memory {

void secure_zero(std::span<std::byte> bytes) noexcept {
  if (bytes.empty()) return;
#if defined(LIBCHIAKING_HAVE_EXPLICIT_BZERO)
  explicit_bzero(bytes.data(), bytes.size());
#else
  volatile std::byte* cursor = bytes.data();
  for (std::size_t index = 0; index < bytes.size(); ++index)
    cursor[index] = std::byte{};
#endif
}

}  // namespace libchiaking::memory
