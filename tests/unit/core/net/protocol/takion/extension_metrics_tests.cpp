#include "src/core/net/protocol/takion/extension_metrics.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

struct NativeSample final {
  std::uint64_t elapsed_us;
  std::uint32_t remote_us;
  std::uint32_t counter;
  std::int32_t delta_us;
};

constexpr std::array<NativeSample, 5> native_receiver_samples{{
    {0, 0, 0, 0},
    {1, 2, 1, -1},
    {0xffffffff, 0xfffffffe, 0xffffffff, 2},
    {0x100000001, 2, 0, -2},
    {0x180000001, 2, 1, -2147483647 - 1},
}};

TEST(ExtensionReceiveMetrics, MeasuresTransitDeltasLikeTheNativeReceiver) {
  takion::ExtensionReceiveMetrics metrics;
  for (const auto& sample : native_receiver_samples) {
    SCOPED_TRACE(sample.counter);
    metrics.observe(sample.remote_us, sample.counter, sample.elapsed_us);
    EXPECT_EQ(metrics.last_transit_delta_us(), sample.delta_us);
  }
  static_assert(takion::ExtensionReceiveMetrics::transit_delta(7, 3, 0, 0) ==
                4);
  static_assert(takion::ExtensionReceiveMetrics::transit_delta(
                    5, 0xfffffff0, 0xfffffff0, 0xffffffe0) == 5);
}

TEST(ExtensionReceiveMetrics, SkipsTheFirstDeltaAndFollowsRfc3550Jitter) {
  takion::ExtensionReceiveMetrics metrics;
  metrics.observe(1000, 0, 5000);
  EXPECT_EQ(metrics.last_transit_delta_us(), 4000U);
  EXPECT_EQ(metrics.jitter_us(), 0U);
  metrics.observe(1000 + 16666, 1, 5000 + 16666 + 1600);
  EXPECT_EQ(metrics.last_transit_delta_us(), 1600U);
  EXPECT_EQ(metrics.jitter_us(), 100U);
  metrics.observe(1000 + 2 * 16666, 2, 5000 + 2 * 16666);
  EXPECT_EQ(metrics.last_transit_delta_us(), -1600);
  EXPECT_EQ(metrics.jitter_us(), 193U);
}

TEST(ExtensionReceiveMetrics, HasNoJitterForASteadyStream) {
  takion::ExtensionReceiveMetrics steady;
  for (std::uint32_t index = 0; index < 100; ++index)
    steady.observe(index * 16666U, index,
                   20'000 + std::uint64_t{index} * 16666U);
  EXPECT_EQ(steady.jitter_us(), 0U);
}

TEST(ExtensionReceiveMetrics, ConvergesOnTheJitterOfAnAlternatingDelay) {
  takion::ExtensionReceiveMetrics alternating;
  for (std::uint32_t index = 0; index < 400; ++index)
    alternating.observe(
        index * 16666U, index,
        std::uint64_t{index} * 16666U + (index % 2 == 0 ? 0U : 1000U));
  EXPECT_GE(alternating.jitter_us(), 990U);
  EXPECT_LE(alternating.jitter_us(), 1000U);
}

TEST(ExtensionReceiveMetrics, TakesJitterFromAbsoluteSignedDeltas) {
  takion::ExtensionReceiveMetrics metrics;
  for (const auto& sample : native_receiver_samples)
    metrics.observe(sample.remote_us, sample.counter, sample.elapsed_us);
  EXPECT_EQ(metrics.jitter_us(), 2147483653U >> 4U);
}

TEST(ExtensionReceiveMetrics, CountsUntimedObservationsWithoutJitter) {
  takion::ExtensionReceiveMetrics metrics;
  metrics.observe_untimed(10, 0);
  metrics.observe_untimed(11, 500'000);
  metrics.observe(100'000, 12, 600'000);
  EXPECT_EQ(metrics.jitter_us(), 0U);
  EXPECT_EQ(metrics.received(), 3U);
  EXPECT_EQ(metrics.lost(), 0U);
  metrics.observe(100'000 + 16666, 13, 600'000 + 16666 + 320);
  EXPECT_EQ(metrics.jitter_us(), 20U);
}

TEST(ExtensionReceiveMetrics, CountsCounterGapsAsLoss) {
  takion::ExtensionReceiveMetrics metrics;
  EXPECT_EQ(metrics.expected(), 0U);
  EXPECT_EQ(metrics.lost(), 0U);
  for (const std::uint32_t counter : {0U, 1U, 2U, 4U, 5U, 6U, 8U, 9U})
    metrics.observe(counter * 1000U, counter, std::uint64_t{counter} * 1000U);
  EXPECT_EQ(metrics.received(), 8U);
  EXPECT_EQ(metrics.expected(), 10U);
  EXPECT_EQ(metrics.lost(), 2U);
  EXPECT_EQ(metrics.loss_ratio(), 0.0);
}

TEST(ExtensionReceiveMetrics, DoesNotCountReorderingOrDuplicatesAsLoss) {
  takion::ExtensionReceiveMetrics metrics;
  for (const std::uint32_t counter : {0U, 2U, 1U, 3U, 3U})
    metrics.observe_untimed(counter, 0);
  EXPECT_EQ(metrics.expected(), 4U);
  EXPECT_EQ(metrics.received(), 5U);
  EXPECT_EQ(metrics.lost(), 0U);
}

TEST(ExtensionReceiveMetrics, ExtendsTheSequenceAcrossACounterWrap) {
  takion::ExtensionReceiveMetrics metrics;
  for (const std::uint32_t counter : {0xfffffffeU, 0xffffffffU, 1U, 2U})
    metrics.observe_untimed(counter, 0);
  EXPECT_EQ(metrics.expected(), 5U);
  EXPECT_EQ(metrics.received(), 4U);
  EXPECT_EQ(metrics.lost(), 1U);
}

TEST(ExtensionReceiveMetrics, ReportsTheLossRatioPerOneSecondInterval) {
  takion::ExtensionReceiveMetrics metrics;
  std::uint32_t counter = 100;
  for (std::uint64_t now = 0; now < 1'000'000; now += 10'000) {
    if (counter % 10 != 3) metrics.observe_untimed(counter, now);
    ++counter;
  }
  EXPECT_EQ(metrics.loss_ratio(), 0.0);
  metrics.observe_untimed(counter, 1'000'000);
  EXPECT_EQ(metrics.expected(), 101U);
  EXPECT_EQ(metrics.lost(), 10U);
  EXPECT_EQ(metrics.loss_ratio(), 10.0 / 101.0);
  ++counter;
  for (std::uint64_t now = 1'010'000; now <= 2'000'000; now += 10'000)
    metrics.observe_untimed(counter++, now);
  EXPECT_EQ(metrics.loss_ratio(), 0.0);
  EXPECT_EQ(metrics.lost(), 10U);
  counter += 25;
  metrics.observe_untimed(counter++, 2'500'000);
  for (std::uint64_t now = 2'510'000; now < 3'000'000; now += 10'000)
    metrics.observe_untimed(counter++, now);
  metrics.observe_untimed(counter++, 3'000'000);
  EXPECT_EQ(metrics.lost(), 35U);
  EXPECT_EQ(metrics.loss_ratio(), 25.0 / 76.0);
}

}  // namespace
}  // namespace takion_test
