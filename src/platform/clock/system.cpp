#include "libchiaking/platform/runtime/clock_provider.hpp"

#include <chrono>

namespace libchiaking::platform {

std::uint64_t SystemClock::now_microseconds() const noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace libchiaking::platform
