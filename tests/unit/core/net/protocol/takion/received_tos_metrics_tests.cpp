#include "src/core/net/protocol/takion/received_tos_metrics.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

constexpr std::uint8_t video_kind = 0x02;

TEST(ReceivedTosMetrics, PacksDscpCountsLikeTheMetricsReference) {
  std::array<std::uint32_t, takion::received_tos_dscp_count> counts{};
  counts[0] = 5;
  counts[10] = 1U << 24U;
  counts[46] = 0x1234567;
  counts[63] = 1;
  EXPECT_EQ(takion::ReceivedTosMetrics::pack_dscp_counts(counts),
            (std::vector<std::uint32_t>{5, 167772160, 774063463, 1056964609}));
  EXPECT_TRUE(takion::ReceivedTosMetrics::pack_dscp_counts({}).empty());
}

TEST(ReceivedTosMetrics, ReportsOnlyOnTheIntervalByDefault) {
  takion::ReceivedTosMetrics metrics;
  EXPECT_EQ(metrics.interval_ms(), 1000U);
  EXPECT_EQ(metrics.packet_threshold(), 0U);
  EXPECT_FALSE(metrics.account(video_kind, 0xb8, 0));
  metrics.start(1'000'000);
  for (std::uint64_t offset = 0; offset < 999'000; offset += 100'000) {
    SCOPED_TRACE(::testing::PrintToString(offset));
    EXPECT_FALSE(metrics.account(video_kind, 0xbb, 1'000'000 + offset));
  }
  EXPECT_EQ(metrics.trigger_count(), 10U);
  const auto report = metrics.account(video_kind, 0x01, 2'000'000);
  ASSERT_TRUE(report);
  EXPECT_EQ(report->time_offset_msec, 1000U);
  EXPECT_EQ(report->dscp_counts,
            (std::vector<std::uint32_t>{(0U << 24U) | 1U, (46U << 24U) | 10U}));
  EXPECT_EQ(report->ecn_ect1_count, 1U);
  EXPECT_EQ(report->ecn_ect0_count, 0U);
  EXPECT_EQ(report->ecn_ce_count, 10U);
  EXPECT_TRUE(report->opposite_sent_dscp_counts.empty());
  EXPECT_EQ(metrics.trigger_count(), 0U);
  for (const auto count : metrics.dscp_counts()) {
    EXPECT_EQ(count, 0U);
  }
  EXPECT_FALSE(metrics.account(video_kind, 0, 2'500'000));
  const auto next = metrics.account(video_kind, 0, 3'000'000);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->time_offset_msec, 1000U);
  EXPECT_EQ(next->dscp_counts, (std::vector<std::uint32_t>{2U}));
}

TEST(ReceivedTosMetrics, DoesNotCountAnExcludedKind) {
  takion::ReceivedTosMetrics metrics;
  metrics.start(0);
  metrics.configure(1, std::nullopt);
  EXPECT_FALSE(metrics.account(0x1f, 0x03, 10));
  EXPECT_FALSE(metrics.account(0x0f, 0x03, 10));
  EXPECT_EQ(metrics.dscp_counts()[0], 0U);
  EXPECT_TRUE(metrics.account(0x00, 0x03, 10));
}

TEST(ReceivedTosMetrics, ReportsTheFirstMarkedPacketWithAThresholdOfOne) {
  takion::ReceivedTosMetrics one;
  one.start(0);
  one.configure(1, std::nullopt);
  EXPECT_FALSE(one.account(video_kind, 0x00, 1000));
  const auto first_marked = one.account(video_kind, 0x02, 2000);
  ASSERT_TRUE(first_marked);
  EXPECT_EQ(first_marked->time_offset_msec, 2U);
  EXPECT_EQ(first_marked->dscp_counts, (std::vector<std::uint32_t>{2U}));
  EXPECT_EQ(first_marked->ecn_ect0_count, 1U);
}

TEST(ReceivedTosMetrics, ReportsOnTheThirdMarkedPacketWithAThresholdOfThree) {
  takion::ReceivedTosMetrics three;
  three.start(0);
  three.configure(3, std::nullopt);
  EXPECT_FALSE(three.account(video_kind, 0x01, 1));
  EXPECT_FALSE(three.account(video_kind, 0x00, 2));
  EXPECT_FALSE(three.account(video_kind, 0x03, 3));
  EXPECT_TRUE(three.account(video_kind, 0x02, 4));
}

