#include "libchiaking/platform/runtime/utc_offset.hpp"

#include <switch.h>

namespace libchiaking::platform {

auto SystemUtcOffset::utc_offset() const noexcept
    -> std::optional<std::chrono::minutes> {
  constexpr s32 seconds_per_minute = 60;
  if (R_FAILED(::timeInitialize())) return std::nullopt;
  u64 now = 0;
  TimeCalendarTime calendar{};
  TimeCalendarAdditionalInfo info{};
  const bool known =
      R_SUCCEEDED(::timeGetCurrentTime(TimeType_Default, &now)) &&
      R_SUCCEEDED(::timeToCalendarTimeWithMyRule(now, &calendar, &info));
  ::timeExit();
  if (!known) return std::nullopt;
  return std::chrono::minutes{info.offset / seconds_per_minute};
}

}  // namespace libchiaking::platform
