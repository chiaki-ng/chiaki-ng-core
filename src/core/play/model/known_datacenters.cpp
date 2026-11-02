#include "libchiaking/detail/play/datacenter.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace libchiaking::play {

KnownDatacenters::KnownDatacenters(
    std::vector<DatacenterMeasurement> measurements) {
  for (auto& measurement : measurements) {
    const auto named = [&](const DatacenterMeasurement& kept) {
      return kept.name() == measurement.name();
    };
    if (std::ranges::none_of(measurements_, named))
      measurements_.push_back(std::move(measurement));
  }
  std::ranges::stable_sort(measurements_,
                           [](const DatacenterMeasurement& left,
                              const DatacenterMeasurement& right) {
                             if (left.round_trip() != right.round_trip())
                               return left.round_trip() < right.round_trip();
                             return left.name() < right.name();
                           });
  if (measurements_.size() > limit)
    measurements_.erase(
        measurements_.begin() + static_cast<std::ptrdiff_t>(limit),
        measurements_.end());
}

auto KnownDatacenters::find(const DatacenterName& name) const noexcept
    -> const DatacenterMeasurement* {
  const auto found =
      std::ranges::find(measurements_, name, &DatacenterMeasurement::name);
  return found == measurements_.end() ? nullptr : &*found;
}

auto KnownDatacenters::recording(
    std::span<const DatacenterMeasurement> fresh) const -> KnownDatacenters {
  auto merged = measurements_;
  for (const auto& measurement : fresh) {
    const auto kept = std::ranges::find(merged, measurement.name(),
                                        &DatacenterMeasurement::name);
    if (kept == merged.end()) {
      merged.push_back(measurement);
      continue;
    }
    const auto& earlier = kept->probe();
    const auto incoming =
        measurement.probe().bandwidth_kbps == 0 && earlier.bandwidth_kbps > 0
            ? measurement.with_bandwidth(earlier.bandwidth_kbps,
                                         earlier.bandwidth_measured_at,
                                         earlier.bandwidth_round_trip)
            : measurement;
    if (!kept->is_same_probe_as(incoming)) *kept = incoming;
  }
  return KnownDatacenters{std::move(merged)};
}

}  // namespace libchiaking::play
