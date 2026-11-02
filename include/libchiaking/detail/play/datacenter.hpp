/// @file
/// @brief Declares cloud datacenter names, latency measurements, offers,
/// choices and preferences, and DatacenterStore, the storage port a frontend
/// implements to keep the measurements between runs.
#pragma once

#include "libchiaking/detail/catalog/platform.hpp"
#include "libchiaking/detail/expected.hpp"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::play {

/// @brief Holds the validated name of one cloud streaming datacenter.
///
/// Names come from PlayStation's cloud service and are shown to the player as
/// they are. A DatacenterName is never empty and never the reserved word
/// DatacenterName::automatic.
class DatacenterName {
 public:
  /// @brief Longest name accepted, in bytes.
  static constexpr std::size_t limit = 256;
  /// @brief Reserved word meaning "let the service choose"; never a valid
  /// name.
  static constexpr std::string_view automatic = "Auto";

  /// @brief Validates a datacenter name.
  ///
  /// @param text Name as reported by the service or stored by the frontend.
  /// @return The name, or `std::nullopt` when `text` is empty, equals
  ///     DatacenterName::automatic, is longer than DatacenterName::limit or
  ///     contains a control character.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<DatacenterName>;

  /// @brief Returns the name as text, suitable for display and for storing.
  ///
  /// @return The name as text, suitable for display and for storing.
  auto text() const noexcept -> const std::string& { return text_; }

  /// @brief Compares two names byte for byte.
  friend auto operator==(const DatacenterName&, const DatacenterName&)
      -> bool = default;
  /// @brief Orders names byte-wise, so they can key ordered containers.
  friend auto operator<=>(const DatacenterName&, const DatacenterName&)
      -> std::strong_ordering = default;
  /// @brief Compares a name with raw text byte for byte.
  friend auto operator==(const DatacenterName& name, std::string_view text)
      -> bool {
    return name.text_ == text;
  }

 private:
  explicit DatacenterName(std::string_view text) : text_(text) {}

  std::string text_;
};

/// @brief Records how long one datacenter took to answer a latency probe.
///
/// Measurements are taken while a cloud launch looks for a server. They are
/// reported through CloudLaunch::measurements, kept between launches through
/// DatacenterStore, and read back with Play::known_datacenters so a frontend
/// can list datacenters before the next launch.
class DatacenterMeasurement {
 public:
  /// @brief Round-trip time of a probe, in whole milliseconds.
  using RoundTrip = std::chrono::milliseconds;
  /// @brief Wall-clock time of a measurement, in milliseconds since the Unix
  /// epoch.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;

  /// @brief Round trip above which a datacenter counts as high latency.
  static constexpr RoundTrip high_latency_threshold{100};

  /// @brief Raw detail of one probe, kept so it can be stored and reused.
  ///
  /// A frontend that implements DatacenterStore must persist these fields and
  /// return them unchanged. They are not meant for display.
  struct Probe {
    /// @brief Individual round-trip samples behind the summarised round
    /// trip.
    std::vector<RoundTrip> samples{};
    /// @brief Inbound MTU the probe reported.
    std::int32_t mtu_in{};
    /// @brief Outbound MTU the probe reported.
    std::int32_t mtu_out{};
    /// @brief Port the probe reported.
    std::int32_t port{};
    /// @brief Public address of this client as the datacenter saw it.
    std::string public_address{};
    /// @brief Bandwidth ceiling the probe reported.
    std::int32_t max_bandwidth{};
    /// @brief Downstream bandwidth a bandwidth test measured, in kilobits
    /// per second; zero when none was measured.
    std::int32_t bandwidth_kbps{};
    /// @brief When the bandwidth was measured; meaningless while
    /// Probe::bandwidth_kbps is zero.
    Time bandwidth_measured_at{};
    /// @brief Round trip to the datacenter when the bandwidth was measured,
    /// used to tell whether the network has changed since.
    RoundTrip bandwidth_round_trip{};

