#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/kakusan/control_channel_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <variant>

namespace net_kakusan_test {
namespace {

namespace control = libchiaking::net::kakusan::control;

class ControlChannelTerminal : public ControlChannelTest {};

TEST_F(ControlChannelTerminal, ReplacesTheQueueWhenNothingIsOutstanding) {
  ControlChannel channel{start};
  static_cast<void>(enqueued(channel, control::Hello{}, start));
  static_cast<void>(enqueued(channel, control::AdmissionPending{}, start));
  EXPECT_TRUE(channel.send_terminal(control::Failure{Error::protocol}, start));
  const auto records = polled(channel, start);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(decoded(records[0]).message_id, 1U);
  EXPECT_FALSE(channel.send_terminal(control::End{}, start));
  EXPECT_FALSE(channel.enqueue(control::Hello{}, start));
  EXPECT_FALSE(received(channel, ack_record(1), start));
  EXPECT_FALSE(channel.take_acknowledged());
  EXPECT_TRUE(channel.idle());
  EXPECT_TRUE(polled(channel, start + 20 * sec).empty());
}

TEST_F(ControlChannelTerminal, TakesTheIdAfterTheLastAckedMessage) {
  Pair pair{start};
  static_cast<void>(enqueued(pair.host, control::Hello{}, start));
  pair.settle(start);
  EXPECT_EQ(pair.at_guest.size(), 1U);
  EXPECT_TRUE(
      pair.host.send_terminal(control::End{control::Reason::capacity}, start));
  pair.settle(start);
  ASSERT_EQ(pair.at_guest.size(), 2U);
  ASSERT_TRUE(std::holds_alternative<control::End>(pair.at_guest[1]));
  EXPECT_EQ(std::get<control::End>(pair.at_guest[1]).reason,
            control::Reason::capacity);
  EXPECT_TRUE(pair.host.idle());
}

TEST_F(ControlChannelTerminal, WaitsBehindTheOutstandingMessage) {
  ControlChannel channel{start};
  static_cast<void>(enqueued(channel, control::Hello{}, start));
  static_cast<void>(enqueued(channel, control::AdmissionPending{}, start));
  EXPECT_EQ(polled(channel, start).size(), 1U);
  EXPECT_TRUE(
      channel.send_terminal(control::End{control::Reason::source}, start));
  EXPECT_FALSE(channel.send_terminal(control::End{}, start));
  EXPECT_FALSE(channel.enqueue(control::AdmissionPending{}, start));
  EXPECT_TRUE(polled(channel, start).empty());
  EXPECT_FALSE(channel.idle());
  EXPECT_FALSE(received(channel, ack_record(1), start));
  const auto records = polled(channel, start);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(decoded(records[0]).message_id, 2U);
  EXPECT_FALSE(received(channel, ack_record(2), start));
  EXPECT_TRUE(channel.idle());
  EXPECT_TRUE(polled(channel, start + 20 * sec).empty());
}

TEST_F(ControlChannelTerminal, ReachesThePeerAfterTheOutstandingMessage) {
  Pair pair{start};
  static_cast<void>(enqueued(pair.host, control::Hello{}, start));
  const auto held = polled(pair.host, start);
  ASSERT_EQ(held.size(), 1U);
  EXPECT_TRUE(
      pair.host.send_terminal(control::End{control::Reason::source}, start));
  if (auto message = received(pair.guest, held[0], start))
    pair.at_guest.push_back(std::move(*message));
  pair.settle(start);
  ASSERT_EQ(pair.at_guest.size(), 2U);
  EXPECT_TRUE(std::holds_alternative<control::Hello>(pair.at_guest[0]));
  ASSERT_TRUE(std::holds_alternative<control::End>(pair.at_guest[1]));
  EXPECT_EQ(std::get<control::End>(pair.at_guest[1]).reason,
            control::Reason::source);
  EXPECT_TRUE(pair.host.idle());
}

TEST_F(ControlChannelTerminal, DropsTheQueueButStillAcksThePeer) {
  Pair pair{start};
  static_cast<void>(enqueued(pair.guest, control::Hello{}, start));
  pair.settle(start);
  EXPECT_EQ(pair.at_host.size(), 1U);

  for (std::uint32_t index = 1; index <= 5; ++index)
    static_cast<void>(enqueued(pair.host, active_with(index), start));
  EXPECT_TRUE(
      pair.host.send_terminal(control::Failure{Error::protocol}, start));
  pair.settle(start);
  ASSERT_EQ(pair.at_guest.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<control::Failure>(pair.at_guest[0]));

  static_cast<void>(enqueued(pair.guest, control::AdmissionPending{}, start));
  pair.settle(start);
  EXPECT_EQ(pair.at_host.size(), 2U);
  EXPECT_TRUE(pair.guest.idle());

  const auto duplicate = fragments_of(2, control::AdmissionPending{})[0];
  EXPECT_FALSE(received(pair.host, duplicate, start));
  const auto reack = polled(pair.host, start);
  ASSERT_EQ(reack.size(), 1U);
  EXPECT_EQ(decoded(reack[0]).kind, ControlKind::ack);
  EXPECT_EQ(decoded(reack[0]).message_id, 2U);
}

TEST_F(ControlChannelTerminal, ObeysTheSendRateLimit) {
  ControlChannel channel{start};
  for (std::uint32_t index = 1; index <= 40; ++index) {
    static_cast<void>(enqueued(channel, active_with(index), start));
    EXPECT_EQ(polled(channel, start).size(), 1U);
    EXPECT_FALSE(received(channel, ack_record(index), start));
  }
  EXPECT_TRUE(channel.send_terminal(control::End{}, start));
  EXPECT_TRUE(polled(channel, start).empty());
  EXPECT_EQ(channel.next_deadline_us(), start + 50 * ms);
  EXPECT_EQ(polled(channel, start + 50 * ms).size(), 1U);
}

}  // namespace
}  // namespace net_kakusan_test
