/// @file
/// @brief Declares RttSampler, which collects Senkusha's round-trip samples
/// and turns them into the statistics and MTU timeouts the test reports and
/// uses.
#pragma once

#include "src/core/net/protocol/senkusha/types.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::protocol::senkusha {

/// @brief The round-trip samples of one measurement, and the arithmetic over
/// them.
///
/// Each probe is one attempt; an attempt whose echo came back adds one
/// sample. A plain value used from one thread.
class RttSampler {
 public:
  /// @brief Reads how many probes have been settled, answered or lost.
  ///
  /// @return The attempt count.
  std::uint32_t attempted() const noexcept { return attempted_; }

  /// @brief Adds the round-trip time of an answered probe.
  ///
  /// @param sample Time from send to echo, on the monotonic clock.
  void record(std::chrono::microseconds sample) { samples_.push_back(sample); }

  /// @brief Counts one probe as settled, answered or not.
  void complete_attempt() noexcept { ++attempted_; }

  /// @brief Summarises the samples so far.
  ///
  /// @param adjustment The correction to apply to the mean.
  /// @return The statistics; see summarize().
  /// @retval Error See summarize().
  [[nodiscard]] auto summary(const RttAdjustmentPolicy& adjustment) const
      -> Expected<RttStatistics, Error> {
    return summarize(samples_, attempted_, adjustment);
  }

  /// @brief Computes count, mean, minimum, maximum and population variance
  /// of a set of samples, and the adjusted mean.
  ///
  /// @param successful_samples The answered probes' round-trip times; none
  ///     negative.
  /// @param attempted Probes sent; not fewer than the samples.
  /// @param adjustment The correction to apply to the mean; neither part
  ///     negative.
  /// @return The statistics.
  /// @retval Error::invalid_configuration More samples than
  ///     attempts, a negative sample, or a negative adjustment.
  /// @retval Error::no_rtt_samples No sample at all.
  /// @retval Error::arithmetic_overflow The sum or mean does not
  ///     fit.
  [[nodiscard]] static auto summarize(
      std::span<const std::chrono::microseconds> successful_samples,
      std::uint32_t attempted, const RttAdjustmentPolicy& adjustment)
      -> Expected<RttStatistics, Error>;

  /// @brief Picks the moment an echo should count as received.
  ///
  /// The time the socket was first seen readable is used when it falls at
  /// or after the send and before the read, since it is closer to the real
  /// arrival; otherwise the read time is used.
  ///
  /// @param sent_at_us When the probe was sent, in monotonic microseconds.
  /// @param readable_at_us When the socket was marked readable, if known.
  /// @param read_at_us When the echo was taken off the socket.
  /// @return The receive time to use.
  static constexpr auto pong_received_at(
      std::uint64_t sent_at_us, std::optional<std::uint64_t> readable_at_us,
      std::uint64_t read_at_us) noexcept -> std::uint64_t {
    return readable_at_us && *readable_at_us >= sent_at_us &&
                   *readable_at_us < read_at_us
               ? *readable_at_us
               : read_at_us;
  }

  /// @brief Computes how long one MTU attempt waits for its answer:
  /// `adjusted_rtt * timeout_multiplier`, in whole milliseconds rounded
  /// down, clamped to the floor and ceiling.
  ///
  /// @param adjusted_rtt The adjusted mean round-trip time; not negative.
  /// @param policy The multiplier, floor and ceiling.
  /// @return The timeout; 5 to 500 ms with the default policy.
  /// @retval Error::invalid_configuration A negative time, a zero
  ///     multiplier, or a non-positive or inverted floor and ceiling.
  /// @retval Error::arithmetic_overflow The product does not fit.
  [[nodiscard]] static auto mtu_timeout(std::chrono::microseconds adjusted_rtt,
                                        const MtuPolicy& policy)
      -> Expected<std::chrono::milliseconds, Error>;

 private:
  /// @brief Round-trip times of the answered probes, in arrival order.
  std::vector<std::chrono::microseconds> samples_;
  /// @brief Probes settled so far.
  std::uint32_t attempted_{};
};

}  // namespace libchiaking::net::protocol::senkusha