    /// @brief Compares every field, samples included.
    friend auto operator==(const Probe&, const Probe&) -> bool = default;
  };

  /// @brief Builds a measurement with its raw probe detail.
  ///
  /// @param name Datacenter that answered.
  /// @param round_trip Summarised round trip; must be positive.
  /// @param measured_at When the probe was taken.
  /// @param probe Raw probe detail.
  /// @return The measurement, or `std::nullopt` when `round_trip` is zero or
  ///     negative.
  [[nodiscard]] static auto create(DatacenterName name, RoundTrip round_trip,
                                   Time measured_at, Probe probe)
      -> std::optional<DatacenterMeasurement> {
    if (round_trip.count() <= 0) return std::nullopt;
    return DatacenterMeasurement{std::move(name), round_trip, measured_at,
                                 std::move(probe)};
  }

  /// @brief Builds a measurement that has no raw probe detail.
  ///
  /// @param name Datacenter that answered.
  /// @param round_trip Summarised round trip; must be positive.
  /// @param measured_at When the probe was taken.
  /// @return The measurement, or `std::nullopt` when `round_trip` is zero or
  ///     negative.
  [[nodiscard]] static auto create(DatacenterName name, RoundTrip round_trip,
                                   Time measured_at)
      -> std::optional<DatacenterMeasurement> {
    return create(std::move(name), round_trip, measured_at, Probe{});
  }

  /// @brief Returns the datacenter that was measured.
  ///
  /// @return The datacenter that was measured.
  auto name() const noexcept -> const DatacenterName& { return name_; }
  /// @brief Returns the round trip; always positive.
  ///
  /// @return The round trip; always positive.
  auto round_trip() const noexcept -> RoundTrip { return round_trip_; }
  /// @brief Returns when the probe was taken.
  ///
  /// The library stamps measurements with PlayStation's server clock, not the
  /// local clock. The value is the epoch when that clock was not known at the
  /// time.
  ///
  /// @return When the probe was taken.
  auto measured_at() const noexcept -> Time { return measured_at_; }
  /// @brief Returns the raw probe detail.
  ///
  /// @return The raw probe detail.
  auto probe() const noexcept -> const Probe& { return probe_; }

  /// @brief Returns a copy that carries a bandwidth measurement.
  ///
  /// @param kbps Measured downstream bandwidth in kilobits per second.
  /// @param measured_at When the bandwidth was measured.
  /// @param round_trip Round trip to the datacenter at that time.
  /// @return This measurement with Probe::bandwidth_kbps,
  ///     Probe::bandwidth_measured_at and Probe::bandwidth_round_trip set.
  auto with_bandwidth(std::int32_t kbps, Time measured_at,
                      RoundTrip round_trip) const -> DatacenterMeasurement {
    auto probe = probe_;
    probe.bandwidth_kbps = kbps;
    probe.bandwidth_measured_at = measured_at;
    probe.bandwidth_round_trip = round_trip;
    return DatacenterMeasurement{name_, round_trip_, measured_at_,
                                 std::move(probe)};
  }
  /// @brief Reports whether the round trip exceeds
  /// DatacenterMeasurement::high_latency_threshold.
  ///
  /// @return `true` when the round trip exceeds
  ///     DatacenterMeasurement::high_latency_threshold.
  auto is_high_latency() const noexcept -> bool {
    return round_trip_ > high_latency_threshold;
  }
  /// @brief Reports whether two measurements describe the same probe result.
  ///
  /// @param other Measurement to compare with.
  /// @return `true` when name, round trip and raw probe detail match. The
  ///     time of measurement is ignored, so an identical result seen again
  ///     does not count as new.
  auto is_same_probe_as(const DatacenterMeasurement& other) const noexcept
      -> bool {
    return name_ == other.name_ && round_trip_ == other.round_trip_ &&
           probe_ == other.probe_;
  }

  /// @brief Compares every field, the time of measurement included.
  friend auto operator==(const DatacenterMeasurement&,
                         const DatacenterMeasurement&) -> bool = default;

