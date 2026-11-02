#include "src/core/net/kakusan/congestion.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;

class CongestionControl : public ::testing::Test {
 protected:
  static auto tolerance(double expected) -> double {
    return 1e-6 * std::max(1.0, std::abs(expected));
  }

  static auto send(CongestionController& target, std::size_t bytes,
                   std::uint64_t at) -> std::uint64_t {
    const auto number = target.on_packet_sent(bytes, false, at);
    if (!number) {
      ADD_FAILURE() << "refused to track a packet at " << at;
      return 0;
    }
    return number.value();
  }

  static void send_many(CongestionController& target, std::size_t count,
                        std::uint64_t at) {
    for (std::size_t index = 0; index < count; ++index) send(target, 1200, at);
  }

  static auto ack(CongestionController& target, std::uint32_t delay,
                  std::initializer_list<AckRange> ranges, std::uint64_t at)
      -> AckOutcome {
    MediaAck message;
    message.ranges.assign(ranges.begin(), ranges.end());
    message.largest = message.ranges.front().high;
    message.delay_us = delay;
    const auto outcome = target.on_ack(message, at);
    if (!outcome) {
      ADD_FAILURE() << "refused an acknowledgement at " << at;
      return {};
    }
    return outcome.value();
  }

  void sample(std::uint64_t round_trip) {
    const auto number = send(controller, 1200, now);
    now += round_trip;
    ack(controller, 0, {AckRange{number, number}}, now);
  }

  void sample_at(std::uint64_t sent, std::uint64_t round_trip) {
    const auto number = send(controller, 1200, sent);
    ack(controller, 0, {AckRange{number, number}}, sent + round_trip);
  }

  void rounds(int count, std::uint64_t missing) {
    for (int index = 0; index < count; ++index) {
      const auto first = controller.next_packet_number();
      send_many(controller, 20, now);
      now += 50000;
      ack(controller, 0, {AckRange{first + missing, first + 19}}, now);
    }
  }

  auto rejects(MediaAck message) -> ::testing::AssertionResult {
    const auto result = controller.on_ack(message, 1000);
    if (result) return ::testing::AssertionFailure() << "accepted";
    if (result.error() != Error::protocol)
      return ::testing::AssertionFailure()
             << "rejected with " << ::testing::PrintToString(result.error());
    if (controller.bytes_in_flight() != 3600)
      return ::testing::AssertionFailure()
             << "left " << controller.bytes_in_flight() << " in flight";
    if (controller.largest_acked())
      return ::testing::AssertionFailure() << "recorded a largest ack";
    if (controller.has_rtt_sample())
      return ::testing::AssertionFailure() << "took an rtt sample";
    return ::testing::AssertionSuccess();
  }

  CongestionController controller;
  std::uint64_t now{};
};

TEST_F(CongestionControl, StartsEmptyWithTheDefaultRoundTrip) {
  EXPECT_EQ(controller.bytes_in_flight(), 0U);
  EXPECT_EQ(controller.smoothed_rtt_us(), 333000.0);
  EXPECT_EQ(controller.rtt_variance_us(), 166500.0);
  EXPECT_FALSE(controller.has_rtt_sample());
  EXPECT_FALSE(controller.loss_detection_deadline());
  EXPECT_EQ(controller.path(), PathState::clear);
  EXPECT_EQ(controller.delay_floor_us(), 0.0);
}

TEST_F(CongestionControl, NumbersAndTracksSentPackets) {
  for (std::uint64_t index = 0; index < 10; ++index) {
    EXPECT_TRUE(controller.can_track());
    EXPECT_EQ(send(controller, 1200, 0), index);
  }
  EXPECT_EQ(controller.bytes_in_flight(), 12000U);
  EXPECT_TRUE(controller.can_track());
  EXPECT_EQ(controller.loss_detection_deadline(), 1024000U);
}

TEST_F(CongestionControl, RefusesAnEmptyOrOversizedPacket) {
  send_many(controller, 10, 0);
  EXPECT_FALSE(controller.on_packet_sent(1201, false, 0));
  EXPECT_FALSE(controller.on_packet_sent(0, false, 0));
}

TEST_F(CongestionControl, DeclaresLossByPacketThenByTimeThreshold) {
  send_many(controller, 5, 0);
  const auto outcome = ack(controller, 0, {AckRange{3, 3}}, 50000);
  EXPECT_EQ(outcome.lost, 1U);
  EXPECT_EQ(controller.bytes_in_flight(), 3U * 1200U);
  EXPECT_EQ(controller.loss_detection_deadline(), 56250U);
  EXPECT_EQ(controller.on_timeout(56249), TimeoutEvent::none);
  EXPECT_EQ(controller.on_timeout(56250), TimeoutEvent::loss);
  EXPECT_EQ(controller.bytes_in_flight(), 1200U);
  EXPECT_EQ(controller.loss_detection_deadline(), 175000U);
}

