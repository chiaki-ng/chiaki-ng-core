#include "src/core/net/protocol/remote_play/rudp_switch.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace remote_play_test {
namespace {

namespace sw = libchiaking::net::protocol::remote_play::transport_switch_detail;

class RemoteTransportSwitchState : public ::testing::Test {
 protected:
  static constexpr std::uint64_t a = 1;
  static constexpr std::uint64_t b = 2;
  sw::RemoteTransportSwitch s;
};

TEST_F(RemoteTransportSwitchState, IsReadyOnlyAfterTheAckOfItsOwnAttempt) {
  EXPECT_FALSE(s.begin(0));
  EXPECT_FALSE(s.on_ack(a));
  EXPECT_TRUE(s.begin(a));
  EXPECT_FALSE(s.begin(b));
  EXPECT_FALSE(s.take_ready());
  EXPECT_FALSE(s.on_ack(b));
  EXPECT_EQ(s.state(), sw::State::waiting);
  EXPECT_TRUE(s.on_ack(a));
  EXPECT_EQ(s.state(), sw::State::ready);
  EXPECT_FALSE(s.on_ack(b));
  EXPECT_FALSE(s.on_ack(a));
  EXPECT_EQ(s.stats().duplicate_acks, 1U);
  EXPECT_EQ(s.stats().stale_acks, 3U);
  EXPECT_TRUE(s.take_ready());
  EXPECT_FALSE(s.take_ready());
  EXPECT_FALSE(s.on_ack(a));
  EXPECT_TRUE(s.begin(b));
  EXPECT_FALSE(s.on_ack(a));
  EXPECT_TRUE(s.on_ack(b));
  EXPECT_TRUE(s.take_ready());
}

TEST_F(RemoteTransportSwitchState, ATimedOutAttemptIsRetiredForANewOne) {
  EXPECT_TRUE(s.begin(a));
  EXPECT_FALSE(s.timeout(b));
  EXPECT_TRUE(s.timeout(a));
  EXPECT_FALSE(s.on_ack(a));
  EXPECT_FALSE(s.timeout(a));
  EXPECT_FALSE(s.take_ready());
  EXPECT_FALSE(s.begin(a));
  EXPECT_TRUE(s.begin(b));
  EXPECT_FALSE(s.on_ack(a));
  EXPECT_TRUE(s.on_ack(b));
}

TEST_F(RemoteTransportSwitchState, ACancelledAttemptIsRetiredForANewOne) {
  EXPECT_TRUE(s.begin(a));
  EXPECT_FALSE(s.cancel(b));
  EXPECT_TRUE(s.cancel(a));
  EXPECT_FALSE(s.on_ack(a));
  EXPECT_FALSE(s.cancel(a));
  EXPECT_FALSE(s.begin(a));
  EXPECT_TRUE(s.begin(b));
  EXPECT_FALSE(s.on_ack(a));
  EXPECT_TRUE(s.on_ack(b));
  EXPECT_FALSE(s.cancel(b));
  EXPECT_TRUE(s.take_ready());
  EXPECT_FALSE(s.begin(a));
  EXPECT_TRUE(s.begin(3));
  EXPECT_FALSE(s.on_ack(a));
  EXPECT_TRUE(s.on_ack(3));
  EXPECT_TRUE(s.take_ready());
}

}  // namespace
}  // namespace remote_play_test
