#include "src/core/net/protocol/takion/video_frame_metrics.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

struct RolloverVector final {
  std::uint64_t previous;
  std::uint64_t value;
  std::uint64_t latch;
  std::uint64_t epoch;
  std::uint64_t expanded;
  std::uint64_t next_epoch;
  std::uint64_t next_latch;
};

struct WindowVector final {
  std::uint64_t now_us;
  std::uint64_t sequence;
  std::uint64_t bytes;
  std::uint64_t throughput_bps;
  double loss_ratio;
  std::uint64_t pending_bytes;
  std::uint32_t pending_received;
};

#include "tests/unit/core/net/protocol/takion/video_frame_metrics_vectors.inc"

class VideoFrameMetricsRig : public ::testing::Test {
 protected:
  template <class Expander, class Wide, std::size_t Count>
  static void expect_rollover(const RolloverVector (&vectors)[Count]) {
    for (std::size_t index = 0; index < Count; ++index) {
      SCOPED_TRACE(index);
      const auto& vector = vectors[index];
      Expander expander{vector.previous, static_cast<Wide>(vector.epoch),
                        vector.latch != 0};
      EXPECT_EQ(expander.expand(vector.value),
                static_cast<Wide>(vector.expanded));
      EXPECT_EQ(expander.previous(), vector.value);
      EXPECT_EQ(expander.epoch(), static_cast<Wide>(vector.next_epoch));
      EXPECT_EQ(expander.latched(), (vector.next_latch != 0));
    }
  }

  static auto video(std::uint64_t arrival, std::uint16_t frame,
                    std::uint16_t unit, std::uint16_t total,
                    std::uint16_t packet, std::uint32_t sender,
                    std::uint32_t payload = 1000, std::uint32_t overhead = 30)
      -> takion::VideoFrameObservation {
    takion::VideoFrameObservation observation;
    observation.arrival_us = arrival;
    observation.frame_index = frame;
    observation.unit_index = unit;
    observation.total_units = total;
    observation.packet_index = packet;
    observation.sender_timestamp_us = sender;
    observation.payload_size = payload;
    observation.overhead_size = overhead;
    return observation;
  }
};

using VideoFrameRollover = VideoFrameMetricsRig;
using VideoFrameMetricWindows = VideoFrameMetricsRig;
using VideoFrameReportWorker = VideoFrameMetricsRig;
using VideoFrameMetricsCollector = VideoFrameMetricsRig;

TEST_F(VideoFrameRollover, ExpandsCountersLikeTheNativeValidatedReference) {
  expect_rollover<takion::FrameNumberExpander, std::uint32_t>(
      frame_rollover_vectors);
  expect_rollover<takion::SenderTimestampExpander, std::uint64_t>(
      timestamp_rollover_vectors);
  expect_rollover<takion::PacketIndexExpander, std::uint64_t>(
      packet_rollover_vectors);
  static_assert(sizeof(frame_rollover_vectors) / sizeof(RolloverVector) +
                    sizeof(timestamp_rollover_vectors) /
                        sizeof(RolloverVector) +
                    sizeof(packet_rollover_vectors) / sizeof(RolloverVector) ==
                2178);

  takion::FrameNumberExpander frames;
  EXPECT_EQ(frames.expand(0xfffe), 0xfffeU);
  EXPECT_EQ(frames.expand(0x0001), 0x10001U);
  EXPECT_EQ(frames.expand(0xffff), 0xffffU);
  EXPECT_EQ(frames.expand(0x0002), 0x10002U);
  EXPECT_EQ(frames.expand(0x9000), 0x19000U);
  EXPECT_FALSE(frames.latched());
}

TEST_F(VideoFrameMetricWindows, MatchTheMetricsReference) {
  takion::MetricWindows windows;
  for (const auto& vector : window_vectors) {
    SCOPED_TRACE(vector.now_us);
    windows.receive(vector.now_us, vector.sequence, vector.bytes);
    EXPECT_EQ(windows.throughput_bps(), vector.throughput_bps);
    EXPECT_EQ(windows.loss_ratio(), vector.loss_ratio);
    EXPECT_EQ(windows.pending_bytes(), vector.pending_bytes);
    EXPECT_EQ(windows.pending_received(), vector.pending_received);
  }
}