TEST_F(CongestionControl, TimesLossFromTheLatestRoundTrip) {
  send_many(controller, 2, 0);
  ack(controller, 0, {AckRange{1, 1}}, 20000);
  send_many(controller, 2, 20000);
  ack(controller, 0, {AckRange{3, 3}, AckRange{1, 1}}, 100000);
  EXPECT_EQ(controller.latest_rtt_us(), 80000.0);
  EXPECT_EQ(controller.bytes_in_flight(), 1200U);
  EXPECT_EQ(controller.loss_detection_deadline(), 20000U + 90000U);
}

TEST_F(CongestionControl, KeepsAMillisecondTimeThresholdOnAFastPath) {
  send_many(controller, 2, 0);
  ack(controller, 0, {AckRange{1, 1}}, 400);
  EXPECT_EQ(controller.smoothed_rtt_us(), 400.0);
  EXPECT_EQ(controller.loss_detection_deadline(), 1000U);
  EXPECT_EQ(controller.on_timeout(999), TimeoutEvent::none);
  EXPECT_EQ(controller.on_timeout(1000), TimeoutEvent::loss);
  EXPECT_EQ(controller.bytes_in_flight(), 0U);
}

TEST_F(CongestionControl, ProbesAndBacksOffWhenNothingIsAcknowledged) {
  send(controller, 1200, 0);
  EXPECT_EQ(controller.loss_detection_deadline(), 1024000U);
  EXPECT_EQ(controller.on_timeout(1023999), TimeoutEvent::none);
  EXPECT_FALSE(controller.probe_pending());
  EXPECT_EQ(controller.on_timeout(1024000), TimeoutEvent::probe);
  EXPECT_TRUE(controller.probe_pending());
  EXPECT_EQ(controller.pto_count(), 1U);
  EXPECT_EQ(controller.on_timeout(1024000), TimeoutEvent::none);
  EXPECT_EQ(controller.loss_detection_deadline(), 2048000U);

  send(controller, 1200, 1024000);
  EXPECT_TRUE(controller.probe_pending());
  const auto probe = controller.on_packet_sent(76, true, 1024000);
  ASSERT_TRUE(probe);
  EXPECT_EQ(probe.value(), 2U);
  EXPECT_FALSE(controller.probe_pending());
  EXPECT_EQ(controller.bytes_in_flight(), 2476U);
  EXPECT_EQ(controller.loss_detection_deadline(), 1024000U + 2048000U);

  EXPECT_EQ(controller.on_timeout(3072000), TimeoutEvent::probe);
  EXPECT_EQ(controller.pto_count(), 2U);
  EXPECT_EQ(controller.loss_detection_deadline(), 1024000U + 4096000U);
  EXPECT_TRUE(controller.on_packet_sent(76, true, 3072000));
  EXPECT_EQ(controller.loss_detection_deadline(), 3072000U + 4096000U);

  const auto outcome = ack(controller, 0, {AckRange{0, 3}}, 3100000);
  EXPECT_EQ(outcome.newly_acked, 4U);
  EXPECT_TRUE(outcome.rtt_sampled);
  EXPECT_EQ(controller.pto_count(), 0U);
  EXPECT_EQ(controller.latest_rtt_us(), 28000.0);
  EXPECT_FALSE(controller.loss_detection_deadline());

  send(controller, 1200, 3200000);
  EXPECT_EQ(controller.loss_detection_deadline(),
            3200000U + 28000U + 56000U + 25000U);
}

TEST_F(CongestionControl, KeepsBackingOffOnARepeatedAcknowledgement) {
  send(controller, 1200, 0);
  ack(controller, 0, {AckRange{0, 0}}, 50000);
  send(controller, 1200, 100000);
  EXPECT_EQ(controller.on_timeout(275000), TimeoutEvent::probe);
  EXPECT_TRUE(controller.on_packet_sent(76, true, 275000));
  EXPECT_EQ(controller.pto_count(), 1U);
  ack(controller, 0, {AckRange{0, 0}}, 280000);
  EXPECT_EQ(controller.pto_count(), 1U);
  EXPECT_EQ(controller.loss_detection_deadline(), 275000U + 350000U);
}

