#include "src/core/net/protocol/takion/video_frame_metrics.hpp"

#include <algorithm>

namespace libchiaking::net::protocol::takion {

void MetricWindows::receive(std::uint64_t now_us,
                            std::uint64_t expanded_sequence,
                            std::uint64_t bytes) noexcept {
  loss_updated_ = false;
  byte_count_ += bytes;
  if (!started_) {
    started_ = true;
    loss_start_us_ = now_us;
    throughput_start_us_ = now_us;
    received_count_ = 1;
    return;
  }
  const auto elapsed = now_us - throughput_start_us_;
  if (elapsed >= video_frame_throughput_window_us) {
    throughput_bps_ = 8000000U * byte_count_ / elapsed;
    byte_count_ = 0;
    throughput_start_us_ = now_us;
  }
  const auto span =
      static_cast<std::int64_t>(expanded_sequence - previous_sequence_);
  ++received_count_;
  if (now_us - loss_start_us_ >= video_frame_loss_window_us ||
      span >= video_frame_loss_window_packets) {
    loss_start_us_ = now_us;
    previous_sequence_ = expanded_sequence;
    const auto missing = span - static_cast<std::int64_t>(received_count_);
    if (span > 0 && missing >= 0) {
      loss_ratio_ = static_cast<double>(missing) / static_cast<double>(span);
      loss_updated_ = true;
    }
    received_count_ = 0;
  }
}

void VideoFrameMetricsCollector::initialize(
    Record& record, const VideoFrameObservation& observation,
    std::uint32_t frame, std::uint64_t sender, std::uint64_t packet) noexcept {
  signalled_ = true;
  record = Record{};
  record.frame = frame;
  record.first_arrival_us = observation.arrival_us;
  record.last_arrival_us = observation.arrival_us;
  record.total_bytes = observation.payload_size + observation.overhead_size;
  record.expected_units = observation.total_units;
  record.wire_frame = observation.frame_index;
  record.received_units = 1;
  record.payload_bytes = observation.payload_size;
  record.profile = observation.profile;
  record.first_sender_us = sender;
  record.last_sender_us = sender;
  record.index_sum = observation.unit_index;
  record.fec_units = observation.fec_units;
  record.slice_index = observation.slice_index;
  record.slice_last_sender_us = sender;
  record.excluded_gap_us = 0;
  record.slice_count = observation.slice_count;
  record.first_packet = packet;
  record.last_packet = packet;
}

bool VideoFrameMetricsCollector::finalize(Record& record, std::uint64_t now_us,
                                          bool new_frame) noexcept {
  const auto expected = record.expected_units;
  bool complete = false;
  if (static_cast<std::uint16_t>(expected - 1U) < record.received_units) {
    const auto required = (static_cast<std::uint32_t>(expected) *
                           static_cast<std::uint32_t>(expected - 1U)) >>
                          1U;
    complete = record.index_sum >= required;
  } else if (!new_frame) {
    return false;
  }
  if (!baseline_) {
    baseline_frame_ = record.frame;
    baseline_sender_us_ = record.first_sender_us;
    baseline_arrival_us_ = record.first_arrival_us;
    baseline_ = true;
    return complete;
  }

  std::int32_t transmission{};
  const auto gap = record.excluded_gap_us;
  if (record.last_sender_us >= record.first_sender_us) {
    const auto span = static_cast<std::int32_t>(static_cast<std::uint32_t>(
        record.last_sender_us - record.first_sender_us));
    transmission = span - (gap < span ? gap : 0);
  }
  const auto received_for = static_cast<std::int32_t>(
      static_cast<std::uint32_t>(now_us) -
      static_cast<std::uint32_t>(record.first_arrival_us));
  const auto transfer = received_for <= gap ? 0 : received_for - gap;

  if (new_frame) {
    snapshot_.send_int_us =
        static_cast<std::int32_t>(static_cast<std::uint32_t>(
            record.first_sender_us - baseline_sender_us_));
    snapshot_.recv_int_us =
        static_cast<std::int32_t>(static_cast<std::uint32_t>(
            record.first_arrival_us - baseline_arrival_us_));
    snapshot_.frame_no_wide = record.frame;
    snapshot_.timestamp_us = record.first_arrival_us;
    snapshot_.is_consecutive_frame_no = record.frame - baseline_frame_ == 1U;
    baseline_frame_ = record.frame;
    baseline_sender_us_ = record.first_sender_us;
    baseline_arrival_us_ = record.first_arrival_us;
  }
  if (complete) {
    snapshot_.transmission_time_us = transmission;
    snapshot_.transfer_time_us = transfer;
    snapshot_.recv_size = record.total_bytes;
    snapshot_.is_trans_time_estimated = false;
    snapshot_.frame_no_at_tt_updated = record.wire_frame;
    snapshot_.throughput_bps =
        static_cast<std::uint32_t>(windows_.throughput_bps());
    snapshot_.loss_ratio = windows_.loss_ratio();
  }
  return complete;
}

bool VideoFrameMetricsCollector::estimate(Record& record) noexcept {
  const auto span =
      static_cast<std::int64_t>(record.last_packet - record.first_packet);
  std::uint16_t expected = record.expected_units;
  if (span > static_cast<std::int64_t>(expected)) {
    expected = static_cast<std::uint16_t>(span);
    record.expected_units = expected;
  }
  const auto received = record.received_units;
  if (expected < video_frame_estimate_minimum_units ||
      received < video_frame_estimate_minimum_units)
    return false;
  const auto ratio = std::min(
      static_cast<double>(received) / static_cast<double>(expected), 1.0);
  if (ratio < video_frame_estimate_minimum_ratio) return false;
  std::int32_t transmission{};
  const auto gap = record.excluded_gap_us;
  if (received == expected && record.last_sender_us >= record.first_sender_us) {
    const auto sender_span =
        static_cast<std::int32_t>(static_cast<std::uint32_t>(
            record.last_sender_us - record.first_sender_us));
    transmission = sender_span - (gap < sender_span ? gap : 0);
  }
  const auto scale = 1.0 / ratio;
  const auto duration = static_cast<std::int32_t>(static_cast<std::uint32_t>(
      record.last_arrival_us - record.first_arrival_us));
  const auto scaled =
      static_cast<std::int32_t>(scale * static_cast<double>(duration));
  snapshot_.transmission_time_us = transmission;
  snapshot_.transfer_time_us = scaled <= gap ? 0 : scaled - gap;
  snapshot_.recv_size = static_cast<std::uint32_t>(static_cast<std::int32_t>(
      scale * static_cast<double>(record.total_bytes)));
  snapshot_.is_trans_time_estimated = received != expected;
  snapshot_.frame_no_at_tt_updated = record.wire_frame;
  snapshot_.throughput_bps =
      static_cast<std::uint32_t>(windows_.throughput_bps());
  snapshot_.loss_ratio = windows_.loss_ratio();
  return true;
}

void VideoFrameMetricsCollector::retire(std::uint32_t frame) noexcept {
  finalized_ = std::max(finalized_, frame);
  for (auto& slot : slots_) {
    if (static_cast<std::uint32_t>(slot.frame - 1U) < finalized_)
      slot.frame = 0;
  }
}

void VideoFrameMetricsCollector::observe(
    const VideoFrameObservation& observation) {
  const auto bytes =
      std::uint64_t{observation.payload_size} + observation.overhead_size;
  const auto frame = frames_.expand(observation.frame_index);
  const auto sender = timestamps_.expand(observation.sender_timestamp_us);
  const auto sender_unlatched =
      timestamps_.unlatched(observation.sender_timestamp_us);
  const auto packet = packets_.expand(observation.packet_index);

  Record* matched = nullptr;
  if (slots_[0].frame == frame)
    matched = &slots_[0];
  else if (slots_[1].frame == frame)
    matched = &slots_[1];

  if (matched) {
    auto& record = *matched;
    record.total_bytes += static_cast<std::uint32_t>(bytes);
    record.payload_bytes += observation.payload_size;
    ++record.received_units;
    record.last_arrival_us = observation.arrival_us;
    record.first_sender_us = std::min(record.first_sender_us, sender);
    record.last_sender_us = std::max(record.last_sender_us, sender);
    record.index_sum += observation.unit_index;
    record.fec_units = std::max(record.fec_units, observation.fec_units);
    record.first_packet = std::min(record.first_packet, packet);
    record.last_packet = std::max(record.last_packet, packet);
    record.expected_units =
        std::max(record.expected_units, observation.total_units);
    if (observation.slice_count >= 2U) {
      if (observation.slice_index > record.slice_index) {
        if (observation.slice_index - record.slice_index == 1U)
          record.excluded_gap_us = static_cast<std::int32_t>(
              static_cast<std::uint32_t>(record.excluded_gap_us) +
              static_cast<std::uint32_t>(sender_unlatched) -
              static_cast<std::uint32_t>(record.slice_last_sender_us));
        record.slice_index = observation.slice_index;
      }
      if (record.slice_index == observation.slice_index &&
          record.slice_last_sender_us < sender)
        record.slice_last_sender_us = sender;
    }
    if (finalize(record, observation.arrival_us, false)) retire(record.frame);
  } else if (finalized_ < frame) {
    Record* target = nullptr;
    if (slots_[0].frame == 0)
      target = &slots_[0];
    else if (slots_[1].frame == 0)
      target = &slots_[1];
    if (!target &&
        (slots_[0].frame != 0xffffffffU || slots_[1].frame < slots_[0].frame)) {
      auto& smaller = slots_[1].frame < slots_[0].frame ? slots_[1] : slots_[0];
      if (smaller.frame < frame) {
        static_cast<void>(estimate(smaller));
        retire(smaller.frame);
        target = &smaller;
      }
    }
    if (target) {
      initialize(*target, observation, frame, sender, packet);
      if (finalize(*target, observation.arrival_us, true))
        retire(target->frame);
    }
  }

  windows_.receive(observation.arrival_us, packet, bytes);
  if (windows_.loss_updated()) snapshot_.loss_ratio = windows_.loss_ratio();
}

VideoFrameMetricsReport VideoFrameReportWorker::report(
    const VideoFrameSnapshot& snapshot, std::uint64_t now_us) const noexcept {
  return VideoFrameMetricsReport{
      sequence_,
      snapshot.send_int_us,
      snapshot.recv_int_us,
      snapshot.transfer_time_us,
      snapshot.recv_size,
      snapshot.frame_no_wide,
      snapshot.frame_no_at_tt_updated,
      snapshot.is_trans_time_estimated,
      snapshot.is_consecutive_frame_no,
      snapshot.transmission_time_us,
      snapshot.throughput_bps,
      snapshot.loss_ratio,
      static_cast<std::uint32_t>(now_us - snapshot.timestamp_us)};
}

void VideoFrameReportWorker::complete(const VideoFrameSnapshot& snapshot,
                                      std::uint64_t now_us) noexcept {
  ++sequence_;
  if (snapshot.is_consecutive_frame_no)
    baseline_us_ = static_cast<std::uint32_t>(snapshot.send_int_us);
  deadline_us_ = now_us + timeout_us();
}

}  // namespace libchiaking::net::protocol::takion
