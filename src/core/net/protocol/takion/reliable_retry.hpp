/// @file
/// @brief Declares the retry rules and round-trip estimator of Takion's
/// reliable channel.
#pragma once

#include <libchiaking/platform/runtime/time_units.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace libchiaking::net::protocol::takion {

/// @brief Age, in milliseconds, past which a fragment is given up on whatever
/// its retry count (15 s).
inline constexpr std::uint64_t ordinary_retry_age_limit_ms = 15'000;
/// @brief Round-trip samples at or above this, in milliseconds, are ignored
/// (2 s).
inline constexpr std::uint64_t round_trip_sample_ceiling_ms = 2'000;
/// @brief Transmissions beyond the retry budget at which a fragment is
/// discarded: with `maximum_retries` retries a fragment is sent at most
/// `maximum_retries + 1` times.
inline constexpr std::uint32_t retries_to_discarding_attempt = 2;

/// @brief The decision made before each transmission of a reliable fragment.
struct DataAttemptDecision {
  /// @brief Computes the transmission count at which a fragment is discarded.
  ///
  /// @param maximum_retries The configured retry budget.
  /// @return `maximum_retries + 2`.
  static constexpr std::uint32_t discarding_attempt(
      std::uint32_t maximum_retries) noexcept {
    return maximum_retries + retries_to_discarding_attempt;
  }

  /// @brief Decides the next transmission of a fragment.
  ///
  /// @param transmission_count Transmissions so far.
  /// @param accumulated_ms Age of the fragment, in milliseconds.
  /// @param discarding_attempt_count From discarding_attempt().
  /// @return The new count, and whether to discard instead of sending.
  static constexpr DataAttemptDecision next(
      std::uint32_t transmission_count, std::uint64_t accumulated_ms,
      std::uint32_t discarding_attempt_count) noexcept {
    const auto count = transmission_count + 1U;
    return {count, accumulated_ms > ordinary_retry_age_limit_ms ||
                       count >= discarding_attempt_count};
  }

  /// @brief The fragment's transmission count after this decision.
  std::uint32_t transmission_count{};
  /// @brief Whether the fragment is given up instead of sent.
  bool discard{};

  /// @brief Compares both fields.
  ///
  /// @return Whether both decisions are the same.
  bool operator==(const DataAttemptDecision&) const = default;
};

/// @brief Estimates the control channel's round-trip time from acknowledged
/// fragments and derives the retransmission timeout from it.
///
/// The smoothing is TCP's (RFC 6298) in whole milliseconds: the first sample
/// sets the mean and half of it as the variation; later ones update the
/// variation by 1/4 and the mean by 1/8. The timeout is mean + 4 x variation,
/// clamped to the bounds. A plain value used under the client's lock.
class RoundTripEstimator {
 public:
  /// @brief Starts with no samples and an initial timeout.
  ///
  /// @param initial_timeout_us Timeout before any sample, in microseconds.
  /// @param minimum_timeout_us Lower clamp, in microseconds.
  /// @param maximum_timeout_us Upper clamp, in microseconds.
  constexpr RoundTripEstimator(std::uint64_t initial_timeout_us,
                               std::uint64_t minimum_timeout_us,
                               std::uint64_t maximum_timeout_us) noexcept
      : timeout_us_(initial_timeout_us),
        minimum_timeout_us_(minimum_timeout_us),
        maximum_timeout_us_(maximum_timeout_us) {}

  /// @brief Folds in one sample.
  ///
  /// @param sample_ms A round-trip time, in milliseconds, from a fragment
  ///     acknowledged after its first transmission; 2000 or more is ignored.
  constexpr void observe(std::uint64_t sample_ms) noexcept {
    if (sample_ms >= round_trip_sample_ceiling_ms) return;
    ++samples_;
    if (smoothed_ms_ != 0) {
      const auto deviation = sample_ms > smoothed_ms_
                                 ? sample_ms - smoothed_ms_
                                 : smoothed_ms_ - sample_ms;
      variation_ms_ = (3 * variation_ms_ + deviation) / 4;
      smoothed_ms_ = (sample_ms + 7 * smoothed_ms_) / 8;
    } else {
      smoothed_ms_ = sample_ms;
      variation_ms_ = sample_ms / 2;
    }
    timeout_us_ = std::clamp(
        (smoothed_ms_ + 4 * variation_ms_) * microseconds_per_millisecond,
        minimum_timeout_us_, maximum_timeout_us_);
  }

  /// @brief Reads the smoothed round-trip time.
  ///
  /// @return Milliseconds; 0 before any sample.
  constexpr std::uint64_t smoothed_ms() const noexcept { return smoothed_ms_; }
  /// @brief Reads the round-trip variation.
  ///
  /// @return Milliseconds; 0 before any sample.
  constexpr std::uint64_t variation_ms() const noexcept {
    return variation_ms_;
  }
  /// @brief Reads the retransmission timeout.
  ///
  /// @return Microseconds.
  constexpr std::uint64_t timeout_us() const noexcept { return timeout_us_; }
  /// @brief Reads how many samples were taken.
  ///
  /// @return The count.
  constexpr std::uint64_t samples() const noexcept { return samples_; }

