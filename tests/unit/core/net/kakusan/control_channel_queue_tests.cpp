#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/kakusan/control_channel_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <variant>

namespace net_kakusan_test {
namespace {

namespace control = libchiaking::net::kakusan::control;

class ControlChannelQueue : public ControlChannelTest {};

TEST_F(ControlChannelQueue, HoldsFortyMessages) {
  ControlChannel channel{start};
  for (std::uint32_t index = 1; index <= 40; ++index)
    static_cast<void>(enqueued(channel, active_with(index), start));
  const auto overflow = channel.enqueue(active_with(41), start);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), Error::protocol);
  EXPECT_FALSE(channel.enqueue(ping_with(1), start));
  EXPECT_FALSE(channel.enqueue(control::SourceIdr{}, start));
}

TEST_F(ControlChannelQueue, CountsTheMessageInFlight) {
  Pair pair{start};
  for (std::uint32_t index = 1; index <= 40; ++index)
    static_cast<void>(enqueued(pair.host, active_with(index), start));
  EXPECT_EQ(polled(pair.host, start).size(), 1U);
  const auto overflow = pair.host.enqueue(active_with(41), start);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), Error::protocol);
  EXPECT_FALSE(pair.host.enqueue(control::Hello{}, start));
}

TEST_F(ControlChannelQueue, FreesASlotWhenAMessageIsAcked) {
  Pair pair{start};
  for (std::uint32_t index = 1; index <= 40; ++index)
    static_cast<void>(enqueued(pair.host, active_with(index), start));
  const auto records = polled(pair.host, start);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_TRUE(received(pair.guest, records[0], start));
  const auto acks = polled(pair.guest, start);
  ASSERT_EQ(acks.size(), 1U);
  EXPECT_FALSE(received(pair.host, acks[0], start));
  EXPECT_TRUE(pair.host.enqueue(active_with(41), start));
  EXPECT_FALSE(pair.host.enqueue(active_with(42), start));
}

TEST_F(ControlChannelQueue, CoalescesPendingPingsAndIdrRequests) {
  ControlChannel channel{start};
  const auto blocker = enqueued(channel, control::Hello{}, start);
  const auto ping_one = enqueued(channel, ping_with(1), start);
  const auto idr_one = enqueued(channel, control::SourceIdr{}, start);
  const auto ping_two = enqueued(channel, ping_with(2), start);
  const auto idr_two = enqueued(channel, control::SourceIdr{}, start);
  const auto ready = enqueued(channel, control::AdmissionPending{}, start);
  EXPECT_EQ(ping_one, ping_two);
  EXPECT_EQ(idr_one, idr_two);
  EXPECT_NE(ping_one, idr_one);
  EXPECT_NE(blocker, ping_one);
  EXPECT_NE(ready, ping_one);
}

TEST_F(ControlChannelQueue, DeliversOnlyTheLatestCoalescedPing) {
  Pair pair{start};
  static_cast<void>(enqueued(pair.host, control::Hello{}, start));
  static_cast<void>(enqueued(pair.host, ping_with(1), start));
  static_cast<void>(enqueued(pair.host, control::SourceIdr{}, start));
  static_cast<void>(enqueued(pair.host, ping_with(2), start));
  static_cast<void>(enqueued(pair.host, control::SourceIdr{}, start));
  static_cast<void>(enqueued(pair.host, ping_with(3), start));
  pair.settle(start);
  ASSERT_EQ(pair.at_guest.size(), 3U);
  EXPECT_TRUE(std::holds_alternative<control::Hello>(pair.at_guest[0]));
  ASSERT_TRUE(std::holds_alternative<control::Ping>(pair.at_guest[1]));
  EXPECT_EQ(std::get<control::Ping>(pair.at_guest[1]).nonce,
            ping_with(3).nonce);
  EXPECT_TRUE(std::holds_alternative<control::SourceIdr>(pair.at_guest[2]));
}

TEST_F(ControlChannelQueue, NeverCoalescesWithAMessageAlreadySent) {
  ControlChannel channel{start};
  const auto first = enqueued(channel, ping_with(1), start);
  EXPECT_EQ(polled(channel, start).size(), 1U);
  const auto second = enqueued(channel, ping_with(2), start);
  const auto third = enqueued(channel, ping_with(3), start);
  EXPECT_NE(first, second);
  EXPECT_EQ(second, third);
}

TEST_F(ControlChannelQueue, CoalescesEvenWhenFull) {
  ControlChannel channel{start};
  for (std::uint32_t index = 1; index <= 39; ++index)
    static_cast<void>(enqueued(channel, active_with(index), start));
  const auto ping = enqueued(channel, ping_with(1), start);
  EXPECT_FALSE(channel.enqueue(control::SourceIdr{}, start));
  const auto again = enqueued(channel, ping_with(2), start);
  EXPECT_EQ(again, ping);
}

}  // namespace
}  // namespace net_kakusan_test
