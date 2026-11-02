#include "src/core/net/protocol/senkusha/rtt.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace senkusha_test {
namespace {

namespace senkusha = libchiaking::net::protocol::senkusha;
using namespace std::chrono_literals;

static_assert(senkusha::RttSampler::pong_received_at(1000, 1300, 6300) == 1300);
static_assert(senkusha::RttSampler::pong_received_at(1000, std::nullopt,
                                                     6300) == 6300);
static_assert(senkusha::RttSampler::pong_received_at(1000, 999, 6300) == 6300);
static_assert(senkusha::RttSampler::pong_received_at(1000, 1000, 6300) == 1000);
static_assert(senkusha::RttSampler::pong_received_at(1000, 6300, 6300) == 6300);
static_assert(senkusha::RttSampler::pong_received_at(1000, 7000, 6300) == 6300);

class RttSummary : public ::testing::Test {
 protected:
  const std::vector<std::chrono::microseconds> samples{1000us, 2000us, 3000us};
  const std::chrono::microseconds maximum{
      std::numeric_limits<std::int64_t>::max()};
};

TEST_F(RttSummary, CountsLostPingsAndDescribesTheSamples) {
  const auto summary = senkusha::RttSampler::summarize(samples, 4, {});
  ASSERT_TRUE(summary);
  EXPECT_EQ(summary.value().attempted, 4U);
  EXPECT_EQ(summary.value().successful, 3U);
  EXPECT_EQ(summary.value().lost, 1U);
  EXPECT_EQ(summary.value().raw_mean, 2000us);
  EXPECT_EQ(summary.value().minimum, 1000us);
  EXPECT_EQ(summary.value().maximum, 3000us);
  EXPECT_LT(
      std::abs(summary.value().variance_us_squared - (2'000'000.0L / 3.0L)),
      0.001L);
  EXPECT_EQ(summary.value().adjusted_mean, 2000us);
}

TEST_F(RttSummary, SubtractsTheAdjustmentFromTheMean) {
  const auto adjusted = senkusha::RttSampler::summarize(
      samples, 3, senkusha::RttAdjustmentPolicy{500us, 100us});
  ASSERT_TRUE(adjusted);
  EXPECT_EQ(adjusted.value().adjusted_mean, 1500us);
}

TEST_F(RttSummary, NeverAdjustsTheMeanBelowItsFloor) {
  const std::vector low_samples{300us, 500us};
  const auto floored = senkusha::RttSampler::summarize(
      low_samples, 2, senkusha::RttAdjustmentPolicy{500us, 100us});
  ASSERT_TRUE(floored);
  EXPECT_EQ(floored.value().adjusted_mean, 100us);
}

TEST_F(RttSummary, OfOneSampleHasNoVariance) {
  const std::vector one_sample{123us};
  const auto one = senkusha::RttSampler::summarize(one_sample, 1, {});
  ASSERT_TRUE(one);
  EXPECT_EQ(one.value().variance_us_squared, 0.0L);
  EXPECT_EQ(one.value().minimum, 123us);
  EXPECT_EQ(one.value().maximum, 123us);
}

TEST_F(RttSummary, NeedsAtLeastOneSample) {
  const auto empty = senkusha::RttSampler::summarize({}, 1, {});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), senkusha::Error::no_rtt_samples);
}

TEST_F(RttSummary, RefusesMoreSamplesThanAttempts) {
  EXPECT_FALSE(senkusha::RttSampler::summarize(samples, 2, {}));
}

TEST_F(RttSummary, RefusesANegativeSample) {
  const std::vector negative{-1us};
  EXPECT_FALSE(senkusha::RttSampler::summarize(negative, 1, {}));
}

TEST_F(RttSummary, ReportsAnOverflowingSum) {
  const std::vector overflowing{maximum, maximum, maximum};
  const auto overflow = senkusha::RttSampler::summarize(overflowing, 3, {});
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), senkusha::Error::arithmetic_overflow);
}

class RttMtuTimeout : public ::testing::Test {
 protected:
  senkusha::MtuPolicy policy;
};

TEST_F(RttMtuTimeout, IsAMultipleOfTheRoundTripWithinItsBounds) {
  EXPECT_EQ(senkusha::RttSampler::mtu_timeout(100us, policy).value(), 5ms);
  EXPECT_EQ(senkusha::RttSampler::mtu_timeout(50'000us, policy).value(), 250ms);
  EXPECT_EQ(senkusha::RttSampler::mtu_timeout(10'000'000us, policy).value(),
            500ms);
}

TEST_F(RttMtuTimeout, RefusesANegativeRoundTrip) {
  EXPECT_FALSE(senkusha::RttSampler::mtu_timeout(-1us, policy));
}

TEST_F(RttMtuTimeout, ReportsAnOverflowingMultiple) {
  policy.timeout_multiplier = std::numeric_limits<std::uint32_t>::max();
  const auto overflow = senkusha::RttSampler::mtu_timeout(
      std::chrono::microseconds{std::numeric_limits<std::int64_t>::max()},
      policy);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), senkusha::Error::arithmetic_overflow);
}

}  // namespace
}  // namespace senkusha_test
