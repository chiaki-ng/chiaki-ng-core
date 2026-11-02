/// @file
/// @brief Declares Timestamp, the 64-bit NTP timestamp format carried on the
/// wire by the SNTP client.
#pragma once

#include <chrono>
#include <cstdint>

namespace libchiaking::net::ntp {

/// @brief An NTP timestamp: 32 bits of seconds since 1900-01-01 00:00 UTC
/// followed by 32 bits of binary fraction of a second (RFC 5905 section 6).
///
/// Held exactly as the 64-bit wire value, so one sent as a nonce compares bit
/// for
/// bit with the one echoed back. Conversions to and from system time are at
/// microsecond precision. Seconds values below 2^31 are read as NTP era 1 (from
/// 2036-02-07), so the type is valid from 1968 to 2104. A value type, safe to
/// copy and share between threads.
class Timestamp {
 public:
  /// @brief A UTC wall-clock instant at microsecond precision.
  using Time = std::chrono::sys_time<std::chrono::microseconds>;

  /// @brief The zero timestamp, which NTP uses to mean "not set".
  constexpr Timestamp() noexcept = default;
  /// @brief Holds a timestamp read from the wire or drawn as a nonce.
  ///
  /// @param wire The 64-bit value in host order: seconds in the high 32 bits,
  ///     fraction in the low 32 bits.
  explicit constexpr Timestamp(std::uint64_t wire) noexcept : wire_(wire) {}

  /// @brief Encodes a wall-clock instant.
  ///
  /// @param time The instant; the seconds field holds instants from 1900 to
  ///     2036, later instants wrap into the next era.
  /// @return The timestamp, its fraction rounded down to the nearest 2^-32 s.
  static constexpr auto at(Time time) noexcept -> Timestamp {
    const auto since_unix = time.time_since_epoch();
    const auto whole = std::chrono::floor<std::chrono::seconds>(since_unix);
    const auto fraction = since_unix - whole;
    const auto seconds =
        static_cast<std::uint64_t>(whole.count() + unix_offset_seconds);
    const auto scaled = (static_cast<std::uint64_t>(fraction.count()) << 32U) /
                        micros_per_second;
    return Timestamp{(seconds << 32U) | (scaled & fraction_mask)};
  }

  /// @brief The 64-bit wire value in host order.
  ///
  /// @return Seconds in the high 32 bits, fraction in the low 32 bits.
  constexpr auto wire() const noexcept -> std::uint64_t { return wire_; }

  /// @brief Whether this is the unset timestamp.
  ///
  /// @return True when every bit is zero.
  constexpr auto is_zero() const noexcept -> bool { return wire_ == 0; }

  /// @brief Decodes the timestamp to a wall-clock instant.
  ///
  /// Seconds below 2^31 are taken to be in era 1, after 2036-02-07.
  ///
  /// @return The instant, the fraction rounded down to the microsecond.
  constexpr auto time() const noexcept -> Time {
    auto seconds = static_cast<std::int64_t>(wire_ >> 32U);
    if (seconds < era_pivot_seconds) seconds += era_seconds;
    const auto micros = ((wire_ & fraction_mask) * micros_per_second) >> 32U;
    return Time{std::chrono::seconds{seconds - unix_offset_seconds} +
                std::chrono::microseconds{static_cast<std::int64_t>(micros)}};
  }

  /// @brief Compares the wire values bit for bit.
  ///
  /// @return True when both carry the same 64 bits.
  friend constexpr auto operator==(Timestamp, Timestamp) noexcept
      -> bool = default;

 private:
  /// @brief Seconds from the NTP epoch (1900) to the Unix epoch (1970).
  static constexpr std::int64_t unix_offset_seconds = 2'208'988'800;
  /// @brief Seconds in one NTP era: 2^32.
  static constexpr std::int64_t era_seconds = std::int64_t{1} << 32U;
  /// @brief Seconds field below which a timestamp is taken to be in era 1:
  /// 2^31, which is 1968-01-20.
  static constexpr std::int64_t era_pivot_seconds = std::int64_t{1} << 31U;
  /// @brief Mask of the low 32 bits, the fraction of a second.
  static constexpr std::uint64_t fraction_mask = 0xffff'ffffU;
  /// @brief Microseconds in a second.
  static constexpr std::uint64_t micros_per_second = 1'000'000U;

  /// @brief The 64-bit wire value in host order; zero when unset.
  std::uint64_t wire_{};
};

}  // namespace libchiaking::net::ntp
