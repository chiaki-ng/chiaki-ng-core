#include "src/core/net/protocol/senkusha/bandwidth_test.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <variant>

namespace senkusha_test {
namespace {

using libchiaking::net::protocol::senkusha::BandwidthPolicy;
using libchiaking::net::protocol::senkusha::BandwidthStep;
using libchiaking::net::protocol::senkusha::BandwidthTest;
namespace step = libchiaking::net::protocol::senkusha::bandwidth_step;

class SenkushaBandwidthTest : public ::testing::Test {
 protected:
  static constexpr std::uint64_t second = 1'000'000;
  static constexpr std::uint64_t packet_bytes = 1405;

  static auto policy() -> BandwidthPolicy {
    BandwidthPolicy value;
    value.target_kbps = 60000;
    value.duration = std::chrono::milliseconds{1000};
    return value;
  }

  static auto sent(const BandwidthStep& value) -> std::optional<step::Send> {
    if (const auto* send = std::get_if<step::Send>(&value)) return *send;
    return std::nullopt;
  }

  static void deliver(BandwidthTest& test, std::uint64_t start,
                      std::uint64_t packets, std::uint64_t span) {
    for (std::uint64_t index = 0; index < packets; ++index)
      test.packet(start + span * index / (packets > 1 ? packets - 1 : 1),
                  packet_bytes);
  }

