/// @file
/// @brief The counters and link measurements a running stream reports for
/// diagnostics.
///
/// Every section has a `present` flag: a value is meaningful only when the
/// Takion version and the console supplied it. Counters count from the start
/// of the active session.
#pragma once
#include "src/core/net/protocol/remote_play/stream/controller.hpp"
#include "src/core/net/protocol/takion/reliable_retry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace libchiaking::net::protocol::remote_play {
/// @brief The number of DSCP values a traffic class can carry.
inline constexpr std::size_t stream_dscp_count = 64;
/// @brief The number of ECN values a traffic class can carry.
inline constexpr std::size_t stream_ecn_count = 4;

/// @brief The round trip Takion measures.
struct StreamRoundTripDiagnostics {
  /// @brief Whether a round trip was measured.
  bool present{};
  /// @brief The smoothed round trip, in microseconds.
  std::uint64_t smoothed_us{};
  /// @brief The round trip's variation, in microseconds.
  std::uint64_t variation_us{};
  /// @brief Percentiles of the latest samples.
  takion::RoundTripSpread spread{};
};

/// @brief The receive statistics of Takion's extended messages.
struct StreamExtensionDiagnostics {
  /// @brief Whether the statistics were reported.
  bool present{};
  /// @brief The packet arrival jitter, in microseconds.
  std::uint64_t jitter_us{};
  /// @brief Packets received.
  std::uint64_t received{};
  /// @brief Packets lost.
  std::uint64_t lost{};
  /// @brief The share lost, 0 to 1.
  double loss_ratio{};
};

/// @brief Video packets whose unit counts do not describe their frame, and
/// the first of them.
struct StreamVideoCountDiagnostics {
  /// @brief How many such packets arrived.
  std::uint64_t packets{};
  /// @brief Whether the first one was recorded.
  bool present{};
  /// @brief Its frame index.
  std::uint64_t frame{};
  /// @brief Its unit index.
  std::uint64_t unit{};
  /// @brief Its unit total.
  std::uint64_t total{};
  /// @brief Its parity unit count.
  std::uint64_t fec{};
  /// @brief Its block coverage word.
  std::uint64_t video_word{};
  /// @brief Its payload size, in bytes.
  std::uint64_t bytes{};
};

/// @brief The video frame metrics the client reports to the console (Takion
/// video_frame_metrics).
struct StreamVideoMetricsDiagnostics {
  /// @brief Whether the metrics are tracked.
  bool present{};
  /// @brief Whether any report was made.
  bool reports_present{};
  /// @brief The video throughput, in bits per second.
  std::uint64_t throughput_bps{};
  /// @brief The share of video lost, 0 to 1.
  double loss_ratio{};
  /// @brief The interval the console sent frames at, in microseconds.
  std::int32_t send_interval_us{};
  /// @brief The interval frames arrived at, in microseconds.
  std::int32_t receive_interval_us{};
  /// @brief How long the last frame took from first to last packet, in
  /// microseconds.
  std::int32_t transfer_time_us{};
  /// @brief How long the last frame was in transit, in microseconds.
  std::int32_t transmission_time_us{};
  /// @brief The last frame's size, in bytes.
  std::uint32_t frame_bytes{};
  /// @brief The last frame's number.
  std::uint32_t frame_number{};
  /// @brief Whether the last frame's values were estimated.
  bool frame_estimated{};
  /// @brief How many reports were sent.
  std::uint32_t reports_sent{};
};

/// @brief The traffic class of received packets (Takion received_tos_metrics).
struct StreamTosDiagnostics {
  /// @brief Whether traffic classes are tracked.
  bool present{};
  /// @brief Whether the last packet's class is known.
  bool last_present{};
  /// @brief The last packet's DSCP, 0 to 63.
  std::uint8_t last_dscp{};
  /// @brief The last packet's ECN, 0 to 3.
  std::uint8_t last_ecn{};
  /// @brief Packets whose class was read.
  std::uint64_t packets{};
  /// @brief Reports sent to the console.
  std::uint64_t reports{};
  /// @brief Packets per DSCP value.
  std::array<std::uint64_t, stream_dscp_count> dscp_counts{};
  /// @brief Packets per ECN value.
  std::array<std::uint64_t, stream_ecn_count> ecn_counts{};
};

