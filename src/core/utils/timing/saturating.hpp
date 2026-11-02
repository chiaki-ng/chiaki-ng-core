/// @file
/// @brief Declares saturating time arithmetic: sums that stop at the
/// representable limit instead of overflowing, and elapsed times that never
/// go negative.
#pragma once

#include <chrono>
#include <type_traits>

namespace libchiaking::timing {

/// @brief Adds two durations, clamping to the representable range instead of
/// overflowing.
///
/// @tparam Rep The tick count type, signed or unsigned.
/// @tparam Period The tick length.
/// @param left One duration.
/// @param right The other duration.
/// @return The sum; `Duration::max()` when it would exceed the maximum and,
///     for signed ticks, `Duration::min()` when it would fall below the
///     minimum.
template <class Rep, class Period>
constexpr auto saturating_add(std::chrono::duration<Rep, Period> left,
                              std::chrono::duration<Rep, Period> right) noexcept
    -> std::chrono::duration<Rep, Period> {
  using Duration = std::chrono::duration<Rep, Period>;
  if (right > Duration::zero() && left > Duration::max() - right)
    return Duration::max();
  if constexpr (std::is_signed_v<Rep>)
    if (right < Duration::zero() && left < Duration::min() - right)
      return Duration::min();
  return left + right;
}

/// @brief Moves a time point by an interval, clamping instead of
/// overflowing.
///
/// @tparam Clock The time point's clock.
/// @tparam Duration The time point's duration type.
/// @param at The time point.
/// @param interval How far to move it.
/// @return The moved time point, clamped as saturating_add() of durations
///     clamps.
template <class Clock, class Duration>
constexpr auto saturating_add(std::chrono::time_point<Clock, Duration> at,
                              Duration interval) noexcept
    -> std::chrono::time_point<Clock, Duration> {
  return std::chrono::time_point<Clock, Duration>{
      saturating_add(at.time_since_epoch(), interval)};
}

/// @brief Measures the time from one reading to a later one, never negative.
///
/// @tparam Rep The tick count type.
/// @tparam Period The tick length.
/// @param since The earlier reading, as time since the clock's origin.
/// @param now The later reading, as time since the clock's origin.
/// @return `now - since`, or zero when `now` is not after `since`.
template <class Rep, class Period>
constexpr auto elapsed(std::chrono::duration<Rep, Period> since,
                       std::chrono::duration<Rep, Period> now) noexcept
    -> std::chrono::duration<Rep, Period> {
  return now > since ? now - since : std::chrono::duration<Rep, Period>::zero();
}

/// @brief Measures the time from one time point to a later one, never
/// negative.
///
/// @tparam Clock The time points' clock.
/// @tparam Duration The time points' duration type.
/// @param since The earlier time point.
/// @param now The later time point.
/// @return `now - since`, or zero when `now` is not after `since`.
template <class Clock, class Duration>
constexpr auto elapsed(std::chrono::time_point<Clock, Duration> since,
                       std::chrono::time_point<Clock, Duration> now) noexcept
    -> Duration {
  return elapsed(since.time_since_epoch(), now.time_since_epoch());
}

}  // namespace libchiaking::timing
