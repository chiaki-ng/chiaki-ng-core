/// @file
/// @brief Declares the codec for Takion extended messages (packet type 0x0e,
/// for versions with the extended_messages feature, 18 on): round-trip pings
/// and pongs, and the video-frame and traffic-class metrics reports.
///
/// After the type byte and any extended header, an extended message has a
/// 12-byte header and then a protobuf `tkproto.ExtMessage`:
///
/// | Offset | Size | Field |
/// |-------:|-----:|-------|
/// | 0 | 4 | GMAC tag (zero when unprotected) |
/// | 4 | 4 | key position, big-endian |
/// | 8 | 1 | header version, 1 |
/// | 9 | 1 | outer type, 0 for protobuf |
/// | 10 | 2 | flags, big-endian; 0x1 when the protobuf is encrypted |
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief Bytes of the extended message header.
inline constexpr std::size_t ext_message_header_size = 12;
/// @brief Bytes of the header before its version byte: the tag and key
/// position.
inline constexpr std::size_t ext_message_header_reserved_size = 8;
/// @brief The only header version understood.
inline constexpr std::uint8_t ext_message_header_version = 1;
/// @brief The only outer type understood: a protobuf body.
inline constexpr std::uint8_t ext_message_outer_type_protobuf = 0;
/// @brief The header flag marking an encrypted body.
inline constexpr std::uint16_t ext_message_flag_encrypted = 0x1;
/// @brief Largest protobuf body accepted for sending, in bytes.
inline constexpr std::size_t ext_message_payload_limit = 1024;

/// @brief What the extended message header says.
struct ExtMessageHeader {
  /// @brief Whether the body is encrypted.
  bool encrypted{};
};

/// @brief A round-trip ping from the console, which the client answers at
/// once with a pong echoing the same fields.
struct RttPing {
  /// @brief The ping's slot in the console's measurement.
  std::optional<std::uint32_t> entry_index;
  /// @brief When the console sent it, in microseconds on its clock.
  std::optional<std::int64_t> orig_send_timestamp_us;
};

/// @brief A video frame metrics report (CCVIDEOFRAMEMETRICS), sent for
/// versions with the video_frame_metrics feature. Times are microseconds.
struct VideoFrameMetricsReport {
  /// @brief Report number, from 0.
  std::uint32_t seq_num{};
  /// @brief Sender time between the starts of the last two frames.
  std::int32_t send_int_us{};
  /// @brief Local time between the arrivals of the last two frames' first
  /// units.
  std::int32_t recv_int_us{};
  /// @brief Time from the frame's first unit arriving to it being complete,
  /// less gaps between slices.
  std::int32_t transfer_time_us{};
  /// @brief Bytes of the frame received, headers included; scaled up when
  /// estimated.
  std::uint32_t recv_size{};
  /// @brief The frame's number, unwrapped beyond 16 bits.
  std::uint32_t frame_no_wide{};
  /// @brief The 16-bit frame index whose timing these figures describe.
  std::uint32_t frame_no_at_tt_updated{};
  /// @brief Whether the timing was estimated from an incomplete frame.
  bool is_trans_time_estimated{};
  /// @brief Whether the last frame directly followed the one before.
  bool is_consecutive_frame_no{};
  /// @brief Sender time across the frame's units, less gaps between slices.
  std::int32_t transmission_time_us{};
  /// @brief Video throughput over the last 200 ms window, in bits per second.
  std::uint32_t throughput_bps{};
  /// @brief Video packet loss over the last window, a fraction.
  double loss_ratio{};
  /// @brief Time from the frame's first arrival to the report.
  std::uint32_t proc_time_us{};

  /// @brief Compares every field.
  ///
  /// @return Whether both reports are the same.
  bool operator==(const VideoFrameMetricsReport&) const = default;
};

/// @brief A received traffic-class report (RECEIVEDTOSMETRICS).
struct ReceivedTosMetricsReport {
  /// @brief Milliseconds the report covers.
  std::uint32_t time_offset_msec{};
  /// @brief Packed per-DSCP counts: DSCP << 24 | count (24 bits), non-zero
  /// only.
  std::vector<std::uint32_t> dscp_counts;
  /// @brief Packets marked ECT(1).
  std::uint32_t ecn_ect1_count{};
  /// @brief Packets marked ECT(0).
  std::uint32_t ecn_ect0_count{};
  /// @brief Packets marked CE.
  std::uint32_t ecn_ce_count{};
  /// @brief Per-DSCP counts of what this side sent; never filled in by the
  /// library.
  std::vector<std::uint32_t> opposite_sent_dscp_counts;

  /// @brief Compares every field.
  ///
  /// @return Whether both reports are the same.
  bool operator==(const ReceivedTosMetricsReport&) const = default;
};

/// @brief Encodes and decodes extended messages. Stateless; safe from any
/// thread.
class ExtMessageCodec {
 public:
  /// @brief Builds the 12-byte header.
  ///
  /// @param header Whether the body is encrypted.
  /// @return The header, with the tag and key position zeroed for the crypto
  ///     layer to fill.
  [[nodiscard]] static auto encode_header(
      const ExtMessageHeader& header) noexcept
      -> std::array<std::byte, ext_message_header_size>;

  /// @brief Reads the 12-byte header.
  ///
  /// @param payload The message after the type byte and extended header.
  /// @return What it says; nothing when short or of an unknown version or outer
  ///     type.
  [[nodiscard]] static auto decode_header(
      std::span<const std::byte> payload) noexcept
      -> std::optional<ExtMessageHeader>;

  /// @brief Reads a round-trip ping.
  ///
  /// @param payload The protobuf body.
  /// @return The ping; nothing when the body is not an RTT message with ping
  ///     set.
  [[nodiscard]] static auto decode_rtt_ping(std::span<const std::byte> payload)
      -> std::optional<RttPing>;

  /// @brief Builds the pong answering a ping.
  ///
  /// @param ping The ping.
  /// @return The protobuf body; nothing when it cannot be serialised.
  [[nodiscard]] static auto encode_rtt_pong(const RttPing& ping)
      -> std::optional<std::vector<std::byte>>;

  /// @brief Builds a video frame metrics body.
  ///
  /// @param report The report.
  /// @return The protobuf body; nothing past 1024 bytes.
  [[nodiscard]] static auto encode_video_frame_metrics(
      const VideoFrameMetricsReport& report)
      -> std::optional<std::vector<std::byte>>;

  /// @brief Reads a video frame metrics body.
  ///
  /// @param payload The protobuf body.
  /// @return The report; nothing when it is another type or malformed.
  [[nodiscard]] static auto decode_video_frame_metrics(
      std::span<const std::byte> payload)
      -> std::optional<VideoFrameMetricsReport>;

  /// @brief Builds a traffic-class report body.
  ///
  /// @param report The report.
  /// @return The protobuf body; nothing past 1024 bytes.
  [[nodiscard]] static auto encode_received_tos_metrics(
      const ReceivedTosMetricsReport& report)
      -> std::optional<std::vector<std::byte>>;

  /// @brief Reads a traffic-class report body.
  ///
  /// @param payload The protobuf body.
  /// @return The report; nothing when it is another type or malformed.
  [[nodiscard]] static auto decode_received_tos_metrics(
      std::span<const std::byte> payload)
      -> std::optional<ReceivedTosMetricsReport>;
};

}  // namespace libchiaking::net::protocol::takion