  static void pass_the_preliminary_round(BandwidthTest& test) {
    static_cast<void>(test.start(0));
    deliver(test, 1000, 86, second);
    static_cast<void>(test.reply(1, 86, second));
  }
};

TEST_F(SenkushaBandwidthTest, ACleanMainRoundReportsItsRate) {
  auto test = BandwidthTest::create(policy()).value();
  const auto first = sent(test.start(0));
  ASSERT_TRUE(first);
  EXPECT_EQ(first->round.id, 1U);
  EXPECT_EQ(first->round.kbps, 1000U);
  EXPECT_EQ(first->round.duration_ms, 1000U);
  EXPECT_EQ(first->round.fps, 30U);
  deliver(test, 1000, 86, second);
  const auto reply = test.reply(1, 86, second);
  ASSERT_TRUE(reply);
  const auto main = sent(*reply);
  ASSERT_TRUE(main);
  EXPECT_EQ(main->round.id, 2U);
  EXPECT_EQ(main->round.kbps, 60000U);
  deliver(test, second + 1000, 5000, second);
  const auto last = test.reply(2, 5000, 2 * second);
  ASSERT_TRUE(last);
  const auto* done = std::get_if<step::Finished>(&*last);
  ASSERT_NE(done, nullptr);
  ASSERT_TRUE(done->measurement);
  EXPECT_EQ(done->measurement->loss, 0.0);
  EXPECT_EQ(done->measurement->kbps, 60000U);
  EXPECT_EQ(done->measurement->requested_kbps, 60000U);
  EXPECT_EQ(test.phase(), BandwidthTest::Phase::finished);
}

TEST_F(SenkushaBandwidthTest, StepsDownAfterARestWhenLossIsHeavy) {
  auto test = BandwidthTest::create(policy()).value();
  pass_the_preliminary_round(test);
  deliver(test, second, 2000, second);
  const auto reply = test.reply(2, 5000, 2 * second);
  ASSERT_TRUE(reply);
  const auto* waiting = std::get_if<step::Wait>(&*reply);
  ASSERT_NE(waiting, nullptr);
  EXPECT_EQ(waiting->until_us, 2 * second + 500'000);
  EXPECT_FALSE(test.tick(2 * second + 1));
  const auto tick = test.tick(waiting->until_us);
  ASSERT_TRUE(tick);
  const auto retry = sent(*tick);
  ASSERT_TRUE(retry);
  EXPECT_EQ(retry->round.id, 3U);
  EXPECT_LT(retry->round.kbps, 59000U);
  EXPECT_GT(retry->round.kbps, 20000U);
  deliver(test, 3 * second, 1900, second);
  const auto last = test.reply(3, 1900, 4 * second);
  ASSERT_TRUE(last);
  const auto* done = std::get_if<step::Finished>(&*last);
  ASSERT_NE(done, nullptr);
  ASSERT_TRUE(done->measurement);
  EXPECT_EQ(done->measurement->requested_kbps, retry->round.kbps);
}

TEST_F(SenkushaBandwidthTest, GivesUpOnASilentPreliminaryRound) {
  auto test = BandwidthTest::create(policy()).value();
  static_cast<void>(test.start(0));
  EXPECT_EQ(test.deadline_us(), 2 * second);
  EXPECT_FALSE(test.tick(second));
  const auto last = test.tick(2 * second);
  ASSERT_TRUE(last);
  const auto* done = std::get_if<step::Finished>(&*last);
  ASSERT_NE(done, nullptr);
  EXPECT_FALSE(done->measurement);
}

TEST_F(SenkushaBandwidthTest, IgnoresStaleAndUnrelatedReplies) {
  auto test = BandwidthTest::create(policy()).value();
  static_cast<void>(test.start(0));
  EXPECT_FALSE(test.reply(7, 86, second));
  deliver(test, 1000, 86, second);
  static_cast<void>(test.reply(1, 86, second));
  EXPECT_FALSE(test.reply(1, 86, second));
}

TEST_F(SenkushaBandwidthTest, StopsEndlessLossAtTheRoundLimit) {
  auto value = policy();
  value.maximum_rounds = 3;
  auto test = BandwidthTest::create(value).value();
  pass_the_preliminary_round(test);
  deliver(test, second, 2000, second);
  const auto reply = test.reply(2, 5000, 2 * second);
  ASSERT_TRUE(reply);
  const auto* waiting = std::get_if<step::Wait>(&*reply);
  ASSERT_NE(waiting, nullptr);
  static_cast<void>(test.tick(waiting->until_us));
  deliver(test, 3 * second, 100, second);
  const auto last = test.reply(3, 5000, 4 * second);
  ASSERT_TRUE(last);
  const auto* done = std::get_if<step::Finished>(&*last);
  ASSERT_NE(done, nullptr);
  ASSERT_TRUE(done->measurement);
  EXPECT_GT(done->measurement->loss, 0.9);
  EXPECT_GT(done->measurement->kbps, 1000U);
  EXPECT_LT(done->measurement->kbps, 2000U);
}

TEST_F(SenkushaBandwidthTest, ReportsWhatArrivedWhenTheReplyIsMissing) {
  auto test = BandwidthTest::create(policy()).value();
  pass_the_preliminary_round(test);
  deliver(test, second, 2500, second);
  EXPECT_FALSE(test.tick(2 * second));
  const auto last = test.tick(3 * second);
  ASSERT_TRUE(last);
  const auto* done = std::get_if<step::Finished>(&*last);
  ASSERT_NE(done, nullptr);
  ASSERT_TRUE(done->measurement);
  EXPECT_GT(done->measurement->kbps, 25000U);
  EXPECT_LT(done->measurement->kbps, 30000U);
  EXPECT_EQ(done->measurement->requested_kbps, 60000U);
}

TEST_F(SenkushaBandwidthTest, ReportsWhatArrivedWhenTheReplyHasNoSentCount) {
  auto test = BandwidthTest::create(policy()).value();
  pass_the_preliminary_round(test);
  deliver(test, second, 2500, second);
  const auto last = test.reply(2, 0, 2 * second);
  ASSERT_TRUE(last);
  const auto* done = std::get_if<step::Finished>(&*last);
  ASSERT_NE(done, nullptr);
  ASSERT_TRUE(done->measurement);
  EXPECT_GT(done->measurement->kbps, 25000U);
  EXPECT_LT(done->measurement->kbps, 30000U);
  EXPECT_EQ(done->measurement->requested_kbps, 60000U);
}

TEST_F(SenkushaBandwidthTest, RefusesInvalidPolicies) {
  auto value = policy();
  value.target_kbps = 500;
  EXPECT_FALSE(BandwidthTest::create(value));
  value = policy();
  value.maximum_loss = 2.0;
  EXPECT_FALSE(BandwidthTest::create(value));
  value = policy();
  value.fps = 0;
  EXPECT_FALSE(BandwidthTest::create(value));
}

}  // namespace
}  // namespace senkusha_test
