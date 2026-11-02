#include "libchiaking/platform/runtime/utc_offset.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace libchiaking::platform {

auto SystemUtcOffset::utc_offset() const noexcept
    -> std::optional<std::chrono::minutes> {
  TIME_ZONE_INFORMATION zone{};
  const auto kind = GetTimeZoneInformation(&zone);
  if (kind == TIME_ZONE_ID_INVALID) return std::nullopt;
  const long seasonal = kind == TIME_ZONE_ID_DAYLIGHT   ? zone.DaylightBias
                        : kind == TIME_ZONE_ID_STANDARD ? zone.StandardBias
                                                        : 0;
  return std::chrono::minutes{-(zone.Bias + seasonal)};
}

}  // namespace libchiaking::platform