 private:
  DatacenterMeasurement(DatacenterName name, RoundTrip round_trip,
                        Time measured_at, Probe probe) noexcept
      : name_(std::move(name)),
        round_trip_(round_trip),
        measured_at_(measured_at),
        probe_(std::move(probe)) {}

  DatacenterName name_;
  RoundTrip round_trip_;
  Time measured_at_;
  Probe probe_;
};

/// @brief Holds the most recent measurement of each datacenter of one cloud
/// service.
///
/// The set is a value: it holds at most one measurement per datacenter, ordered
/// by ascending round trip and then by name, and never more than
/// KnownDatacenters::limit entries.
///
/// @see Play::known_datacenters
class KnownDatacenters {
 public:
  /// @brief Largest number of datacenters kept; the slowest are dropped
  /// beyond it.
  static constexpr std::size_t limit = 64;

  /// @brief Creates an empty set.
  KnownDatacenters() noexcept = default;
  /// @brief Builds the set from stored measurements.
  ///
  /// @param measurements Measurements in any order. When a name appears more
  ///     than once the first occurrence is kept. The result is sorted and cut
  ///     to KnownDatacenters::limit.
  explicit KnownDatacenters(std::vector<DatacenterMeasurement> measurements);

  /// @brief Returns the measurements, fastest first.
  ///
  /// @return A view into this set, valid until the set is destroyed or
  ///     assigned.
  auto all() const noexcept -> std::span<const DatacenterMeasurement> {
    return measurements_;
  }
  /// @brief Returns the number of datacenters known.
  ///
  /// @return The number of datacenters known.
  auto size() const noexcept -> std::size_t { return measurements_.size(); }
  /// @brief Reports whether no datacenter is known.
  ///
  /// @return `true` when no datacenter is known.
  auto empty() const noexcept -> bool { return measurements_.empty(); }
  /// @brief Looks up the measurement of one datacenter.
  ///
  /// @param name Datacenter to look up.
  /// @return The measurement, owned by this set, or `nullptr` when that
  ///     datacenter has not been measured.
  auto find(const DatacenterName& name) const noexcept
      -> const DatacenterMeasurement*;
  /// @brief Returns a copy of this set updated with newer measurements.
  ///
  /// @param fresh Measurements from a launch. A datacenter not yet known is
  ///     added. A known one is replaced unless the new measurement is the
  ///     same probe result (DatacenterMeasurement::is_same_probe_as). A
  ///     replacement without a bandwidth measurement keeps the earlier
  ///     measurement's bandwidth.
  /// @return The merged set; this set is left unchanged. It compares equal to
  ///     this set when nothing new was learned.
  auto recording(std::span<const DatacenterMeasurement> fresh) const
      -> KnownDatacenters;

  /// @brief Compares the measurements held.
  friend auto operator==(const KnownDatacenters&, const KnownDatacenters&)
      -> bool = default;

 private:
  std::vector<DatacenterMeasurement> measurements_;
};

/// @brief Lists the datacenters a player may pick from while a cloud launch
/// waits.
///
/// An offer exists only when the launch was started with
/// DatacenterPreference::ask_at_launch. It is delivered through
/// SessionEvents::on_datacenters_offered and answered with
/// CloudLaunch::choose_datacenter or CloudLaunch::choose_lowest_latency.
class DatacenterOffer {
 public:
  /// @brief Round-trip time of a probe, in whole milliseconds.
  using RoundTrip = std::chrono::milliseconds;

  /// @brief Round trip above which an offered datacenter counts as high
  /// latency.
  static constexpr RoundTrip high_latency_threshold =
      DatacenterMeasurement::high_latency_threshold;
  /// @brief Time a launch waits for the player's choice unless told
  /// otherwise.
  static constexpr std::chrono::milliseconds default_wait{15000};
  /// @brief Shortest wait an offer can carry; shorter waits are raised to it.
  static constexpr std::chrono::milliseconds minimum_wait{1000};

