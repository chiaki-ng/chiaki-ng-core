#include <libchiaking/detail/secure_zero.hpp>

#include <windows.h>

namespace libchiaking::memory {

void secure_zero(std::span<std::byte> bytes) noexcept {
  if (bytes.empty()) return;
  SecureZeroMemory(bytes.data(), bytes.size());
}

}  // namespace libchiaking::memory