TEST(ReceivedTosMetrics, ReportsEveryPacketWithAZeroInterval) {
  takion::ReceivedTosMetrics every;
  every.start(0);
  every.configure(std::nullopt, 0);
  EXPECT_EQ(every.interval_ms(), 0U);
  const auto immediate = every.account(video_kind, 0x00, 0);
  ASSERT_TRUE(immediate);
  EXPECT_EQ(immediate->time_offset_msec, 0U);
  EXPECT_TRUE(every.account(video_kind, 0x00, 0));
  EXPECT_EQ(every.trigger_count(), 0U);
}

TEST(ReceivedTosMetrics, ReportsImmediatelyWhenThresholdAndIntervalAreZero) {
  takion::ReceivedTosMetrics both;
  both.start(0);
  both.configure(0, 0);
  EXPECT_TRUE(both.account(video_kind, 0x03, 0));
}

TEST(ReceivedTosMetrics, KeepsItsSettingsWhenConfiguredWithoutValues) {
  takion::ReceivedTosMetrics preserved;
  preserved.configure(5, 250);
  preserved.configure(std::nullopt, std::nullopt);
  EXPECT_EQ(preserved.packet_threshold(), 5U);
  EXPECT_EQ(preserved.interval_ms(), 250U);
  preserved.configure(0, std::nullopt);
  EXPECT_EQ(preserved.packet_threshold(), 0U);
  EXPECT_EQ(preserved.interval_ms(), 250U);
}

TEST(ReceivedTosMetrics, PutsTheHighestDscpInTheTopEntry) {
  takion::ReceivedTosMetrics metrics;
  metrics.start(0);
  metrics.configure(std::nullopt, 0xffffffffU);
  for (std::uint32_t index = 0; index < 4; ++index) {
    SCOPED_TRACE(::testing::PrintToString(index));
    EXPECT_FALSE(
        metrics.account(video_kind, static_cast<std::uint8_t>(63U << 2U), 0));
  }
  metrics.configure(std::nullopt, 0);
  const auto report =
      metrics.account(video_kind, static_cast<std::uint8_t>(63U << 2U), 0);
  ASSERT_TRUE(report);
  EXPECT_EQ(report->dscp_counts,
            (std::vector<std::uint32_t>{(63U << 24U) | 5U}));
}

TEST(ReceivedTosMetrics, KeepsCumulativeCountsWhenAReportClearsTheInterval) {
  takion::ReceivedTosMetrics metrics;
  EXPECT_FALSE(metrics.last_traffic_class());
  EXPECT_EQ(metrics.total_packets(), 0U);
  metrics.start(0);
  EXPECT_FALSE(metrics.account(0x0f, 0xb9, 10));
  EXPECT_FALSE(metrics.last_traffic_class());
  EXPECT_EQ(metrics.total_packets(), 0U);
  EXPECT_FALSE(metrics.account(video_kind, 0xb9, 100'000));
  EXPECT_FALSE(metrics.account(video_kind, 0xbb, 200'000));
  EXPECT_TRUE(metrics.account(video_kind, 0x02, 1'000'000));
  EXPECT_EQ(metrics.reports(), 1U);
  for (const auto count : metrics.dscp_counts()) {
    EXPECT_EQ(count, 0U);
  }
  EXPECT_FALSE(metrics.account(video_kind, 0xb8, 1'100'000));
  EXPECT_EQ(metrics.total_packets(), 4U);
  EXPECT_EQ(metrics.total_dscp_counts()[46], 3U);
  EXPECT_EQ(metrics.total_dscp_counts()[0], 1U);
  EXPECT_EQ(metrics.total_ecn_counts(),
            (std::array<std::uint64_t, 4>{1, 1, 1, 1}));
  EXPECT_EQ(metrics.last_traffic_class(), std::uint8_t{0xb8});
  EXPECT_EQ(metrics.reports(), 1U);
}

}  // namespace
}  // namespace takion_test