  /// @brief Raw probe result an offer is built from.
  struct Probe {
    /// @brief Datacenter name as reported; entries with invalid names are
    /// left out.
    std::string name{};
    /// @brief Measured round trip; only meaningful when `answered` is set.
    RoundTrip round_trip{};
    /// @brief Whether the datacenter answered the probe.
    bool answered{};
  };

  /// @brief Describes one datacenter in an offer.
  class Entry {
   public:
    /// @brief Returns the datacenter's name, to show and to pass to
    /// CloudLaunch::choose_datacenter.
    ///
    /// @return The datacenter's name, to show and to pass to
    ///     CloudLaunch::choose_datacenter.
    auto name() const noexcept -> const DatacenterName& { return name_; }
    /// @brief Returns the measured round trip, or `std::nullopt` when the
    /// datacenter did not answer.
    ///
    /// @return The measured round trip, or `std::nullopt` when the
    ///     datacenter did not answer.
    auto round_trip() const noexcept -> std::optional<RoundTrip> {
      return round_trip_;
    }
    /// @brief Reports whether the datacenter answered the latency probe.
    ///
    /// @return `true` when the datacenter answered the latency probe.
    auto answered() const noexcept -> bool { return round_trip_.has_value(); }
    /// @brief Reports whether the datacenter is a poor choice for latency.
    ///
    /// @return `true` when it did not answer or its round trip exceeds
    ///     DatacenterOffer::high_latency_threshold.
    auto is_high_latency() const noexcept -> bool {
      return !round_trip_ || *round_trip_ > high_latency_threshold;
    }

    /// @brief Compares name and round trip.
    friend auto operator==(const Entry&, const Entry&) -> bool = default;

   private:
    friend class DatacenterOffer;

    Entry(DatacenterName name, std::optional<RoundTrip> round_trip) noexcept
        : name_(std::move(name)), round_trip_(round_trip) {}

    DatacenterName name_;
    std::optional<RoundTrip> round_trip_;
  };

  /// @brief Creates an empty offer carrying DatacenterOffer::default_wait.
  DatacenterOffer() noexcept = default;

  /// @brief Builds an offer from probe results.
  ///
  /// Results with an invalid or repeated name are dropped. Datacenters that
  /// answered come first, fastest first; those that did not answer follow in
  /// the order given.
  ///
  /// @param probes Probe results, one per datacenter.
  /// @param wait Time the launch will wait for a choice; raised to
  ///     DatacenterOffer::minimum_wait when shorter.
  /// @return The offer.
  static auto from(std::span<const Probe> probes,
                   std::chrono::milliseconds wait = default_wait)
      -> DatacenterOffer;

  /// @brief Returns the offered datacenters, answered ones first and fastest
  /// first.
  ///
  /// @return A view into this offer, valid until the offer is destroyed or
  ///     assigned.
  auto entries() const noexcept -> std::span<const Entry> { return entries_; }
  /// @brief Reports whether nothing is offered.
  ///
  /// @return `true` when nothing is offered.
  auto empty() const noexcept -> bool { return entries_.empty(); }
  /// @brief Returns how long the launch waits for a choice.
  ///
  /// When the time runs out without a choice the launch continues with the
  /// lowest-latency datacenter, reported as
  /// DatacenterChoice::How::lowest_after_waiting.
  ///
  /// @return How long the launch waits for a choice.
  auto wait() const noexcept -> std::chrono::milliseconds { return wait_; }
  /// @brief Returns the fastest datacenter that answered.
  ///
  /// @return The entry, owned by this offer, or `nullptr` when none answered.
  auto lowest() const noexcept -> const Entry*;
  /// @brief Looks up an offered datacenter by name.
  ///
  /// @param name Datacenter to look up.
  /// @return The entry, owned by this offer, or `nullptr` when it is not
  ///     offered.
  auto find(const DatacenterName& name) const noexcept -> const Entry*;
  /// @brief Reports whether a datacenter is part of this offer.
  ///
  /// @param name Datacenter to look up.
  /// @return `true` when a datacenter is part of this offer.
  auto offers(const DatacenterName& name) const noexcept -> bool {
    return find(name) != nullptr;
  }
  /// @brief Reports whether an entry is the fastest one that answered.
  ///
  /// @param entry Entry to test; compared by name.
  /// @return `true` when an entry is the fastest one that answered.
  auto is_lowest(const Entry& entry) const noexcept -> bool;