TEST_F(VideoFrameReportWorker,
       StartsFromTheInitialSnapshotAndFollowsItsSchedule) {
  takion::VideoFrameMetricsCollector collector;
  const auto& initial = collector.snapshot();
  EXPECT_EQ(initial.send_int_us, 16666U);
  EXPECT_EQ(initial.recv_int_us, 0U);
  EXPECT_EQ(initial.transfer_time_us, 0U);
  EXPECT_EQ(initial.recv_size, 0U);
  EXPECT_EQ(initial.frame_no_wide, 0U);
  EXPECT_EQ(initial.frame_no_at_tt_updated, 0U);
  EXPECT_FALSE(initial.is_trans_time_estimated);
  EXPECT_FALSE(initial.is_consecutive_frame_no);
  EXPECT_EQ(initial.transmission_time_us, 0U);
  EXPECT_EQ(initial.throughput_bps, 0U);
  EXPECT_EQ(initial.loss_ratio, 0.0);
  EXPECT_EQ(initial.timestamp_us, 0U);

  takion::VideoFrameReportWorker worker{1000};
  EXPECT_EQ(worker.timeout_us(), 625000U);
  EXPECT_EQ(worker.deadline_us(), 626000U);
  EXPECT_FALSE(worker.due(625999, false));
  EXPECT_TRUE(worker.due(1001, true));
  EXPECT_TRUE(worker.due(626000, false));
  const auto first = worker.report(initial, 626000);
  EXPECT_EQ(first.seq_num, 0U);
  EXPECT_EQ(first.send_int_us, 16666U);
  EXPECT_EQ(first.proc_time_us, 626000U);
  worker.complete(initial, 626000);
  EXPECT_EQ(worker.sequence(), 1U);
  EXPECT_EQ(worker.baseline_us(), 500000U);
  EXPECT_EQ(worker.deadline_us(), 626000 + 625000);
  const auto unchanged = worker.report(initial, 700000);
  EXPECT_EQ(unchanged.seq_num, 1U);
  EXPECT_EQ(unchanged.send_int_us, 16666U);
  worker.complete(initial, 700000);
  EXPECT_EQ(worker.sequence(), 2U);

  takion::VideoFrameSnapshot consecutive;
  consecutive.send_int_us = 16666;
  consecutive.is_consecutive_frame_no = true;
  worker.complete(consecutive, 800000);
  EXPECT_EQ(worker.baseline_us(), 16666U);
  EXPECT_EQ(worker.timeout_us(), 16666 + 4166);
  EXPECT_EQ(worker.deadline_us(), 800000 + 20832);
  takion::VideoFrameSnapshot other;
  other.send_int_us = 33333;
  worker.complete(other, 900000);
  EXPECT_EQ(worker.baseline_us(), 16666U);
  EXPECT_EQ(worker.sequence(), 4U);
}

TEST_F(VideoFrameMetricsCollector,
       PublishesIntervalsAndTimingForCompleteFrames) {
  takion::VideoFrameMetricsCollector collector;
  collector.observe(video(1000, 10, 0, 3, 1, 500, 100, 20));
  EXPECT_TRUE(collector.take_signal());
  collector.observe(video(1100, 10, 1, 3, 2, 600, 100, 20));
  EXPECT_FALSE(collector.take_signal());
  EXPECT_EQ(collector.snapshot().recv_size, 0U);
  collector.observe(video(1200, 10, 2, 3, 3, 700, 100, 20));
  const auto first = collector.snapshot();
  EXPECT_EQ(first.transmission_time_us, 200U);
  EXPECT_EQ(first.transfer_time_us, 200U);
  EXPECT_EQ(first.recv_size, 360U);
  EXPECT_EQ(first.frame_no_at_tt_updated, 10U);
  EXPECT_FALSE(first.is_trans_time_estimated);
  EXPECT_EQ(first.frame_no_wide, 0U);
  EXPECT_EQ(collector.finalized_frame(), 10U);
  EXPECT_EQ(collector.slot_frame(0), 0U);

  collector.observe(video(17666, 11, 0, 3, 4, 17166));
  EXPECT_TRUE(collector.take_signal());
  const auto next = collector.snapshot();
  EXPECT_EQ(next.send_int_us, 16666U);
  EXPECT_EQ(next.recv_int_us, 16666U);
  EXPECT_EQ(next.frame_no_wide, 11U);
  EXPECT_TRUE(next.is_consecutive_frame_no);
  EXPECT_EQ(next.timestamp_us, 17666U);
  EXPECT_EQ(next.recv_size, 360U);

  collector.observe(video(18000, 10, 0, 3, 5, 800));
  EXPECT_FALSE(collector.take_signal());
  EXPECT_EQ(collector.snapshot(), next);

  collector.observe(video(50000, 13, 0, 3, 6, 50000));
  EXPECT_EQ(collector.snapshot().frame_no_wide, 13U);
  EXPECT_FALSE(collector.snapshot().is_consecutive_frame_no);
}

