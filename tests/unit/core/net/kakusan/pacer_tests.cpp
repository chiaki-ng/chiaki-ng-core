#include "src/core/net/kakusan/congestion.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace net_kakusan_test {
namespace {

using libchiaking::net::kakusan::Pacer;
using libchiaking::net::kakusan::pacer_initial_rate;

class PacerTest : public ::testing::Test {
 protected:
  static auto tolerance(double expected) -> double {
    return 1e-6 * std::max(1.0, std::abs(expected));
  }

  void SetUp() override { pacer.set_rate(pacer_initial_rate, 0); }

  Pacer pacer{0};
};

TEST(Pacer, StartsIdle) {
  Pacer pacer{0};
  EXPECT_TRUE(pacer.idle());
}

TEST_F(PacerTest, HoldsOnePacketOfTokensWhileIdle) {
  EXPECT_EQ(pacer.tokens(10000000), 1200.0);
  pacer.set_idle(false, 0);
  EXPECT_EQ(pacer.tokens(0), 1200.0);
  EXPECT_TRUE(pacer.can_send(1200, 0));
  EXPECT_FALSE(pacer.can_send(1201, 0));
}

TEST_F(PacerTest, RefillsAtItsRateAfterSending) {
  pacer.set_idle(false, 0);
  pacer.on_sent(1200, 0);
  EXPECT_FALSE(pacer.can_send(1, 0));
  const auto next = pacer.next_send_time(1200);
  EXPECT_TRUE(next == 33300 || next == 33301) << next;
  EXPECT_FALSE(pacer.can_send(1200, 33299));
  EXPECT_TRUE(pacer.can_send(1200, next));
  EXPECT_NEAR(pacer.tokens(16650), 600.0, tolerance(600.0));
}

TEST_F(PacerTest, CapsABusyBucketAtTwoPacketsAndLetsItGoNegative) {
  pacer.set_idle(false, 0);
  pacer.on_sent(1200, 0);
  EXPECT_EQ(pacer.tokens(10000000), 2400.0);
  pacer.on_sent(1200, 10000000);
  pacer.on_sent(1200, 10000000);
  EXPECT_FALSE(pacer.can_send(76, 10000000));
  EXPECT_EQ(pacer.tokens(10000000), 0.0);

  pacer.on_sent(76, 10000000);
  EXPECT_EQ(pacer.tokens(10000000), -76.0);
  const auto repaid = pacer.next_send_time(1200);
  EXPECT_TRUE(repaid == 10000000 + 35409 || repaid == 10000000 + 35410)
      << repaid;
}

TEST_F(PacerTest, CapsAnIdleBucketBelowTheBusyCap) {
  pacer.set_idle(false, 0);
  pacer.on_sent(1200, 0);
  pacer.on_sent(1200, 10000000);
  pacer.on_sent(1200, 10000000);
  pacer.on_sent(76, 10000000);
  pacer.set_idle(true, 20000000);
  EXPECT_EQ(pacer.tokens(20000000), 2400.0);
  EXPECT_EQ(pacer.tokens(30000000), 2400.0);
  pacer.on_sent(2300, 30000000);
  EXPECT_NEAR(pacer.tokens(30000000), 100.0, tolerance(100.0));
  EXPECT_EQ(pacer.tokens(40000000), 1200.0);
  pacer.set_idle(false, 40000000);
  EXPECT_EQ(pacer.tokens(50000000), 2400.0);
  pacer.set_idle(true, 50000000);
  EXPECT_EQ(pacer.tokens(60000000), 2400.0);
  pacer.on_sent(600, 60000000);
  EXPECT_EQ(pacer.tokens(70000000), 1800.0);
}

TEST(Pacer, AccruesEachRateOnlyForTheTimeItHeld) {
  Pacer switching{0};
  switching.set_idle(false, 0);
  switching.on_sent(1200, 0);
  switching.set_rate(0.01, 0);
  switching.set_rate(0.1, 1000);
  EXPECT_NEAR(switching.tokens(1000), 10.0, 1e-5);
  EXPECT_NEAR(switching.tokens(2000), 110.0, 110e-6);
  switching.set_rate(1.0, 2000);
  EXPECT_EQ(switching.tokens(100000), 10000.0);
}

}  // namespace
}  // namespace net_kakusan_test
