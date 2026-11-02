/// @file
/// @brief Declares Datacenter, a GK datacenter as GK lists it, and
/// Ping, a datacenter as the client measured it and reports it back to GK
/// when one is selected.
#pragma once

#include "src/core/net/protocol/senkusha/types.hpp"
#include "src/core/psn/gk/datacenter_ping.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::gk {

/// @brief A datacenter GK offers the session. Plain data.
struct Datacenter {
  /// @brief The `dataCenter` name, which select and allocate name it by.
  std::string name;
  /// @brief The `publicIp` the client measures against.
  std::string public_ip;
  /// @brief The `port` to measure; 0 when GK named none.
  int port = 0;
  /// @brief The `maxBandwidth` GK stated, in kbit/s; 0 when absent or not
  /// a number.
  int max_bandwidth = 0;
};

/// @brief A datacenter's measurement: the published cloud::DatacenterPing
/// fields (name, round-trip times, MTUs, port, address, bandwidth, whether it
/// was measured) and what the client chose for it.
///
/// Round-trip times are kept true here; only the values reported to GK are
/// lowered by reported_rtt_offset (to at least reported_rtt_floor). A value;
/// safe to copy.
struct Ping : cloud::DatacenterPing {
  /// @brief The largest ping report sent to GK, in bytes.
  static constexpr std::size_t report_limit =
      cloud::DatacenterPing::report_limit;
  /// @brief The round-trip time recorded for a datacenter that could not be
  /// measured, in ms.
  static constexpr int unmeasured_rtt_ms = 999;
  /// @brief The round-trip time sent at allocation when none was measured, in
  /// ms.
  static constexpr int default_rtt_ms = 25;
  /// @brief The downstream MTU sent at allocation when none was measured, in
  /// bytes.
  static constexpr int default_mtu_in =
      static_cast<int>(net::protocol::senkusha::mtu_upper_bound);
  /// @brief The upstream MTU sent at allocation when none was measured, in
  /// bytes.
  static constexpr int default_mtu_out = 1254;
  /// @brief How much lower each measured round-trip time is reported than
  /// measured.
  static constexpr std::chrono::milliseconds reported_rtt_offset{20};
  /// @brief The lowest round-trip time reported.
  static constexpr std::chrono::milliseconds reported_rtt_floor{1};

  /// @brief Starts an unmeasured row for a listed datacenter.
  ///
  /// @param datacenter The datacenter.
  /// @return The row with its name, address, port and bandwidth.
  static auto from(const Datacenter& datacenter) -> Ping;
  /// @brief Wraps published measurements.
  ///
  /// @param values The measurements.
  /// @return The rows, with no port or bandwidth chosen.
  static auto from(const std::vector<cloud::DatacenterPing>& values)
      -> std::vector<Ping>;
  /// @brief Strips rows to their published measurements.
  ///
  /// @param rows The rows.
  /// @return The measurements, in order.
  static auto published(const std::vector<Ping>& rows)
      -> std::vector<cloud::DatacenterPing>;
  /// @brief Writes rows as the ping report GK's select request carries: a
  /// JSON array of `dataCenter`, `rtt`, `rtts`, `mtu_in`, `mtu_out`, `port`,
  /// `publicIp`, `maxBandwidth`, `measured`.
  ///
  /// @param rows The rows, as they should be reported.
  /// @param limit The largest report allowed, in bytes.
  /// @return The report; nothing when there are no rows, a name or address
  ///     holds a NUL, or the report is too large.
  static auto serialize(const std::vector<Ping>& rows,
                        std::size_t limit = report_limit)
      -> std::optional<std::string>;
  /// @brief Reads a ping report back, such as one saved from an earlier
  /// launch.
  ///
  /// @param report The report.
  /// @return The rows; empty when it does not decode.
  static auto parse(std::string_view report) -> std::vector<Ping>;
  /// @brief Lowers every row's round-trip times as GK is told them.
  ///
  /// @param rows The rows.
  /// @return The rows as reported().
  static auto reported(const std::vector<Ping>& rows) -> std::vector<Ping>;

  /// @brief Lowers this row's round-trip times as GK is told them.
  ///
  /// @return A copy with `rtt_ms` and each of `rtts_ms` lowered by
  ///     reported_rtt_offset, not below reported_rtt_floor; unchanged when
  ///     not measured or the round-trip time is not positive.
  auto reported() const -> Ping;
  /// @brief Compares every field.
  ///
  /// @return Whether all fields are equal.
  bool operator==(const Ping&) const = default;

  /// @brief The port chosen to stream from: the listed port, else the
  /// default; 0 until chosen.
  int selected_port = 0;
  /// @brief The bandwidth measured, in kbit/s; 0 when not measured.
  int bandwidth_kbps = 0;

 private:
  /// @brief Lowers one round-trip time as GK is told it.
  ///
  /// @param rtt_ms The time, in ms.
  /// @return It less reported_rtt_offset, at least reported_rtt_floor.
  static auto reported_rtt(int rtt_ms) noexcept -> int;
};

}  // namespace libchiaking::psn::gk