TEST_F(CongestionControl, SamplesTheRoundTripFromTheLargestNewAck) {
  send(controller, 1200, 0);
  ack(controller, 0, {AckRange{0, 0}}, 50000);

  send(controller, 1200, 100000);
  const auto clamped = ack(controller, 40000, {AckRange{0, 1}}, 180000);
  EXPECT_TRUE(clamped.rtt_sampled);
  EXPECT_EQ(controller.latest_rtt_us(), 80000.0);
  EXPECT_EQ(controller.min_rtt_us(), 50000.0);
  EXPECT_NEAR(controller.rtt_variance_us(), 20000.0, tolerance(20000.0));
  EXPECT_NEAR(controller.smoothed_rtt_us(), 50625.0, tolerance(50625.0));

  send(controller, 1200, 200000);
  ack(controller, 20000, {AckRange{0, 2}}, 260000);
  EXPECT_EQ(controller.latest_rtt_us(), 60000.0);
  EXPECT_NEAR(controller.rtt_variance_us(), 17343.75, tolerance(17343.75));
  EXPECT_NEAR(controller.smoothed_rtt_us(), 51796.875, tolerance(51796.875));

  const auto duplicate = ack(controller, 20000, {AckRange{0, 2}}, 300000);
  EXPECT_EQ(duplicate.newly_acked, 0U);
  EXPECT_FALSE(duplicate.rtt_sampled);
  EXPECT_EQ(controller.latest_rtt_us(), 60000.0);
  EXPECT_NEAR(controller.smoothed_rtt_us(), 51796.875, tolerance(51796.875));

  send(controller, 1200, 300000);
  send(controller, 1200, 300000);
  ack(controller, 0, {AckRange{4, 4}}, 320000);
  EXPECT_EQ(controller.min_rtt_us(), 20000.0);
  const auto srtt = controller.smoothed_rtt_us();
  const auto lower = ack(controller, 0, {AckRange{3, 4}}, 400000);
  EXPECT_EQ(lower.newly_acked, 1U);
  EXPECT_FALSE(lower.rtt_sampled);
  EXPECT_EQ(controller.latest_rtt_us(), 20000.0);
  EXPECT_EQ(controller.smoothed_rtt_us(), srtt);
}

TEST_F(CongestionControl, SubtractsTheAckDelayWhenItStaysAboveTheMinimum) {
  send(controller, 1200, 0);
  ack(controller, 0, {AckRange{0, 0}}, 50000);
  send(controller, 1200, 100000);
  ack(controller, 10000, {AckRange{0, 1}}, 180000);
  const auto expected = 0.875 * 50000.0 + 0.125 * 70000.0;
  EXPECT_NEAR(controller.smoothed_rtt_us(), expected, tolerance(expected));
}

TEST_F(CongestionControl, IgnoresALateAckOfALostPacket) {
  send_many(controller, 5, 0);
  const auto first = ack(controller, 0, {AckRange{3, 4}}, 50000);
  EXPECT_EQ(first.lost, 2U);
  EXPECT_EQ(controller.bytes_in_flight(), 1200U);
  const auto srtt = controller.smoothed_rtt_us();
  const auto late = ack(controller, 0, {AckRange{3, 4}, AckRange{0, 1}}, 60000);
  EXPECT_EQ(late.newly_acked, 0U);
  EXPECT_EQ(late.lost, 0U);
  EXPECT_FALSE(late.rtt_sampled);
  EXPECT_EQ(controller.bytes_in_flight(), 1200U);
  EXPECT_EQ(controller.smoothed_rtt_us(), srtt);
  EXPECT_EQ(controller.tracked_packets(), 3U);
}

TEST_F(CongestionControl, MarksThePathCongestedOnDelayAndReleasesIt) {
  sample(50000);
  EXPECT_EQ(controller.path(), PathState::clear);
  EXPECT_EQ(controller.delay_floor_us(), 50000.0);
  EXPECT_EQ(controller.delay_limit_us(), 250000.0);
  EXPECT_EQ(controller.delay_release_us(), 150000.0);

  std::size_t slow = 0;
  while (controller.path() == PathState::clear && slow < 100) {
    sample(400000);
    ++slow;
  }
  EXPECT_EQ(slow, 7U);
  EXPECT_GT(controller.smoothed_rtt_us(), 250000.0);
  EXPECT_EQ(controller.delay_floor_us(), 50000.0);

  std::size_t quick = 0;
  while (controller.path() == PathState::congested && quick < 100) {
    sample(50000);
    ++quick;
  }
  EXPECT_EQ(quick, 6U);
  EXPECT_LT(controller.smoothed_rtt_us(), 150000.0);

  controller.reset();
  EXPECT_EQ(controller.path(), PathState::clear);
  EXPECT_EQ(controller.delay_floor_us(), 0.0);
}

