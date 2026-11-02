#include "src/core/net/protocol/senkusha/rtt.hpp"

#include "libchiaking/platform/runtime/time_units.hpp"

#include <algorithm>
#include <limits>

namespace libchiaking::net::protocol::senkusha {

auto RttSampler::summarize(
    std::span<const std::chrono::microseconds> successful_samples,
    std::uint32_t attempted, const RttAdjustmentPolicy& adjustment)
    -> Expected<RttStatistics, Error> {
  if (successful_samples.size() > std::numeric_limits<std::uint32_t>::max() ||
      successful_samples.size() > attempted ||
      adjustment.subtract.count() < 0 || adjustment.floor.count() < 0) {
    return unexpected(Error::invalid_configuration);
  }
  if (successful_samples.empty()) return unexpected(Error::no_rtt_samples);

  std::uint64_t sum{};
  auto minimum = std::numeric_limits<std::int64_t>::max();
  std::int64_t maximum{};
  for (const auto sample : successful_samples) {
    if (sample.count() < 0) return unexpected(Error::invalid_configuration);
    const auto value = static_cast<std::uint64_t>(sample.count());
    if (value > std::numeric_limits<std::uint64_t>::max() - sum)
      return unexpected(Error::arithmetic_overflow);
    sum += value;
    minimum = std::min(minimum, sample.count());
    maximum = std::max(maximum, sample.count());
  }

  const auto successful = static_cast<std::uint32_t>(successful_samples.size());
  const auto raw_mean_value = sum / successful;
  if (raw_mean_value >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return unexpected(Error::arithmetic_overflow);
  }
  const auto raw_mean =
      std::chrono::microseconds{static_cast<std::int64_t>(raw_mean_value)};

  const auto subtract = static_cast<std::uint64_t>(adjustment.subtract.count());
  const auto floor = static_cast<std::uint64_t>(adjustment.floor.count());
  const auto can_subtract =
      floor <= raw_mean_value && subtract <= raw_mean_value - floor;
  const auto adjusted_value = can_subtract ? raw_mean_value - subtract : floor;
  if (adjusted_value >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return unexpected(Error::arithmetic_overflow);
  }

  const auto exact_mean =
      static_cast<long double>(sum) / static_cast<long double>(successful);
  long double variance{};
  for (const auto sample : successful_samples) {
    const auto difference =
        static_cast<long double>(sample.count()) - exact_mean;
    variance += difference * difference;
  }
  variance /= static_cast<long double>(successful);

  return RttStatistics{
      attempted,
      successful,
      attempted - successful,
      raw_mean,
      std::chrono::microseconds{minimum},
      std::chrono::microseconds{maximum},
      variance,
      std::chrono::microseconds{static_cast<std::int64_t>(adjusted_value)},
  };
}

auto RttSampler::mtu_timeout(std::chrono::microseconds adjusted_rtt,
                             const MtuPolicy& policy)
    -> Expected<std::chrono::milliseconds, Error> {
  if (adjusted_rtt.count() < 0 || policy.timeout_multiplier == 0 ||
      policy.timeout_floor.count() <= 0 ||
      policy.timeout_ceiling.count() <= 0 ||
      policy.timeout_floor > policy.timeout_ceiling) {
    return unexpected(Error::invalid_configuration);
  }
  const auto rtt = static_cast<std::uint64_t>(adjusted_rtt.count());
  const auto multiplier = static_cast<std::uint64_t>(policy.timeout_multiplier);
  if (rtt > std::numeric_limits<std::uint64_t>::max() / multiplier)
    return unexpected(Error::arithmetic_overflow);
  const auto milliseconds = (rtt * multiplier) / microseconds_per_millisecond;
  const auto floor = static_cast<std::uint64_t>(policy.timeout_floor.count());
  const auto ceiling =
      static_cast<std::uint64_t>(policy.timeout_ceiling.count());
  const auto clamped = std::clamp(milliseconds, floor, ceiling);
  if (clamped >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return unexpected(Error::arithmetic_overflow);
  }
  return std::chrono::milliseconds{static_cast<std::int64_t>(clamped)};
}

}  // namespace libchiaking::net::protocol::senkusha
