/// @file
/// @brief Declares ReceivedTosMetrics, which counts the IP traffic class (DSCP
/// and ECN) of the packets a session receives and decides when to report them
/// to the console, for versions with the received_tos_metrics feature.
///
/// The console's STREAMINFO sets the reporting rule: report every
/// `tos_report_interval_msec` (1000 by default), or sooner once
/// `tos_report_num_pkts_recv` packets have arrived ECN-marked.
#pragma once

#include "src/core/net/protocol/takion/ext_message_codec.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief Report interval used until STREAMINFO sets one, in milliseconds.
inline constexpr std::uint32_t received_tos_default_interval_ms = 1000;
/// @brief ECN-marked packets that trigger an early report until STREAMINFO
/// sets a threshold; 0 disables early reports.
inline constexpr std::uint32_t received_tos_default_packet_threshold = 0;
/// @brief Packet type nibble excluded from counting (15, the reserved
/// invalid type).
inline constexpr std::uint8_t received_tos_excluded_kind = 15;
/// @brief Distinct DSCP values, 0 to 63.
inline constexpr std::size_t received_tos_dscp_count = 64;
/// @brief Distinct ECN values: not-ECT, ECT(1), ECT(0), CE.
inline constexpr std::size_t received_tos_ecn_count = 4;

/// @brief The traffic-class counters of one session and its reporting rule.
///
/// Counting starts with start(), once crypto is installed and the transport
/// can read traffic classes. Owned by the client's inbound path and used under
/// the client's lock.
class ReceivedTosMetrics {
 public:
  /// @brief Packs per-DSCP counts for a report.
  ///
  /// @param counts Count per DSCP value.
  /// @return One word per non-zero count, ascending: the DSCP value in the top
  /// 8
  ///     bits and the count's low 24 bits below.
  static auto pack_dscp_counts(
      const std::array<std::uint32_t, received_tos_dscp_count>& counts)
      -> std::vector<std::uint32_t>;

  /// @brief Begins counting; the first interval starts now.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  void start(std::uint64_t now_us) noexcept;

  /// @brief Applies the console's reporting rule; absent values keep the
  /// current ones.
  ///
  /// @param packet_threshold ECN-marked packets that trigger a report.
  /// @param interval_ms Report interval, in milliseconds.
  void configure(std::optional<std::uint32_t> packet_threshold,
                 std::optional<std::uint32_t> interval_ms) noexcept;

  /// @brief Counts one received packet and says whether a report is due.
  ///
  /// @param first_byte The packet's first byte; type nibble 15 is not counted.
  /// @param traffic_class The IP traffic class byte it arrived with: DSCP in
  ///     the top 6 bits, ECN in the low 2.
  /// @param now_us When it arrived, in monotonic microseconds.
  /// @return The report to send, after which the interval's counters restart;
  ///     nothing when none is due or counting has not started.
  [[nodiscard]] auto account(std::uint8_t first_byte,
                             std::uint8_t traffic_class, std::uint64_t now_us)
      -> std::optional<ReceivedTosMetricsReport>;

  /// @brief Tells whether counting has started.
  ///
  /// @return Whether it has.
  bool started() const noexcept { return started_; }
  /// @brief Reads the report interval.
  ///
  /// @return Milliseconds.
  std::uint32_t interval_ms() const noexcept { return interval_ms_; }
  /// @brief Reads the early-report threshold.
  ///
  /// @return ECN-marked packets; 0 when disabled.
  std::uint32_t packet_threshold() const noexcept { return packet_threshold_; }
  /// @brief Reads the ECN-marked packets counted this interval.
  ///
  /// @return The count.
  std::uint32_t trigger_count() const noexcept { return trigger_count_; }
  /// @brief Reads this interval's per-DSCP counts.
  ///
  /// @return Count per DSCP value.
  const std::array<std::uint32_t, received_tos_dscp_count>& dscp_counts()
      const noexcept {
    return dscp_counts_;
  }
  /// @brief Reads the session's per-DSCP counts.
  ///
  /// @return Count per DSCP value.
  const std::array<std::uint64_t, received_tos_dscp_count>& total_dscp_counts()
      const noexcept {
    return total_dscp_counts_;
  }
  /// @brief Reads the session's per-ECN counts.
  ///
  /// @return Count per ECN value.
  const std::array<std::uint64_t, received_tos_ecn_count>& total_ecn_counts()
      const noexcept {
    return total_ecn_counts_;
  }
  /// @brief Reads the packets counted in the session.
  ///
  /// @return The count.
  std::uint64_t total_packets() const noexcept { return total_packets_; }
  /// @brief Reads the traffic class of the latest packet counted.
  ///
  /// @return The byte; nothing before the first.
  std::optional<std::uint8_t> last_traffic_class() const noexcept {
    return last_traffic_class_;
  }
  /// @brief Reads how many reports were produced.
  ///
  /// @return The count.
  std::uint64_t reports() const noexcept { return reports_; }

 private:
  /// @brief This interval's per-DSCP counts.
  std::array<std::uint32_t, received_tos_dscp_count> dscp_counts_{};
  /// @brief This interval's per-ECN counts.
  std::array<std::uint32_t, received_tos_ecn_count> ecn_counts_{};
  /// @brief Report interval, in milliseconds.
  std::uint32_t interval_ms_{received_tos_default_interval_ms};
  /// @brief ECN-marked packets that trigger an early report; 0 disables.
  std::uint32_t packet_threshold_{received_tos_default_packet_threshold};
  /// @brief ECN-marked packets counted this interval.
  std::uint32_t trigger_count_{};
  /// @brief When this interval began, in monotonic microseconds.
  std::uint64_t interval_start_us_{};
  /// @brief Whether counting has started.
  bool started_{};
  /// @brief The session's per-DSCP counts.
  std::array<std::uint64_t, received_tos_dscp_count> total_dscp_counts_{};
  /// @brief The session's per-ECN counts.
  std::array<std::uint64_t, received_tos_ecn_count> total_ecn_counts_{};
  /// @brief Packets counted in the session.
  std::uint64_t total_packets_{};
  /// @brief Traffic class of the latest packet counted.
  std::optional<std::uint8_t> last_traffic_class_{};
  /// @brief Reports produced.
  std::uint64_t reports_{};
};

}  // namespace libchiaking::net::protocol::takion
