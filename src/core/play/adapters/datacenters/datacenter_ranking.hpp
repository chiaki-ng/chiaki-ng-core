/// @file
/// @brief Declares DatacenterRanking, which orders the cloud datacenters a
/// launch measured and picks the one to reserve a server in.
#pragma once

#include "src/core/psn/gk/measurement.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::play {

/// @brief Ranks the datacenters PS offered for one cloud launch and chooses
/// one, automatically, by the player's forced name or by the player's pick at
/// launch.
///
/// Built over the datacenter list of one allocation, optionally with a name
/// the player forced. A forced datacenter is not measured: it gets a
/// placeholder measurement of DatacenterRanking::forced_rtt_ms. An automatic
/// choice is the fastest measured datacenter, and only when its round trip is
/// at most DatacenterRanking::auto_max_rtt_ms. Immutable after construction;
/// the const members may be called from any thread.
class DatacenterRanking {
 public:
  /// @brief A datacenter as PS lists it: name, public address, port and
  /// maximum bandwidth.
  using Datacenter = psn::gk::Datacenter;
  /// @brief One datacenter's measured round trips and path MTUs.
  using Ping = psn::gk::Ping;

  /// @brief Measurements sorted by round trip, with the automatic winner.
  struct Ranking {
    /// @brief Every measurement, fastest first; the order among equal round
    /// trips is kept.
    std::vector<Ping> sorted;
    /// @brief The fastest measured datacenter with its port set, when it is
    /// within DatacenterRanking::auto_max_rtt_ms; empty otherwise.
    std::optional<Ping> fastest;
  };

  /// @brief What a launch may choose from, and what it would choose.
  struct Candidates {
    /// @brief The datacenter that would be reserved; empty when none qualifies.
    std::optional<Ping> selected;
    /// @brief The measurements reported to PS with the allocation.
    std::vector<Ping> ping_results;
    /// @brief The rows shown to the player: every offered datacenter with its
    /// current, prior or placeholder measurement.
    std::vector<Ping> picker_rows;
  };

  /// @brief The final choice of a launch.
  struct Chosen {
    /// @brief The datacenter reserved.
    Ping selected;
    /// @brief The measurements reported to PS with the allocation.
    std::vector<Ping> ping_results;
    /// @brief The rows shown to the player.
    std::vector<Ping> picker_rows;
  };

  /// @brief The highest round trip an automatic choice accepts, in
  /// milliseconds.
  static constexpr int auto_max_rtt_ms = 100;
  /// @brief The port used when PS's listing gives none.
  static constexpr int default_port = 2053;
  /// @brief The round trip given to a datacenter that was forced or picked
  /// without being measured, in milliseconds.
  static constexpr int forced_rtt_ms = 40;

  /// @brief Builds the ranking over one allocation's datacenters.
  ///
  /// @param datacenters The datacenters PS offered.
  /// @param forced The datacenter the player forced; empty or
  ///     DatacenterName::automatic to choose automatically.
  explicit DatacenterRanking(std::vector<Datacenter> datacenters,
                             std::string_view forced = {});

  /// @brief Sorts measurements by round trip and finds the automatic winner.
  ///
  /// @param pings The measurements.
  /// @return The sorted list, and the fastest when it was measured and within
  ///     DatacenterRanking::auto_max_rtt_ms.
  static auto rank(std::vector<Ping> pings) -> Ranking;

  /// @brief Reports whether the player forced a datacenter.
  ///
  /// @return `true` when a name other than DatacenterName::automatic was given.
  auto forced() const noexcept -> bool;
  /// @brief Builds the placeholder measurement of the forced datacenter.
  ///
  /// @return The placeholder; empty when nothing was forced or the forced name
  ///     is not among the offered datacenters.
  auto forced_ping() const -> std::optional<Ping>;
  /// @brief Builds the rows shown to the player.
  ///
  /// Each offered datacenter appears in order with its current measurement,
  /// else its prior one, else a placeholder with a round trip of 0; a forced
  /// datacenter prefers its prior measurement over the placeholder. Prior
  /// datacenters PS no longer offers are appended.
  ///
  /// @param current Measurements from this launch.
  /// @param prior Measurements remembered from earlier launches.
  /// @return The rows.
  auto picker_rows(const std::vector<Ping>& current,
                   const std::vector<Ping>& prior) const -> std::vector<Ping>;
  /// @brief Chooses a datacenter from this launch's measurements.
  ///
  /// @param completed Measurements from this launch.
  /// @param prior Measurements remembered from earlier launches.
  /// @return The candidates: for a forced datacenter its placeholder is
  ///     selected and is the only result; otherwise the automatic winner, which
  ///     may be empty. Empty only when the forced name is not offered.
  auto choose(std::vector<Ping> completed, const std::vector<Ping>& prior) const
      -> std::optional<Candidates>;
  /// @brief Lists the datacenter names a player may pick from.
  ///
  /// @return The offered names in PS's order, without empty names or
  ///     duplicates.
  auto offer() const -> std::vector<std::string>;
  /// @brief Applies the player's pick at launch to the candidates.
  ///
  /// @param name The picked name.
  /// @param choice The candidates of this launch.
  /// @return The candidates with only the picked datacenter selected and
  ///     reported, measured when it was, else as a placeholder; empty when the
  ///     name is automatic, empty or not offered and was not measured.
  auto pick(std::string_view name, Candidates choice) const
      -> std::optional<Candidates>;

 private:
  /// @brief Finds a row by datacenter name.
  ///
  /// @param rows The rows searched.
  /// @param name The name.
  /// @return The first row with the name, pointing into @p rows; null when
  /// none.
  static auto find_ping(const std::vector<Ping>& rows, std::string_view name)
      -> const Ping*;
  /// @brief Sets the port a server is reserved on.
  ///
  /// @param row The measurement.
  /// @return @p row with its selected port set to its own port, or
  ///     DatacenterRanking::default_port when it has none.
  static auto with_selected_port(Ping row) -> Ping;

  /// @brief Builds an unmeasured placeholder for an offered datacenter.
  ///
  /// @param name The datacenter name.
  /// @return A measurement with DatacenterRanking::forced_rtt_ms, the default
  ///     MTUs and the port set; empty when the name is not offered.
  auto placeholder_ping(std::string_view name) const -> std::optional<Ping>;

  /// @brief The datacenters PS offered for this allocation.
  std::vector<Datacenter> datacenters_;
  /// @brief The datacenter the player forced; empty for automatic.
  std::string forced_;
};

}  // namespace libchiaking::play
