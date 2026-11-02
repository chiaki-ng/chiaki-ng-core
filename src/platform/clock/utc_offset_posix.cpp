#define _DEFAULT_SOURCE

#include "libchiaking/platform/runtime/utc_offset.hpp"

#include <ctime>

namespace libchiaking::platform {
auto SystemUtcOffset::utc_offset() const noexcept
    -> std::optional<std::chrono::minutes> {
  constexpr long seconds_per_minute = 60;
  const auto now = std::time(nullptr);
  std::tm local{};
  if (now == static_cast<std::time_t>(-1) ||
      localtime_r(&now, &local) == nullptr)
    return std::nullopt;
  return std::chrono::minutes{local.tm_gmtoff / seconds_per_minute};
}

}  // namespace libchiaking::platform