/// @brief What the stream learned about cloud audio channels and profiles;
/// filled for PS cloud audio assembly only.
struct StreamAudioDiagnostics {
  /// @brief Whether anything was recorded.
  bool present{};
  /// @brief The channel count of the stream info's top-level audio header.
  std::uint32_t top_channels{};
  /// @brief Whether the stream info described the main (type 0) audio channel.
  bool main_present{};
  /// @brief The main channel's channel count.
  std::uint32_t main_channels{};
  /// @brief One bit per channel type (below 32) the stream info describes.
  std::uint32_t channel_type_bits{};
  /// @brief One bit per AV lane (below 32) that carried audio other than
  /// voice and haptics.
  std::uint32_t lane_bits{};
  /// @brief One bit per profile number (below 32) the main channel offers.
  std::uint32_t main_profile_bits{};
  /// @brief Whether the first main-lane audio packet was recorded.
  bool packet_present{};
  /// @brief That packet's AV lane.
  std::uint32_t packet_lane{};
  /// @brief That packet's audio profile.
  std::uint32_t packet_profile{};
  /// @brief That packet's audio mode.
  std::uint32_t packet_mode{};
  /// @brief That packet's frame index.
  std::uint64_t packet_frame{};
  /// @brief That packet's unit index.
  std::uint64_t packet_unit{};
  /// @brief That packet's unit total.
  std::uint64_t packet_total{};
  /// @brief That packet's FEC unit count.
  std::uint64_t packet_fec{};
  /// @brief That packet's payload size, in bytes.
  std::uint64_t packet_bytes{};
  /// @brief The channel count of the audio profile now in use.
  std::uint32_t current_channels{};
  /// @brief Whether the console switched profile.
  bool selection_present{};
  /// @brief The lane of the last switch.
  std::uint32_t selection_lane{};
  /// @brief The profile of the last switch.
  std::uint32_t selection_profile{};
};

/// @brief A snapshot of a running stream's counters and link measurements.
///

/// @brief A cloud server's own measurements of the stream, from its latest
/// METRICSOVERLAY.
struct StreamServiceMetricsDiagnostics {
  /// @brief Whether a measurement has arrived.
  bool present{};
  /// @brief How many have arrived.
  std::uint64_t reports{};
  /// @brief Downstream bandwidth available, in kilobits per second; 0 when
  /// not given.
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
  /// @brief Round trip to the gateway, in microseconds.
  std::uint32_t gateway_round_trip_us{};
  /// @brief Audio channels.
  std::uint32_t audio_channels{};
  /// @brief Video width in pixels.
  std::uint32_t width{};
  /// @brief Video height in pixels.
  std::uint32_t height{};
  /// @brief Corrupt frames the server counted.
  std::uint32_t corrupt_frames{};
  /// @brief The dynamic range name, NUL-terminated and cut to fit.
  std::array<char, 16> dynamic_range{};
  /// @brief The video codec name, NUL-terminated and cut to fit.
  std::array<char, 16> video_codec{};
};

/// @brief The controller input sent to the console.
///
/// Plain data.
struct StreamInputDiagnostics {
  /// @brief Feedback state packets the session accepted.
  std::uint64_t state_posts{};
  /// @brief Feedback state packets the session refused.
  std::uint64_t state_refused{};
  /// @brief Feedback history packets the session accepted.
  std::uint64_t history_posts{};
  /// @brief Feedback history packets the session refused.
  std::uint64_t history_refused{};
  /// @brief The sequence the next feedback state packet carries.
  std::uint16_t state_sequence{};
  /// @brief The sequence number of the newest history event sent.
  std::uint16_t history_sequence{};
  /// @brief How many times the history sequence went from 65535 to 0.
  std::uint64_t history_wraps{};
  /// @brief Samples ignored because their seat was not active.
  std::uint64_t ignored_samples{};
  /// @brief History events queued: touches, buttons, triggers and presence.
  std::uint64_t history_events{};
  /// @brief History windows queued for posting.
  std::uint64_t windows_queued{};
  /// @brief Queued windows dropped because the outbox was full.
  std::uint64_t windows_shed{};
  /// @brief Windows still waiting to be posted.
  std::uint64_t windows_waiting{};
  /// @brief Resends of the newest window.
  std::uint64_t resends{};
  /// @brief The active seats, one bit per seat.
  std::uint8_t active_seats{};
  /// @brief Each seat's buttons as last sent in history.
  std::array<std::uint32_t, Pad::count> held_buttons{};
  /// @brief Each seat's buttons held back until released.
  std::array<std::uint32_t, Pad::count> latched_buttons{};
};

