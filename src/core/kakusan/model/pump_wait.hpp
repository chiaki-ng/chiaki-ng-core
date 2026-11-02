/// @file
/// @brief Declares PumpWait, how long a Kakusan session's loop may sleep
/// before its next pump turn.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace libchiaking::kakusan {

/// @brief The sleep rule of a shared session's or guest visit's loop: no
/// sleep while work waits, never past the machine's next timer, never longer
/// than the caller allows.
///
/// Pure arithmetic on monotonic times the caller reads.
class PumpWait {
 public:
  /// @brief Works out how long the session's loop may sleep before the next
  /// turn.
  ///
  /// @param due True when work is already waiting (queued steps or
  ///     forwarded datagrams), which allows no sleep at all.
  /// @param deadline_us The machine's next timer, in monotonic microseconds,
  ///     or nothing when no timer is set.
  /// @param now_us The current monotonic time, in microseconds.
  /// @param limit The longest sleep the caller allows.
  /// @return Zero when work is due, `limit` is zero or negative, or the
  ///     deadline has passed; otherwise the time to the deadline rounded up
  ///     to whole milliseconds, capped at `limit`; `limit` when there is no
  ///     deadline.
  static constexpr auto remaining_wait(bool due,
                                       std::optional<std::uint64_t> deadline_us,
                                       std::uint64_t now_us,
                                       std::chrono::milliseconds limit) noexcept
      -> std::chrono::milliseconds {
    if (due || limit.count() <= 0) return std::chrono::milliseconds{0};
    if (!deadline_us) return limit;
    if (*deadline_us <= now_us) return std::chrono::milliseconds{0};
    return std::min(ceil_milliseconds(*deadline_us - now_us), limit);
  }

  /// @brief Converts a span of microseconds to milliseconds, rounding up so
  /// a sleep never wakes before its deadline.
  ///
  /// @param microseconds The span, in microseconds.
  /// @return The span in whole milliseconds, rounded up and capped at the
  ///     largest std::chrono::milliseconds value.
  static constexpr auto ceil_milliseconds(std::uint64_t microseconds) noexcept
      -> std::chrono::milliseconds {
    const auto rounded =
        std::chrono::ceil<std::chrono::duration<std::uint64_t, std::milli>>(
            std::chrono::duration<std::uint64_t, std::micro>{microseconds});
    const auto most =
        static_cast<std::uint64_t>(std::chrono::milliseconds::max().count());
    return std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(
            std::min(rounded.count(), most))};
  }
};

}  // namespace libchiaking::kakusan
