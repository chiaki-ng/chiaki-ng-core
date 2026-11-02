/// @file
/// @brief Declares Deadline, a point on the platform's monotonic clock by
/// which something must happen, or no deadline at all.
///
/// Timers in the protocol and play code are kept as deadlines in
/// microseconds of ClockProvider::now_microseconds, and combined by taking
/// the earliest. All arithmetic saturates rather than wrapping, so an
/// interval of "forever" stays forever.
#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/utils/timing/saturating.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ratio>

namespace libchiaking::timing {

/// @brief An optional instant on the monotonic clock, expressed in
/// microseconds since the clock's own origin.
///
/// An empty deadline never expires. Instants are only meaningful against
/// readings of the same ClockProvider; they are not wall-clock times. A
/// plain value; copy freely, but share between threads only with external
/// synchronisation.
class Deadline {
 public:
  /// @brief A `std::chrono` clock type naming the platform's monotonic
  /// microsecond clock, so that instants and durations are typed.
  ///
  /// It has no `now()`; read the time through a ClockProvider with
  /// Deadline::now().
  struct Clock {
    /// @brief Ticks are unsigned 64-bit counts.
    using rep = std::uint64_t;
    /// @brief One tick is a microsecond.
    using period = std::micro;
    /// @brief A span of microseconds.
    using duration = std::chrono::duration<rep, period>;
    /// @brief A reading of the clock.
    using time_point = std::chrono::time_point<Clock>;
    /// @brief The clock never goes back, as ClockProvider promises.
    static constexpr bool is_steady = true;
  };
  /// @brief A span of time in microseconds.
  using Duration = Clock::duration;
  /// @brief A reading of the monotonic clock.
  using Instant = Clock::time_point;

  /// @brief Makes an empty deadline, which never expires.
  constexpr Deadline() noexcept = default;

  /// @brief Makes a deadline at an instant.
  ///
  /// @param at The instant at which it expires.
  constexpr explicit Deadline(Instant at) noexcept : at_(at) {}

  /// @brief Reads the clock as an Instant.
  ///
  /// @param clock The platform clock.
  /// @return The current instant.
  static auto now(const platform::ClockProvider& clock) noexcept -> Instant {
    return instant_of(clock.now_microseconds());
  }

  /// @brief Types a raw microsecond reading as an Instant.
  ///
  /// @param microseconds A reading of ClockProvider::now_microseconds.
  /// @return The instant.
  static constexpr auto instant_of(std::uint64_t microseconds) noexcept
      -> Instant {
    return Instant{Duration{microseconds}};
  }

  /// @brief Makes the deadline an interval after an instant.
  ///
  /// @param now The instant to count from.
  /// @param interval How long after it.
  /// @return The deadline, at the clock's maximum when the sum would
  ///     overflow.
  static constexpr auto after(Instant now, Duration interval) noexcept
      -> Deadline {
    return Deadline{saturating_add(now, interval)};
  }

  /// @brief Makes the deadline an interval from now.
  ///
  /// @param clock The platform clock, read once.
  /// @param interval How long from now.
  /// @return The deadline, at the clock's maximum when the sum would
  ///     overflow.
  static auto after(const platform::ClockProvider& clock,
                    Duration interval) noexcept -> Deadline {
    return after(now(clock), interval);
  }

  /// @brief Adds an interval to a raw microsecond reading, saturating.
  ///
  /// @param now_us The reading, in microseconds.
  /// @param interval_us The interval, in microseconds.
  /// @return The sum in microseconds, or the largest `std::uint64_t` when it
  ///     would overflow.
  static constexpr auto after_us(std::uint64_t now_us,
                                 std::uint64_t interval_us) noexcept
      -> std::uint64_t {
    return saturating_add(Duration{now_us}, Duration{interval_us}).count();
  }

