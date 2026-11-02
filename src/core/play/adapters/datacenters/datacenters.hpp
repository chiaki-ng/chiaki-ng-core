/// @file
/// @brief Declares Datacenters, the in-memory view of the datacenter store
/// that cloud launches read and record measurements through.
#pragma once

#include "libchiaking/detail/catalog/platform.hpp"
#include "libchiaking/detail/play/datacenter.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/psn/gk/measurement.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace libchiaking::play {

/// @brief A bandwidth measurement still fresh enough to reuse.
struct CachedBandwidth {
  /// @brief The measured bandwidth, in kilobits per second.
  std::int32_t kbps{};
  /// @brief How long ago it was measured, in seconds of wall-clock time.
  std::chrono::seconds age{};
};

/// @brief The datacenter measurements of both cloud services, loaded once
/// from the DatacenterStore and written back when a launch learns something.
///
/// Each service's set is read from the store the first time it is needed and
/// kept afterwards. Every non-static member locks one mutex, so the view may
/// be shared by the Play thread and the session threads; a member that loads
/// or saves calls the store under that lock and blocks for as long as the
/// store does. The store must outlive the view. Not copyable.
class Datacenters {
 public:
  /// @brief One datacenter's measured round trips and path MTUs.
  using Ping = psn::gk::Ping;
  /// @brief The wall-clock time a measurement is stamped with.
  using Time = DatacenterMeasurement::Time;

  /// @brief Builds the view; nothing is loaded yet.
  ///
  /// @param threading Provider of the mutex guarding the loaded sets; must
  ///     outlive the view.
  /// @param store Where the measurements are kept; must outlive the view.
  Datacenters(platform::ThreadingProvider& threading, DatacenterStore& store);
  /// @brief Not copyable; one view owns the loaded sets.
  Datacenters(const Datacenters&) = delete;
  /// @brief Not copy-assignable; one view owns the loaded sets.
  auto operator=(const Datacenters&) -> Datacenters& = delete;

  /// @brief Turns a launch's measurements into storable ones.
  ///
  /// @param rows The launch's measurements.
  /// @param measured_at When they were taken.
  /// @return One measurement per row that was measured with a positive round
  ///     trip and a valid datacenter name; other rows are dropped.
  static auto measured(std::span<const Ping> rows, Time measured_at)
      -> std::vector<DatacenterMeasurement>;
  /// @brief Turns remembered measurements back into rows a launch can use as
  /// prior measurements.
  ///
  /// @param known The remembered set.
  /// @return One measured row per remembered datacenter.
  static auto prior_rows(const KnownDatacenters& known) -> std::vector<Ping>;
  /// @brief Builds the offer shown to the player from the picker rows.
  ///
  /// @param picker_rows The rows shown to the player.
  /// @param wait How long the player has to choose.
  /// @return The offer; a row counts as measured only with a positive round
  ///     trip.
  static auto offer_from(std::span<const Ping> picker_rows,
                         std::chrono::milliseconds wait) -> DatacenterOffer;

  /// @brief Gives the remembered measurements of one service, loading them on
  /// first use.
  ///
  /// @param service The cloud service.
  /// @return A copy of the set; empty when the store holds nothing or could
  ///     not be read.
  auto known(catalog::ServiceGeneration service) -> KnownDatacenters;
  /// @brief Merges a launch's fresh measurements into the remembered set and
  /// saves it.
  ///
  /// @param service The cloud service.
  /// @param fresh The launch's measurements.
  /// @param choice How the datacenter was chosen; a forced choice is not
  ///     recorded, since its measurement is a placeholder.
  /// @return `true` when the set changed and was saved; `false` when nothing
  ///     was recorded, nothing changed or the store could not be written.
  auto record(catalog::ServiceGeneration service,
              std::span<const DatacenterMeasurement> fresh,
              const DatacenterChoice& choice) -> bool;
  /// @brief Forgets every remembered measurement of one service, in memory and
  /// in the store.
  ///
  /// @param service The cloud service.
  /// @return Nothing on success; the store's error when it could not be
  ///     written, after the memory was cleared anyway.
  [[nodiscard]] auto forget(catalog::ServiceGeneration service)
      -> Expected<void, DatacenterStore::Error>;
  /// @brief Records a bandwidth measurement against a remembered datacenter and
  /// saves it.
  ///
  /// @param service The cloud service.
  /// @param name The datacenter name.
  /// @param kbps The measured bandwidth, in kilobits per second.
  /// @param measured_at When it was measured; the zero time is refused.
  /// @param round_trip The round trip at the time, kept to judge later reuse.
  /// @return `true` when the set changed and was saved; `false` for a
  ///     non-positive bandwidth, the zero time, an unknown datacenter, no
  ///     change or a failed save.
  auto note_bandwidth(catalog::ServiceGeneration service, std::string_view name,
                      std::int32_t kbps, Time measured_at,
                      std::chrono::milliseconds round_trip) -> bool;
  /// @brief Finds a remembered bandwidth that is still fit to reuse instead of
  /// measuring again.
  ///
  /// @param service The cloud service.
  /// @param name The datacenter name.
  /// @param round_trip The datacenter's round trip now.
  /// @param now The current wall-clock time.
  /// @return The bandwidth and its age; empty when none is remembered, it is
  ///     older than Datacenters::bandwidth_lifetime or from the future, or the
  ///     round trip moved by more than Datacenters::bandwidth_round_trip_drift.
  auto cached_bandwidth(catalog::ServiceGeneration service,
                        std::string_view name,
                        std::chrono::milliseconds round_trip, Time now)
      -> std::optional<CachedBandwidth>;

  /// @brief How long a bandwidth measurement may be reused.
  static constexpr std::chrono::minutes bandwidth_lifetime{30};
  /// @brief How far the round trip may have moved since the bandwidth was
  /// measured before it is measured again.
  static constexpr std::chrono::milliseconds bandwidth_round_trip_drift{15};

 private:
  /// @brief The number of cloud services with their own set: LegacyCloudStream
  /// and PS5 cloud.
  static constexpr std::size_t services = 2;

  /// @brief Maps a service to its slot in Datacenters::known_.
  ///
  /// @param service The cloud service.
  /// @return 0 for the old LegacyCloudStream service, 1 for PS5 cloud.
  static auto slot_of(catalog::ServiceGeneration service) noexcept
      -> std::size_t;

  /// @brief Gives the held set of one service, loading it from the store the
  /// first time; call with the mutex held.
  ///
  /// @param service The cloud service.
  /// @return The held set; empty when the store could not be read.
  auto loaded(catalog::ServiceGeneration service) -> KnownDatacenters&;

  /// @brief Where the measurements are kept.
  DatacenterStore& store_;
  /// @brief Guards Datacenters::known_ and every store call.
  platform::Mutex mutex_;
  /// @brief The loaded set of each service; empty until first needed.
  std::array<std::optional<KnownDatacenters>, services> known_{};
};

}  // namespace libchiaking::play