  /// @brief Compares the entries and the wait.
  friend auto operator==(const DatacenterOffer&, const DatacenterOffer&)
      -> bool = default;

 private:
  std::vector<Entry> entries_;
  std::chrono::milliseconds wait_{default_wait};
};

/// @brief Reports which datacenter a cloud launch settled on, and how.
///
/// It is delivered through SessionEvents::on_datacenter_chosen and can be read
/// later from CloudLaunch::chosen.
class DatacenterChoice {
 public:
  /// @brief Round-trip time, in whole milliseconds.
  using RoundTrip = std::chrono::milliseconds;
  /// @brief Wall-clock time of a stored measurement.
  using Time = DatacenterMeasurement::Time;

  /// @brief Names the way the datacenter was decided.
  enum class How : std::uint8_t {
    /// @brief The datacenter was chosen automatically and the player was
    /// not asked.
    automatic,
    /// @brief The player's stored preference (DatacenterPreference::named)
    /// was used.
    preferred,
    /// @brief The player picked this datacenter from the offer during the
    /// launch.
    picked_at_launch,
    /// @brief The player asked for the lowest latency when shown the offer.
    lowest_at_launch,
    /// @brief The offer went unanswered, so the lowest latency was chosen
    /// for the player.
    lowest_after_waiting,
  };

  /// @brief Describes a choice made automatically, without asking the player.
  ///
  /// @param name Datacenter chosen.
  /// @param measured Round trip measured during this launch; ignored unless
  ///     positive.
  /// @return The choice.
  static auto automatic(DatacenterName name, std::optional<RoundTrip> measured)
      -> DatacenterChoice {
    return DatacenterChoice{std::move(name), measured, How::automatic,
                            std::nullopt};
  }

  /// @brief Describes a choice forced by the player's stored preference.
  ///
  /// @param name Datacenter the player prefers.
  /// @param known Earlier measurements. When they include `name`, its round
  ///     trip and time of measurement are carried into the choice.
  /// @return The choice.
  static auto preferred(DatacenterName name, const KnownDatacenters& known)
      -> DatacenterChoice {
    const auto* last = known.find(name);
    return DatacenterChoice{
        std::move(name),
        last ? std::optional{last->round_trip()} : std::nullopt, How::preferred,
        last ? std::optional{last->measured_at()} : std::nullopt};
  }

  /// @brief Describes a datacenter the player picked from the offer.
  ///
  /// @param name Datacenter picked.
  /// @param measured Round trip measured during this launch; ignored unless
  ///     positive.
  /// @return The choice.
  static auto picked_at_launch(DatacenterName name,
                               std::optional<RoundTrip> measured)
      -> DatacenterChoice {
    return DatacenterChoice{std::move(name), measured, How::picked_at_launch,
                            std::nullopt};
  }

  /// @brief Describes the lowest-latency datacenter, chosen at the player's
  /// request.
  ///
  /// @param name Datacenter chosen.
  /// @param measured Round trip measured during this launch; ignored unless
  ///     positive.
  /// @return The choice.
  static auto lowest_at_launch(DatacenterName name,
                               std::optional<RoundTrip> measured)
      -> DatacenterChoice {
    return DatacenterChoice{std::move(name), measured, How::lowest_at_launch,
                            std::nullopt};
  }

  /// @brief Describes the lowest-latency datacenter, chosen after the offer
  /// timed out.
  ///
  /// @param name Datacenter chosen.
  /// @param measured Round trip measured during this launch; ignored unless
  ///     positive.
  /// @return The choice.
  static auto lowest_after_waiting(DatacenterName name,
                                   std::optional<RoundTrip> measured)
      -> DatacenterChoice {
    return DatacenterChoice{std::move(name), measured,
                            How::lowest_after_waiting, std::nullopt};
  }

