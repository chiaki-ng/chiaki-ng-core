/// @file
/// @brief Declares TrustedTime, a UTC time learned from NTP servers that agree,
/// carried forward on the monotonic clock.
#pragma once

#include "src/core/net/ntp/timestamp.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace libchiaking::net::ntp {

/// @brief The NTP provider whose servers agreed on a time.
enum class TimeSource : std::uint8_t {
  /// @brief Cloudflare's NTP service, `time.cloudflare.com`, tried first.
  cloudflare,
  /// @brief Google's public NTP servers, `time1` to `time4.google.com`,
  /// tried when Cloudflare gives no agreed time.
  google
};

/// @brief UTC as agreed by a majority of one provider's servers, anchored to a
/// monotonic instant so it can be read later without asking again.
///
/// The time read at any later monotonic instant is the agreed time plus the
/// monotonic time elapsed since the anchor, so it does not follow changes to
/// the
/// device's own wall clock. It stays usable for one hour (max_age) after the
/// anchor. A value type, safe to copy and share between threads.
class TrustedTime {
 public:
  /// @brief A UTC wall-clock instant at microsecond precision.
  using Time = Timestamp::Time;

  /// @brief How long after its anchor the time may be read: one hour.
  static constexpr std::chrono::hours max_age{1};

  /// @brief Holds an agreed time.
  ///
  /// @param source The provider whose servers agreed.
  /// @param synced_at UTC at the anchor.
  /// @param anchor_us The monotonic instant `synced_at` is true at, in
  ///     microseconds.
  constexpr TrustedTime(TimeSource source, Time synced_at,
                        std::uint64_t anchor_us) noexcept
      : source_(source), synced_at_(synced_at), anchor_us_(anchor_us) {}

  /// @brief Reads UTC at a monotonic instant.
  ///
  /// @param now_us The monotonic instant, in microseconds.
  /// @return UTC at `now_us`; nothing when `now_us` is before the anchor or
  ///     one
  ///     hour or more after it.
  constexpr auto at(std::uint64_t now_us) const noexcept
      -> std::optional<Time> {
    if (now_us < anchor_us_) return std::nullopt;
    const auto elapsed_us = now_us - anchor_us_;
    if (elapsed_us >= max_age_us) return std::nullopt;
    return synced_at_ +
           std::chrono::microseconds{static_cast<std::int64_t>(elapsed_us)};
  }

  /// @brief Which provider's servers agreed.
  ///
  /// @return The provider.
  constexpr auto source() const noexcept -> TimeSource { return source_; }

  /// @brief UTC at the anchor, when the time was agreed.
  ///
  /// @return The agreed time.
  constexpr auto synced_at() const noexcept -> Time { return synced_at_; }

  /// @brief Compares source, time and anchor.
  ///
  /// @return True when all three are equal.
  friend constexpr auto operator==(const TrustedTime&,
                                   const TrustedTime&) noexcept
      -> bool = default;

 private:
  /// @brief max_age in microseconds.
  static constexpr auto max_age_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(max_age).count());

  /// @brief The provider whose servers agreed.
  TimeSource source_;
  /// @brief UTC at the anchor.
  Time synced_at_;
  /// @brief The monotonic instant of the agreement, in microseconds.
  std::uint64_t anchor_us_;
};

}  // namespace libchiaking::net::ntp