/// Plain data, copied out of the session on request.
struct StreamDiagnostics {
  /// @brief Whether the snapshot comes from a running stream.
  bool available{};
  /// @brief When the stream last showed network trouble, in monotonic
  /// microseconds: a second or more with nothing received from the console,
  /// or, over PSN, the start of a run of control channel errors; empty when
  /// it never did.
  std::optional<std::uint64_t> network_trouble_us{};
  /// @brief Video units received.
  std::uint64_t video_units{};
  /// @brief Video frames completed from FEC parity units.
  std::uint64_t video_fec_repaired{};
  /// @brief Packets Takion rejected, not counting transient send errors.
  std::uint64_t rejected_packets{};
  /// @brief Of those, packets that failed authentication or decryption.
  std::uint64_t crypto_rejections{};
  /// @brief Video frames assembled.
  std::uint64_t assembled_video{};
  /// @brief Frames refused because a frame they depend on was lost.
  std::uint64_t dependency_rejections{};
  /// @brief Frames whose slices were repointed at an older reference
  /// because the one they named was lost.
  std::uint64_t repaired_frames{};
  /// @brief Video packets discarded on request to simulate a lost frame.
  std::uint64_t dropped_on_request{};
  /// @brief Units the assemblers refused.
  std::uint64_t assembly_rejections{};
  /// @brief Video units refused because their frame had already passed.
  std::uint64_t video_stale{}, video_metadata{}, video_duplicate{},
      video_invalid{};
  /// @brief Parity units that arrived after their frame was already complete
  /// or given up; not counted as rejections.
  std::uint64_t video_late_parity{};
  /// @brief Audio units received per AV lane tag, haptics excluded; tags past
  /// the end are counted in the last entry.
  std::array<std::uint64_t, 16> audio_lane_units{};
  /// @brief Media or feedback the caller's queues could not take.
  std::uint64_t dropped_posts{};
  /// @brief Video units the reorder queue gave up waiting for.
  std::uint64_t reorder_skipped{};
  /// @brief Sends dropped on transient errors.
  std::uint64_t send_dropped{};
  /// @brief Video units the reorder queue dropped.
  std::uint64_t reorder_dropped{};
  /// @brief Lost frame ranges reported to the console.
  std::uint64_t lost_ranges{};
  /// @brief Of those, ranges whose frames FEC could not repair.
  std::uint64_t unrepaired_ranges{};
  /// @brief The receive buffer the operating system gave the stream's
  /// socket, in bytes; 0 while unknown.
  std::uint64_t socket_receive_buffer{};
  /// @brief The longest wait between two reads of the stream's socket since
  /// video began, in microseconds.
  std::uint64_t longest_read_gap_us{};
  /// @brief Waits between two reads of the stream's socket longer than one
  /// frame at 60 fps since video began.
  std::uint64_t late_reads{};
  /// @brief The negotiated Takion feature bits.
  std::uint32_t takion_features{};
  /// @brief The round trip.
  StreamRoundTripDiagnostics round_trip{};
  /// @brief The extended receive statistics.
  StreamExtensionDiagnostics extension{};
  /// @brief The video frame metrics.
  StreamVideoMetricsDiagnostics video_metrics{};
  /// @brief The received traffic classes.
  StreamTosDiagnostics tos{};
  /// @brief The cloud audio record.
  StreamAudioDiagnostics audio{};
  /// @brief The video packets with unusable unit counts.
  StreamVideoCountDiagnostics video_counts{};
  /// @brief The cloud server's own measurements.
  StreamServiceMetricsDiagnostics service{};
  /// @brief The controller input sent.
  StreamInputDiagnostics input{};
  /// @var StreamDiagnostics::video_metadata
  /// @brief Video units refused because their metadata disagreed with their
  /// frame's.
  /// @var StreamDiagnostics::video_duplicate
  /// @brief Video units refused as repeats.
  /// @var StreamDiagnostics::video_invalid
  /// @brief Video units refused for any other reason.
};
}  // namespace libchiaking::net::protocol::remote_play