  /// @brief Returns the datacenter the launch is using.
  ///
  /// @return The datacenter the launch is using.
  auto name() const noexcept -> const DatacenterName& { return name_; }
  /// @brief Returns the round trip to the chosen datacenter, when one is
  /// known.
  ///
  /// For DatacenterChoice::How::preferred this is the last stored
  /// measurement, not one taken during this launch;
  /// DatacenterChoice::last_measured_at says how old it is.
  ///
  /// @return The round trip to the chosen datacenter, when one is known.
  auto round_trip() const noexcept -> std::optional<RoundTrip> {
    return round_trip_;
  }
  /// @brief Returns how the datacenter was decided.
  ///
  /// @return How the datacenter was decided.
  auto how() const noexcept -> How { return how_; }
  /// @brief Returns when the stored round trip was measured.
  ///
  /// @return The time for a DatacenterChoice::How::preferred choice with a
  ///     stored measurement; `std::nullopt` otherwise.
  auto last_measured_at() const noexcept -> std::optional<Time> {
    return last_measured_at_;
  }
  /// @brief Reports whether the player named this datacenter instead of
  /// leaving it to latency.
  ///
  /// @return `true` for DatacenterChoice::How::preferred and
  ///     DatacenterChoice::How::picked_at_launch.
  auto was_forced() const noexcept -> bool {
    return how_ == How::preferred || how_ == How::picked_at_launch;
  }
  /// @brief Reports whether the decision came out of an offer shown during
  /// the launch.
  ///
  /// @return `true` for DatacenterChoice::How::picked_at_launch,
  ///     DatacenterChoice::How::lowest_at_launch and
  ///     DatacenterChoice::How::lowest_after_waiting.
  auto was_made_at_launch() const noexcept -> bool {
    return how_ != How::automatic && how_ != How::preferred;
  }

  /// @brief Compares name, round trip, way of deciding and time of
  /// measurement.
  friend auto operator==(const DatacenterChoice&, const DatacenterChoice&)
      -> bool = default;

 private:
  DatacenterChoice(DatacenterName name, std::optional<RoundTrip> round_trip,
                   How how, std::optional<Time> last_measured_at) noexcept
      : name_(std::move(name)),
        round_trip_(round_trip && round_trip->count() > 0
                        ? round_trip
                        : std::optional<RoundTrip>{}),
        how_(how),
        last_measured_at_(last_measured_at) {}

  DatacenterName name_;
  std::optional<RoundTrip> round_trip_;
  How how_;
  std::optional<Time> last_measured_at_;
};

/// @brief Says how the datacenter of a cloud launch should be decided.
///
/// There are three forms. The default leaves the choice to the launch, which
/// picks automatically. DatacenterPreference::named forces one datacenter.
/// DatacenterPreference::ask_at_launch pauses the launch after measuring and
/// offers the list to the player.
///
/// @see Play::start_on_cloud
class DatacenterPreference {
 public:
  /// @brief Time a launch waits for the player's choice unless told
  /// otherwise.
  static constexpr std::chrono::milliseconds default_wait =
      DatacenterOffer::default_wait;
  /// @brief Longest wait DatacenterPreference::ask_at_launch accepts.
  static constexpr std::chrono::milliseconds maximum_wait{60000};

  /// @brief Leaves the choice of datacenter to the launch.
  DatacenterPreference() noexcept = default;

  /// @brief Forces the launch onto one datacenter.
  ///
  /// If that datacenter is not available the launch is refused with
  /// LaunchRefused::Kind::datacenter_unavailable.
  ///
  /// @param name Datacenter to use.
  /// @return The preference.
  static auto named(DatacenterName name) -> DatacenterPreference {
    DatacenterPreference preference;
    preference.name_ = std::move(name);
    return preference;
  }

