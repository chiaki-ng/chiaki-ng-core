/// @file
/// @brief Declares the counters a session reports about its video, link and
/// inputs.
#pragma once

#include "libchiaking/detail/media/video_statistics.hpp"
#include "libchiaking/detail/play/microphone.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace libchiaking::play {

/// @brief Carries the diagnostics of the stream connection itself.
///
/// These figures come from the network side of a session and are meant for a
/// statistics overlay or a diagnostic report. They are zero until
/// LinkStatistics::available is set. Each nested group has its own `present`
/// flag, because not every kind of session reports every group.
struct LinkStatistics {
  /// @brief Number of distinct DSCP values counted in
  /// ServiceMarks::dscp_counts.
  static constexpr std::size_t dscp_count = 64;
  /// @brief Number of distinct ECN values counted in
  /// ServiceMarks::ecn_counts.
  static constexpr std::size_t ecn_count = 4;

  /// @brief Round-trip time of the stream connection.
  struct RoundTrip {
    /// @brief Whether a round trip has been measured.
    bool present{};
    /// @brief Smoothed round-trip time, in microseconds.
    std::uint64_t smoothed_us{};
    /// @brief Variation of the round-trip time, in microseconds.
    std::uint64_t variation_us{};
    /// @brief Round-trip samples taken during the stream.
    std::uint64_t samples{};
    /// @brief Median of the latest samples, in microseconds.
    std::uint64_t median_us{};
    /// @brief 90th percentile of the latest samples, in microseconds.
    std::uint64_t p90_us{};
    /// @brief 99th percentile of the latest samples, in microseconds.
    std::uint64_t p99_us{};
    /// @brief Largest of the latest samples, in microseconds.
    std::uint64_t max_us{};
    /// @brief Largest sample of the ten seconds before the statistics were
    /// read, in microseconds; 0 when none arrived in that time.
    std::uint64_t recent_max_us{};
  };

  /// @brief Reception quality of the stream.
  struct Reception {
    /// @brief Whether reception figures are available.
    bool present{};
    /// @brief Arrival jitter, in microseconds.
    std::uint64_t jitter_us{};
    /// @brief Packets received.
    std::uint64_t received{};
    /// @brief Packets lost.
    std::uint64_t lost{};
    /// @brief Share of packets lost, from 0.0 to 1.0.
    double loss_ratio{};
  };

  /// @brief Video delivery measurements reported by the link.
  struct VideoMetrics {
    /// @brief Whether video measurements are available.
    bool present{};
    /// @brief Whether measurement reports are present.
    bool reports_present{};
    /// @brief Measured video throughput, in bits per second.
    std::uint64_t throughput_bps{};
    /// @brief Share of video packets lost, from 0.0 to 1.0.
    double loss_ratio{};
    /// @brief Interval between sends, in microseconds.
    std::int32_t send_interval_us{};
    /// @brief Interval between receptions, in microseconds.
    std::int32_t receive_interval_us{};
    /// @brief Transfer time, in microseconds.
    std::int32_t transfer_time_us{};
    /// @brief Transmission time, in microseconds.
    std::int32_t transmission_time_us{};
    /// @brief Size of the frame the measurements refer to, in bytes.
    std::uint32_t frame_bytes{};
    /// @brief Number of the frame the measurements refer to.
    std::uint32_t frame_number{};
    /// @brief Whether the frame figures are estimates.
    bool frame_estimated{};
    /// @brief Measurement reports sent so far.
    std::uint32_t reports_sent{};
  };

  /// @brief Type-of-service marks (DSCP and ECN) observed on the stream's
  /// packets.
  struct ServiceMarks {
    /// @brief Whether packet marks are being observed.
    bool present{};
    /// @brief Whether `last_dscp` and `last_ecn` hold a value.
    bool last_present{};
    /// @brief DSCP value of the most recent packet.
    std::uint8_t last_dscp{};
    /// @brief ECN value of the most recent packet.
    std::uint8_t last_ecn{};
    /// @brief Packets whose marks were observed.
    std::uint64_t packets{};
    /// @brief Mark reports counted so far.
    std::uint64_t reports{};
    /// @brief Packets seen with each DSCP value, indexed by that value.
    std::array<std::uint64_t, dscp_count> dscp_counts{};
    /// @brief Packets seen with each ECN value, indexed by that value.
    std::array<std::uint64_t, ecn_count> ecn_counts{};
  };