 private:
  /// @brief Smoothed round-trip time, in milliseconds.
  std::uint64_t smoothed_ms_{};
  /// @brief Round-trip variation, in milliseconds.
  std::uint64_t variation_ms_{};
  /// @brief Current retransmission timeout, in microseconds.
  std::uint64_t timeout_us_{};
  /// @brief Lower timeout clamp, in microseconds.
  std::uint64_t minimum_timeout_us_{};
  /// @brief Upper timeout clamp, in microseconds.
  std::uint64_t maximum_timeout_us_{};
  /// @brief Samples taken.
  std::uint64_t samples_{};
};

/// @brief How many of the latest round-trip samples RoundTripSamples keeps.
inline constexpr std::size_t round_trip_sample_capacity = 4096;
/// @brief How far back RoundTripSpread::recent_max_us looks, in microseconds.
inline constexpr std::uint64_t round_trip_recent_window_us = 10'000'000;
/// @brief How many samples the recent window holds at most; older ones in the
/// window are dropped first.
inline constexpr std::size_t round_trip_recent_capacity = 256;

/// @brief Percentiles of the latest round-trip samples.
struct RoundTripSpread {
  /// @brief Samples taken in all, including those no longer kept.
  std::uint64_t samples{};
  /// @brief The median, in microseconds.
  std::uint64_t median_us{};
  /// @brief The 90th percentile, in microseconds.
  std::uint64_t p90_us{};
  /// @brief The 99th percentile, in microseconds.
  std::uint64_t p99_us{};
  /// @brief The largest, in microseconds.
  std::uint64_t max_us{};
  /// @brief The largest of the samples taken within
  /// round_trip_recent_window_us of when the spread was read, in
  /// microseconds; 0 when none were.
  std::uint64_t recent_max_us{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const RoundTripSpread&) const = default;
};

/// @brief Keeps the latest round-trip samples at microsecond resolution and
/// their percentiles, so runs can be compared beyond the smoothed estimate.
///
/// The percentiles are recomputed on every sample, which arrive a few times a
/// second. A plain value used under the client's lock.
class RoundTripSamples {
 public:
  /// @brief Records one sample.
  ///
  /// @param sample_us A round-trip time, in microseconds, from a fragment
  ///     acknowledged after its first transmission; 2 s or more is ignored.
  /// @param now_us When it was taken, in microseconds on a monotonic clock.
  void observe(std::uint64_t sample_us, std::uint64_t now_us) noexcept {
    if (sample_us >=
        round_trip_sample_ceiling_ms * microseconds_per_millisecond)
      return;
    kept_[next_] = static_cast<std::uint32_t>(sample_us);
    next_ = (next_ + 1) % round_trip_sample_capacity;
    recent_[recent_next_] = {now_us, static_cast<std::uint32_t>(sample_us)};
    recent_next_ = (recent_next_ + 1) % round_trip_recent_capacity;
    recent_count_ = std::min(recent_count_ + 1, round_trip_recent_capacity);
    ++spread_.samples;
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(spread_.samples, round_trip_sample_capacity));
    std::copy_n(kept_.begin(), count, sorted_.begin());
    std::sort(sorted_.begin(), sorted_.begin() + count);
    const auto at = [&](std::size_t percent) -> std::uint64_t {
      return sorted_[(count - 1) * percent / 100];
    };
    spread_.median_us = at(50);
    spread_.p90_us = at(90);
    spread_.p99_us = at(99);
    spread_.max_us = sorted_[count - 1];
  }

  /// @brief Reads the percentiles and the recent largest sample.
  ///
  /// @param now_us The current time, in microseconds on the clock observe()
  ///     was given.
  /// @return The percentiles; all zero before any sample.
  RoundTripSpread spread(std::uint64_t now_us) const noexcept {
    auto spread = spread_;
    for (std::size_t index = 0; index < recent_count_; ++index) {
      const auto& recent = recent_[index];
      if (now_us >= recent.taken_us &&
          now_us - recent.taken_us <= round_trip_recent_window_us)
        spread.recent_max_us =
            std::max<std::uint64_t>(spread.recent_max_us, recent.sample_us);
    }
    return spread;
  }

 private:
  /// @brief The latest samples, oldest overwritten first, in microseconds.
  std::array<std::uint32_t, round_trip_sample_capacity> kept_{};
  /// @brief Scratch space the kept samples are sorted in.
  std::array<std::uint32_t, round_trip_sample_capacity> sorted_{};
  /// @brief Where the next sample goes in kept_.
  std::size_t next_{};
  /// @brief A recent sample with when it was taken.
  struct Recent {
    /// @brief When it was taken, in microseconds.
    std::uint64_t taken_us{};
    /// @brief The sample, in microseconds.
    std::uint32_t sample_us{};
  };
  /// @brief The latest samples with their times, oldest overwritten first.
  std::array<Recent, round_trip_recent_capacity> recent_{};
  /// @brief Where the next sample goes in recent_.
  std::size_t recent_next_{};
  /// @brief How many slots of recent_ hold a sample.
  std::size_t recent_count_{};
  /// @brief The percentiles of the kept samples; recent_max_us stays 0.
  RoundTripSpread spread_{};
};

}  // namespace libchiaking::net::protocol::takion
