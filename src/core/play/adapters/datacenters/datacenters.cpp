#include "src/core/play/adapters/datacenters/datacenters.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>
#include <utility>

namespace libchiaking::play {

Datacenters::Datacenters(platform::ThreadingProvider& threading,
                         DatacenterStore& store)
    : store_(store), mutex_(threading) {}

auto Datacenters::measured(std::span<const Ping> rows, Time measured_at)
    -> std::vector<DatacenterMeasurement> {
  std::vector<DatacenterMeasurement> measurements;
  for (const auto& row : rows) {
    if (!row.measured || row.rtt_ms <= 0) continue;
    auto name = DatacenterName::parse(row.name);
    if (!name) continue;
    DatacenterMeasurement::Probe probe;
    for (const int sample : row.rtts_ms) probe.samples.emplace_back(sample);
    probe.mtu_in = row.mtu_in;
    probe.mtu_out = row.mtu_out;
    probe.port = row.port;
    probe.public_address = row.public_ip;
    probe.max_bandwidth = row.max_bandwidth;
    if (auto measurement = DatacenterMeasurement::create(
            std::move(*name), std::chrono::milliseconds{row.rtt_ms},
            measured_at, std::move(probe)))
      measurements.push_back(std::move(*measurement));
  }
  return measurements;
}

auto Datacenters::prior_rows(const KnownDatacenters& known)
    -> std::vector<Ping> {
  std::vector<Ping> rows;
  rows.reserve(known.size());
  for (const auto& measurement : known.all()) {
    Ping row;
    row.name = measurement.name().text();
    row.rtt_ms = static_cast<int>(measurement.round_trip().count());
    for (const auto sample : measurement.probe().samples)
      row.rtts_ms.push_back(static_cast<int>(sample.count()));
    row.mtu_in = measurement.probe().mtu_in;
    row.mtu_out = measurement.probe().mtu_out;
    row.port = measurement.probe().port;
    row.public_ip = measurement.probe().public_address;
    row.max_bandwidth = measurement.probe().max_bandwidth;
    row.measured = true;
    rows.push_back(std::move(row));
  }
  return rows;
}

auto Datacenters::offer_from(std::span<const Ping> picker_rows,
                             std::chrono::milliseconds wait)
    -> DatacenterOffer {
  std::vector<DatacenterOffer::Probe> probes;
  probes.reserve(picker_rows.size());
  for (const auto& row : picker_rows)
    probes.push_back({row.name, std::chrono::milliseconds{row.rtt_ms},
                      row.measured && row.rtt_ms > 0});
  return DatacenterOffer::from(probes, wait);
}

auto Datacenters::slot_of(catalog::ServiceGeneration service) noexcept
    -> std::size_t {
  return service == catalog::ServiceGeneration::old_service ? 0 : 1;
}

auto Datacenters::loaded(catalog::ServiceGeneration service)
    -> KnownDatacenters& {
  auto& held = known_[slot_of(service)];
  if (!held) {
    auto stored = store_.load(service);
    held.emplace(stored ? KnownDatacenters{std::move(stored.value())}
                        : KnownDatacenters{});
  }
  return *held;
}

auto Datacenters::known(catalog::ServiceGeneration service)
    -> KnownDatacenters {
  const platform::Lock lock{mutex_};
  return loaded(service);
}

auto Datacenters::record(catalog::ServiceGeneration service,
                         std::span<const DatacenterMeasurement> fresh,
                         const DatacenterChoice& choice) -> bool {
  if (choice.was_forced() || fresh.empty()) return false;
  const platform::Lock lock{mutex_};
  auto& held = loaded(service);
  auto merged = held.recording(fresh);
  if (merged == held) return false;
  held = std::move(merged);
  return static_cast<bool>(store_.save(service, held.all()));
}

auto Datacenters::note_bandwidth(catalog::ServiceGeneration service,
                                 std::string_view name, std::int32_t kbps,
                                 Time measured_at,
                                 std::chrono::milliseconds round_trip) -> bool {
  if (kbps <= 0 || measured_at == Time{}) return false;
  const platform::Lock lock{mutex_};
  auto& held = loaded(service);
  const auto& all = held.all();
  const auto known = std::ranges::find_if(
      all, [name](const DatacenterMeasurement& measurement) {
        return measurement.name().text() == name;
      });
  if (known == all.end()) return false;
  const std::array updated{
      known->with_bandwidth(kbps, measured_at, round_trip)};
  auto merged = held.recording(updated);
  if (merged == held) return false;
  held = std::move(merged);
  return static_cast<bool>(store_.save(service, held.all()));
}

auto Datacenters::cached_bandwidth(catalog::ServiceGeneration service,
                                   std::string_view name,
                                   std::chrono::milliseconds round_trip,
                                   Time now) -> std::optional<CachedBandwidth> {
  const platform::Lock lock{mutex_};
  const auto& all = loaded(service).all();
  const auto known = std::ranges::find_if(
      all, [name](const DatacenterMeasurement& measurement) {
        return measurement.name().text() == name;
      });
  if (known == all.end()) return std::nullopt;
  const auto& probe = known->probe();
  const auto age = now - probe.bandwidth_measured_at;
  const auto drift = round_trip > probe.bandwidth_round_trip
                         ? round_trip - probe.bandwidth_round_trip
                         : probe.bandwidth_round_trip - round_trip;
  if (probe.bandwidth_kbps <= 0 || now == Time{} ||
      probe.bandwidth_measured_at == Time{} ||
      age < std::chrono::milliseconds{0} || age > bandwidth_lifetime ||
      drift > bandwidth_round_trip_drift)
    return std::nullopt;
  return CachedBandwidth{probe.bandwidth_kbps,
                         std::chrono::duration_cast<std::chrono::seconds>(age)};
}

auto Datacenters::forget(catalog::ServiceGeneration service)
    -> Expected<void, DatacenterStore::Error> {
  const platform::Lock lock{mutex_};
  known_[slot_of(service)].emplace();
  return store_.save(service, {});
}

}  // namespace libchiaking::play
