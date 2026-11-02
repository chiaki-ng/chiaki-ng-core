/// @file
/// @brief Declares DatacenterPing, one cloud datacenter's measured round trip
/// and path, as reported to the cloud service and to the caller.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::cloud {

/// @brief A cloud streaming datacenter and what the client measured to it.
///
/// Cloud allocation asks the client to measure its datacenters and send the
/// results as a JSON report (`dataCenter`, `rtt`, `rtts`, `mtu_in`,
/// `mtu_out`, `port`, `publicIp`, `maxBandwidth`, `measured`). A value type;
/// copy freely.
struct DatacenterPing {
  /// @brief The largest report sent, in bytes of JSON.
  static constexpr std::size_t report_limit = 12U * 1024U;
  /// @brief The largest stored earlier report accepted, in bytes of JSON.
  static constexpr std::size_t prior_report_limit = 64U * 1024U;

  /// @brief Whether a stored report may be parsed: within prior_report_limit
  /// and free of NUL bytes.
  ///
  /// @param json The report.
  /// @return True when acceptable.
  static auto accepts_report(std::string_view json) noexcept -> bool;
  /// @brief Reads a stored report.
  ///
  /// @param json The report.
  /// @return The datacenters; empty when the report is unacceptable or
  ///     unreadable.
  static auto parse_report(std::string_view json)
      -> std::vector<DatacenterPing>;
  /// @brief Writes a report.
  ///
  /// @param pings The datacenters.
  /// @param limit The largest report allowed, in bytes.
  /// @return The JSON, or nothing when the list is empty, a name or address
  ///     holds a NUL, or the JSON would exceed `limit`.
  static auto report(const std::vector<DatacenterPing>& pings,
                     std::size_t limit = report_limit)
      -> std::optional<std::string>;

  /// @brief The datacenter's name, such as "eu-west".
  std::string name;
  /// @brief The round trip reported, in milliseconds.
  int rtt_ms{};
  /// @brief Each round trip measured, in milliseconds.
  std::vector<int> rtts_ms;
  /// @brief The largest inbound packet measured, in bytes.
  int mtu_in{};
  /// @brief The largest outbound packet measured, in bytes.
  int mtu_out{};
  /// @brief The datacenter's measurement port.
  int port{};
  /// @brief The client's public address as the datacenter saw it.
  std::string public_ip;
  /// @brief The bandwidth the datacenter states it can give, as it states it.
  int max_bandwidth{};
  /// @brief Whether the datacenter was actually measured.
  bool measured{};
  /// @brief Compares every field.
  ///
  /// @return True when equal.
  bool operator==(const DatacenterPing&) const = default;
};

}  // namespace libchiaking::cloud