TEST_F(CongestionControl, MovesTheDelayFloorWithThePath) {
  sample_at(0, 50000);
  sample_at(delay_floor_window_us, 80000);
  EXPECT_EQ(controller.delay_floor_us(), 50000.0);
  sample_at(2 * delay_floor_window_us, 80000);
  EXPECT_EQ(controller.delay_floor_us(), 80000.0);
  EXPECT_EQ(controller.min_rtt_us(), 50000.0);
  sample_at(5 * delay_floor_window_us, 120000);
  EXPECT_EQ(controller.delay_floor_us(), 120000.0);
  EXPECT_EQ(controller.delay_limit_us(), 320000.0);
  EXPECT_EQ(controller.path(), PathState::clear);
}

TEST_F(CongestionControl, MarksThePathCongestedOnHeavyLossAndReleasesIt) {
  rounds(40, 1);
  EXPECT_EQ(controller.lost_packets(), 40U);
  EXPECT_EQ(controller.loss(), PathState::clear);
  EXPECT_EQ(controller.path(), PathState::clear);

  rounds(9, 4);
  EXPECT_EQ(controller.loss(), PathState::clear);
  rounds(1, 4);
  EXPECT_EQ(controller.loss(), PathState::congested);
  EXPECT_EQ(controller.delay(), PathState::clear);
  EXPECT_EQ(controller.path(), PathState::congested);

  rounds(10, 1);
  rounds(10, 1);
  EXPECT_EQ(controller.loss(), PathState::congested);
  rounds(10, 0);
  rounds(1, 0);
  EXPECT_EQ(controller.loss(), PathState::clear);
  EXPECT_EQ(controller.path(), PathState::clear);
  EXPECT_EQ(controller.bytes_in_flight(), 0U);
}

TEST_F(CongestionControl, DoesNotMarkSparseLossSpreadOverWindows) {
  for (std::uint64_t index = 0; index < 20; ++index) {
    const auto first = controller.next_packet_number();
    send_many(controller, 5, index * loss_window_us);
    ack(controller, 0, {AckRange{first + 4, first + 4}},
        index * loss_window_us + 50000);
  }
  EXPECT_GE(controller.lost_packets(), 20U);
  EXPECT_EQ(controller.loss(), PathState::clear);
}

TEST_F(CongestionControl, RejectsAMalformedAckWithoutChangingState) {
  send_many(controller, 3, 0);
  EXPECT_TRUE(rejects(MediaAck{3, 0, {AckRange{0, 3}}}));
  EXPECT_TRUE(rejects(MediaAck{3, 0, {AckRange{3, 3}}}));
  EXPECT_TRUE(rejects(MediaAck{100, 0, {AckRange{100, 100}, AckRange{0, 1}}}));
  EXPECT_TRUE(rejects(MediaAck{2, 0, {}}));
  EXPECT_TRUE(rejects(MediaAck{2, 0, {AckRange{0, 1}}}));
  EXPECT_TRUE(rejects(MediaAck{2, 0, {AckRange{2, 1}}}));
  EXPECT_TRUE(rejects(MediaAck{2, 0, {AckRange{2, 2}, AckRange{0, 1}}}));
  EXPECT_TRUE(rejects(MediaAck{2, 0, {AckRange{1, 2}, AckRange{0, 1}}}));
  EXPECT_TRUE(rejects(MediaAck{2, 0, {AckRange{0, 0}, AckRange{2, 2}}}));
  std::vector<AckRange> many;
  for (std::uint64_t index = 0; index < 33; ++index)
    many.push_back(AckRange{1000 - index * 10, 1000 - index * 10});
  EXPECT_TRUE(rejects(MediaAck{1000, 0, many}));
}

TEST_F(CongestionControl, AcceptsAWellFormedAckWithAGap) {
  send_many(controller, 3, 0);
  const auto accepted =
      controller.on_ack(MediaAck{2, 0, {AckRange{2, 2}, AckRange{0, 0}}}, 1000);
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted.value().newly_acked, 2U);
  EXPECT_EQ(controller.bytes_in_flight(), 1200U);
}

TEST_F(CongestionControl, ResetRestoresTheInitialState) {
  send_many(controller, 5, 0);
  ack(controller, 0, {AckRange{4, 4}}, 50000);
  controller.reset();
  EXPECT_EQ(controller.next_packet_number(), 0U);
  EXPECT_EQ(controller.bytes_in_flight(), 0U);
  EXPECT_FALSE(controller.has_rtt_sample());
  EXPECT_EQ(controller.smoothed_rtt_us(), 333000.0);
  EXPECT_FALSE(controller.loss_detection_deadline());
  EXPECT_EQ(controller.tracked_packets(), 0U);
}

}  // namespace
}  // namespace net_kakusan_test