  /// @brief Measures the time between two raw readings, never negative.
  ///
  /// @param since_us The earlier reading, in microseconds.
  /// @param now_us The later reading, in microseconds.
  /// @return The microseconds from `since_us` to `now_us`, or zero when
  ///     `now_us` is not after it.
  static constexpr auto elapsed_us(std::uint64_t since_us,
                                   std::uint64_t now_us) noexcept
      -> std::uint64_t {
    return elapsed(Duration{since_us}, Duration{now_us}).count();
  }

  /// @brief Picks the sooner of two optional raw deadlines.
  ///
  /// @param left One deadline in microseconds, or none.
  /// @param right The other deadline in microseconds, or none.
  /// @return The smaller of the two; the one present when only one is;
  ///     nothing when neither is.
  static constexpr auto earliest(std::optional<std::uint64_t> left,
                                 std::optional<std::uint64_t> right) noexcept
      -> std::optional<std::uint64_t> {
    if (!left) return right;
    if (!right) return left;
    return std::min(*left, *right);
  }

  /// @brief Tells whether an optional raw deadline has passed.
  ///
  /// @param at_us The deadline in microseconds, or none.
  /// @param now_us The current reading in microseconds.
  /// @return True when there is a deadline and `now_us` has reached it;
  ///     false for no deadline.
  static constexpr auto expired(std::optional<std::uint64_t> at_us,
                                std::uint64_t now_us) noexcept -> bool {
    return at_us && now_us >= *at_us;
  }

  /// @brief Tells whether this deadline has passed at an instant.
  ///
  /// @param now The instant to check at.
  /// @return True when there is a deadline and `now` has reached it; false
  ///     for an empty deadline.
  constexpr auto expired(Instant now) const noexcept -> bool {
    return at_ && now >= *at_;
  }

  /// @brief Tells whether this deadline has passed now.
  ///
  /// @param clock The platform clock, read once.
  /// @return True when there is a deadline and it has been reached.
  auto expired(const platform::ClockProvider& clock) const noexcept -> bool {
    return expired(now(clock));
  }

  /// @brief Measures how long is left before this deadline.
  ///
  /// @param now The instant to measure from.
  /// @return The time left, zero once expired; nothing for an empty
  ///     deadline.
  constexpr auto remaining(Instant now) const noexcept
      -> std::optional<Duration> {
    if (!at_) return std::nullopt;
    return elapsed(now, *at_);
  }

  /// @brief Moves this deadline earlier to another one, if that is sooner.
  ///
  /// @param other The deadline to consider; an empty one changes nothing.
  constexpr void consider(Deadline other) noexcept {
    if (other.at_) at_ = at_ ? std::min(*at_, *other.at_) : other.at_;
  }

  /// @brief Moves this deadline earlier to a raw one, if that is sooner.
  ///
  /// @param at_us The deadline to consider, in microseconds.
  constexpr void consider(std::uint64_t at_us) noexcept {
    consider(Deadline{instant_of(at_us)});
  }

  /// @brief Moves this deadline earlier to an optional raw one, if present
  /// and sooner.
  ///
  /// @param at_us The deadline to consider in microseconds, or none.
  constexpr void consider(std::optional<std::uint64_t> at_us) noexcept {
    if (at_us) consider(*at_us);
  }

  /// @brief Returns the instant of this deadline.
  ///
  /// @return The instant, or nothing for an empty deadline.
  constexpr auto at() const noexcept -> std::optional<Instant> { return at_; }

  /// @brief Returns this deadline as a raw reading.
  ///
  /// @return The deadline in microseconds of the platform clock, or
  ///     nothing for an empty deadline.
  constexpr auto at_us() const noexcept -> std::optional<std::uint64_t> {
    if (!at_) return std::nullopt;
    return at_->time_since_epoch().count();
  }

  /// @brief Compares two deadlines; two empty ones are equal.
  ///
  /// @return True when both are empty or both fall at the same instant.
  friend constexpr auto operator==(const Deadline&, const Deadline&) noexcept
      -> bool = default;

 private:
  /// @brief When the deadline expires, or nothing for never.
  std::optional<Instant> at_;
};

}  // namespace libchiaking::timing