  /// @brief Asks the player to pick a datacenter during the launch.
  ///
  /// @param wait Time to wait for the choice. Zero means
  ///     DatacenterPreference::default_wait.
  /// @return The preference, or `std::nullopt` when `wait` is negative or
  ///     longer than DatacenterPreference::maximum_wait.
  /// @see DatacenterOffer
  [[nodiscard]] static auto ask_at_launch(
      std::chrono::milliseconds wait = default_wait)
      -> std::optional<DatacenterPreference> {
    if (wait.count() < 0 || wait > maximum_wait) return std::nullopt;
    DatacenterPreference preference;
    preference.asks_at_launch_ = true;
    if (wait.count() != 0) preference.wait_ = wait;
    return preference;
  }

  /// @brief Returns the forced datacenter, or `std::nullopt` when none is
  /// forced.
  ///
  /// @return The forced datacenter, or `std::nullopt` when none is forced.
  auto name() const noexcept -> const std::optional<DatacenterName>& {
    return name_;
  }
  /// @brief Reports whether the datacenter is chosen automatically, without
  /// asking the player.
  ///
  /// @return `true` when the datacenter is chosen automatically, without
  ///     asking the player.
  auto is_automatic() const noexcept -> bool {
    return !name_ && !asks_at_launch_;
  }
  /// @brief Reports whether the player is offered the list during the launch.
  ///
  /// @return `true` when the player is offered the list during the launch.
  auto asks_at_launch() const noexcept -> bool { return asks_at_launch_; }
  /// @brief Returns how long the launch waits for the player's choice.
  ///
  /// Only meaningful when DatacenterPreference::asks_at_launch is set.
  ///
  /// @return How long the launch waits for the player's choice.
  auto wait() const noexcept -> std::chrono::milliseconds { return wait_; }

  /// @brief Compares the forced name, the ask-at-launch flag and the wait.
  friend auto operator==(const DatacenterPreference&,
                         const DatacenterPreference&) -> bool = default;

 private:
  std::optional<DatacenterName> name_;
  bool asks_at_launch_{};
  std::chrono::milliseconds wait_{default_wait};
};

/// @brief Persists the datacenter measurements of each cloud service.
///
/// The frontend implements this interface and passes it to Play. The library
/// reads a service's measurements the first time they are needed and keeps them
/// in memory afterwards, so DatacenterStore::load is called at most once per
/// service for the lifetime of a Play. Calls are made one at a time, but may
/// come from a session's own thread as well as from the thread that calls Play.
///
/// @see Play::known_datacenters
/// @see Play::forget_datacenters
class DatacenterStore {
 public:
  /// @brief Names why the store could not serve a request.
  enum class Error : std::uint8_t {
    /// @brief The storage could not be read or written.
    unavailable,
  };

  /// @brief Allows destruction through the interface.
  virtual ~DatacenterStore() = default;

  /// @brief Reads the stored measurements of one cloud service.
  ///
  /// @param service Service whose measurements are wanted. The two services
  ///     are stored separately.
  /// @return The stored measurements in any order; an empty vector when
  ///     nothing is stored. On failure the library carries on as if nothing
  ///     were stored.
  /// @retval Error::unavailable The storage could not be read.
  [[nodiscard]] virtual auto load(catalog::ServiceGeneration service)
      -> Expected<std::vector<DatacenterMeasurement>, Error> = 0;
  /// @brief Replaces the stored measurements of one cloud service.
  ///
  /// Called after a cloud launch learns something new, and with an empty span
  /// when the player clears the list.
  ///
  /// @param service Service the measurements belong to.
  /// @param measurements Complete new set, fastest first. It replaces
  ///     whatever was stored. The span is only valid during the call.
  /// @return Nothing on success.
  /// @retval Error::unavailable The storage could not be written.
  [[nodiscard]] virtual auto save(
      catalog::ServiceGeneration service,
      std::span<const DatacenterMeasurement> measurements)
      -> Expected<void, Error> = 0;
};

}  // namespace libchiaking::play
