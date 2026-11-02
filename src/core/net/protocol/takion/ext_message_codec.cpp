#include "src/core/net/protocol/takion/ext_message_codec.hpp"

#include "src/core/net/protocol/takion/nanopb_message.hpp"

#include "src/core/utils/bytes/byte_io.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::net::protocol::takion {

auto ExtMessageCodec::encode_header(const ExtMessageHeader& header) noexcept
    -> std::array<std::byte, ext_message_header_size> {
  std::array<std::byte, ext_message_header_size> encoded{};
  auto fields = std::span{encoded}.subspan(ext_message_header_reserved_size);
  fields[0] = std::byte{ext_message_header_version};
  fields[1] = std::byte{ext_message_outer_type_protobuf};
  bytes::store_be<std::uint16_t>(
      fields.subspan(2),
      header.encrypted ? ext_message_flag_encrypted : std::uint16_t{});
  return encoded;
}

auto ExtMessageCodec::decode_header(std::span<const std::byte> payload) noexcept
    -> std::optional<ExtMessageHeader> {
  bytes::Reader reader{payload};
  if (!reader.skip(ext_message_header_reserved_size)) return std::nullopt;
  const auto version = reader.u8();
  const auto outer = reader.u8();
  const auto flags = reader.u16_be();
  if (!version || !outer || !flags ||
      version.value() != ext_message_header_version ||
      outer.value() != ext_message_outer_type_protobuf)
    return std::nullopt;
  return ExtMessageHeader{(flags.value() & ext_message_flag_encrypted) != 0};
}

auto ExtMessageCodec::decode_rtt_ping(std::span<const std::byte> payload)
    -> std::optional<RttPing> {
  ExtWire message;
  if (!message.decode(payload) ||
      message->type != tkproto_ExtMessage_PayloadType_RTT ||
      !message->has_rtt_payload)
    return std::nullopt;
  const auto& rtt = message->rtt_payload;
  if (!rtt.has_ping || !rtt.ping) return std::nullopt;
  RttPing ping;
  if (rtt.has_entry_index) ping.entry_index = rtt.entry_index;
  if (rtt.has_orig_send_timestamp_us)
    ping.orig_send_timestamp_us = rtt.orig_send_timestamp_us;
  return ping;
}

auto ExtMessageCodec::encode_video_frame_metrics(
    const VideoFrameMetricsReport& report)
    -> std::optional<std::vector<std::byte>> {
  ExtWire message;
  message->has_type = true;
  message->type =
      ExtWire::number(tkproto_ExtMessage_PayloadType_CCVIDEOFRAMEMETRICS);
  message->has_cc_video_frame_metrics_payload = true;
  auto& metrics = message->cc_video_frame_metrics_payload;
  metrics.seq_num = report.seq_num;
  metrics.send_int_us = report.send_int_us;
  metrics.recv_int_us = report.recv_int_us;
  metrics.transfer_time_us = report.transfer_time_us;
  metrics.recv_size = report.recv_size;
  metrics.frame_no_wide = report.frame_no_wide;
  metrics.frame_no_at_tt_updated = report.frame_no_at_tt_updated;
  metrics.is_trans_time_estimated = report.is_trans_time_estimated;
  metrics.is_consecutive_frame_no = report.is_consecutive_frame_no;
  metrics.transmission_time_us = report.transmission_time_us;
  metrics.throughput_bps = report.throughput_bps;
  metrics.loss_ratio = report.loss_ratio;
  metrics.proc_time_us = report.proc_time_us;
  auto bytes =
      message.encode<std::vector<std::byte>>(ext_message_payload_limit);
  if (!bytes) return std::nullopt;
  return std::move(bytes).value();
}

auto ExtMessageCodec::decode_video_frame_metrics(
    std::span<const std::byte> payload)
    -> std::optional<VideoFrameMetricsReport> {
  ExtWire message;
  if (!message.decode(payload) || !message->has_type ||
      message->type != tkproto_ExtMessage_PayloadType_CCVIDEOFRAMEMETRICS ||
      !message->has_cc_video_frame_metrics_payload)
    return std::nullopt;
  const auto& metrics = message->cc_video_frame_metrics_payload;
  return VideoFrameMetricsReport{metrics.seq_num,
                                 metrics.send_int_us,
                                 metrics.recv_int_us,
                                 metrics.transfer_time_us,
                                 metrics.recv_size,
                                 metrics.frame_no_wide,
                                 metrics.frame_no_at_tt_updated,
                                 metrics.is_trans_time_estimated,
                                 metrics.is_consecutive_frame_no,
                                 metrics.transmission_time_us,
                                 metrics.throughput_bps,
                                 metrics.loss_ratio,
                                 metrics.proc_time_us};
}