TEST_F(VideoFrameMetricsCollector, EstimatesAnIncompleteFrameWhenItIsEvicted) {
  takion::VideoFrameMetricsCollector collector;
  collector.observe(video(1000, 20, 0, 5, 100, 10000));
  collector.observe(video(1100, 20, 1, 5, 101, 10010));
  collector.observe(video(1200, 20, 2, 5, 102, 10020));
  collector.observe(video(1300, 20, 4, 5, 104, 10030));
  EXPECT_EQ(collector.snapshot().recv_size, 0U);
  collector.observe(video(17666, 21, 0, 5, 105, 26666));
  EXPECT_EQ(collector.slot_frame(0), 20U);
  EXPECT_EQ(collector.slot_frame(1), 21U);
  collector.observe(video(34332, 22, 0, 5, 106, 43332));
  const auto estimated = collector.snapshot();
  EXPECT_EQ(estimated.transfer_time_us, 375U);
  EXPECT_EQ(estimated.recv_size, 5150U);
  EXPECT_EQ(estimated.transmission_time_us, 0U);
  EXPECT_TRUE(estimated.is_trans_time_estimated);
  EXPECT_EQ(estimated.frame_no_at_tt_updated, 20U);
  EXPECT_EQ(estimated.frame_no_wide, 22U);
  EXPECT_EQ(collector.finalized_frame(), 20U);
  EXPECT_EQ(collector.slot_frame(0), 22U);
  EXPECT_EQ(collector.slot_frame(1), 21U);

  collector.observe(video(40000, 23, 0, 5, 110, 50000));
  EXPECT_EQ(collector.finalized_frame(), 21U);
  EXPECT_EQ(collector.snapshot().recv_size, 5150U);
  EXPECT_EQ(collector.slot_frame(1), 23U);

  collector.observe(video(40001, 21, 1, 5, 111, 26667));
  EXPECT_EQ(collector.slot_frame(0), 22U);
  EXPECT_EQ(collector.slot_frame(1), 23U);
}

TEST_F(VideoFrameMetricsCollector, ExcludesTheSenderGapBetweenSlices) {
  takion::VideoFrameMetricsCollector collector;
  collector.observe(video(0, 29, 0, 1, 1, 0));
  auto unit = [&](std::uint64_t arrival, std::uint16_t index,
                  std::uint32_t sender, std::uint16_t slice) {
    auto observation = video(arrival, 30, index, 4,
                             static_cast<std::uint16_t>(2 + index), sender);
    observation.slice_index = slice;
    observation.slice_count = 2;
    return observation;
  };
  collector.observe(unit(10000, 0, 1000, 0));
  collector.observe(unit(10100, 1, 1100, 0));
  collector.observe(unit(14000, 2, 5000, 1));
  collector.observe(unit(14100, 3, 5100, 1));
  const auto snapshot = collector.snapshot();
  EXPECT_EQ(snapshot.transmission_time_us, 200U);
  EXPECT_EQ(snapshot.transfer_time_us, 200U);
  EXPECT_EQ(snapshot.frame_no_at_tt_updated, 30U);
}

}  // namespace
}  // namespace takion_test