  /// @brief A cloud server's own measurements of the stream.
  struct ServerMeasurements {
    /// @brief Whether the server has sent any.
    bool present{};
    /// @brief How many the server has sent.
    std::uint64_t reports{};
    /// @brief Downstream bandwidth available, in kilobits per second; 0
    /// when not given.
    std::uint32_t available_down_kbps{};
    /// @brief Downstream bandwidth used, in kilobits per second.
    std::uint32_t used_down_kbps{};
    /// @brief Upstream bandwidth used, in kilobits per second.
    std::uint32_t used_up_kbps{};
    /// @brief Latest round trip, in milliseconds.
    std::uint32_t round_trip_ms{};
    /// @brief Downstream loss, in percent.
    float loss_percent{};
    /// @brief Upstream loss, in percent.
    float upstream_loss_percent{};
    /// @brief Round trip to the server's gateway, in microseconds.
    std::uint32_t gateway_round_trip_us{};
    /// @brief Audio channels the server sends.
    std::uint32_t audio_channels{};
    /// @brief Video width in pixels.
    std::uint32_t width{};
    /// @brief Video height in pixels.
    std::uint32_t height{};
    /// @brief Corrupt frames the server counted.
    std::uint32_t corrupt_frames{};
    /// @brief The dynamic range name the server gives, NUL-terminated.
    std::array<char, 16> dynamic_range{};
    /// @brief The video codec name the server gives, NUL-terminated.
    std::array<char, 16> video_codec{};
  };

  /// @brief Whether the link has reported diagnostics yet.
  bool available{};
  /// @brief Video units the link received.
  std::uint64_t video_units{};
  /// @brief Packets the link rejected.
  std::uint64_t rejected_packets{};
  /// @brief Packets rejected by the cryptographic checks.
  std::uint64_t crypto_rejections{};
  /// @brief Video frames the link assembled.
  std::uint64_t assembled_video{};
  /// @brief Video rejected for a missing dependency.
  std::uint64_t dependency_rejections{};
  /// @brief Video rejected during assembly.
  std::uint64_t assembly_rejections{};
  /// @brief Video frames completed from FEC parity units.
  std::uint64_t fec_repaired_frames{};
  /// @brief Video units counted as stale.
  std::uint64_t video_stale{};
  /// @brief Video units counted as metadata.
  std::uint64_t video_metadata{};
  /// @brief Video units counted as duplicates.
  std::uint64_t video_duplicate{};
  /// @brief Video units counted as invalid.
  std::uint64_t video_invalid{};
  /// @brief Posted items the link dropped.
  std::uint64_t dropped_posts{};
  /// @brief Items the reorder queue skipped.
  std::uint64_t reorder_skipped{};
  /// @brief Sends the link dropped.
  std::uint64_t send_dropped{};
  /// @brief Items the reorder queue dropped.
  std::uint64_t reorder_dropped{};
  /// @brief Bit set of the stream protocol features in use.
  std::uint32_t protocol_features{};
  /// @brief Round-trip time of the stream connection.
  RoundTrip round_trip{};
  /// @brief Reception quality of the stream.
  Reception reception{};
  /// @brief Video delivery measurements reported by the link.
  VideoMetrics video_metrics{};
  /// @brief Type-of-service marks (DSCP and ECN) observed on the stream's
  /// packets.
  ServiceMarks service_marks{};
  /// @brief The cloud server's own measurements.
  ServerMeasurements server{};
};

/// @brief Gathers everything a session can report about how it is running.
///
/// Session::statistics returns a snapshot, so a frontend can poll it to refresh
/// a statistics display.
struct Statistics {
  /// @brief Diagnostics of the stream connection.
  LinkStatistics link{};
  /// @brief Stream protocol version in use, once it is known.
  std::optional<std::uint32_t> protocol_version{};
  /// @brief Counters for the video received.
  media::VideoStatistics video{};
  /// @brief Counters for the microphone audio sent.
  Microphone::Statistics microphone{};
  /// @brief Controller input samples queued and not yet sent, over all seats.
  std::size_t controller_pending{};
  /// @brief Errors the session's loop met while servicing the connection.
  std::uint64_t loop_errors{};
  /// @brief Audio output failures the frontend reported.
  std::uint64_t audio_failures{};
};

}  // namespace libchiaking::play