auto ExtMessageCodec::encode_received_tos_metrics(
    const ReceivedTosMetricsReport& report)
    -> std::optional<std::vector<std::byte>> {
  ExtWire message;
  message->has_type = true;
  message->type =
      ExtWire::number(tkproto_ExtMessage_PayloadType_RECEIVEDTOSMETRICS);
  message->has_received_tos_metrics_payload = true;
  auto& metrics = message->received_tos_metrics_payload;
  metrics.time_offset_msec = report.time_offset_msec;
  std::ranges::copy(report.dscp_counts,
                    ExtWire::resize(metrics.dscp_cnt, metrics.dscp_cnt_count,
                                    report.dscp_counts.size())
                        .begin());
  metrics.has_ecn_ect1_cnt = true;
  metrics.ecn_ect1_cnt = report.ecn_ect1_count;
  metrics.has_ecn_ect0_cnt = true;
  metrics.ecn_ect0_cnt = report.ecn_ect0_count;
  metrics.has_ecn_ce_cnt = true;
  metrics.ecn_ce_cnt = report.ecn_ce_count;
  std::ranges::copy(report.opposite_sent_dscp_counts,
                    ExtWire::resize(metrics.opposite_sent_dscp_cnt,
                                    metrics.opposite_sent_dscp_cnt_count,
                                    report.opposite_sent_dscp_counts.size())
                        .begin());
  auto bytes =
      message.encode<std::vector<std::byte>>(ext_message_payload_limit);
  if (!bytes) return std::nullopt;
  return std::move(bytes).value();
}

auto ExtMessageCodec::decode_received_tos_metrics(
    std::span<const std::byte> payload)
    -> std::optional<ReceivedTosMetricsReport> {
  ExtWire message;
  if (!message.decode(payload) || !message->has_type ||
      message->type != tkproto_ExtMessage_PayloadType_RECEIVEDTOSMETRICS ||
      !message->has_received_tos_metrics_payload)
    return std::nullopt;
  const auto& metrics = message->received_tos_metrics_payload;
  ReceivedTosMetricsReport report;
  report.time_offset_msec = metrics.time_offset_msec;
  report.dscp_counts.assign(metrics.dscp_cnt,
                            metrics.dscp_cnt + metrics.dscp_cnt_count);
  report.ecn_ect1_count = metrics.ecn_ect1_cnt;
  report.ecn_ect0_count = metrics.ecn_ect0_cnt;
  report.ecn_ce_count = metrics.ecn_ce_cnt;
  report.opposite_sent_dscp_counts.assign(
      metrics.opposite_sent_dscp_cnt,
      metrics.opposite_sent_dscp_cnt + metrics.opposite_sent_dscp_cnt_count);
  return report;
}

auto ExtMessageCodec::encode_rtt_pong(const RttPing& ping)
    -> std::optional<std::vector<std::byte>> {
  ExtWire message;
  message->has_type = true;
  message->type = ExtWire::number(tkproto_ExtMessage_PayloadType_RTT);
  message->has_rtt_payload = true;
  auto& rtt = message->rtt_payload;
  rtt.has_ping = true;
  rtt.ping = false;
  if (ping.entry_index) {
    rtt.has_entry_index = true;
    rtt.entry_index = *ping.entry_index;
  }
  if (ping.orig_send_timestamp_us) {
    rtt.has_orig_send_timestamp_us = true;
    rtt.orig_send_timestamp_us = *ping.orig_send_timestamp_us;
  }
  auto bytes =
      message.encode<std::vector<std::byte>>(ext_message_payload_limit);
  if (!bytes) return std::nullopt;
  return std::move(bytes).value();
}

}  // namespace libchiaking::net::protocol::takion
