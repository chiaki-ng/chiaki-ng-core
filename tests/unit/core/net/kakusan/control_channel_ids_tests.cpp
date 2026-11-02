#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/kakusan/control_channel_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace net_kakusan_test {
namespace {

namespace control = libchiaking::net::kakusan::control;

class ControlChannelIds : public ControlChannelTest {};

TEST_F(ControlChannelIds, StartAtOneAndSendOneMessageAtATime) {
  ControlChannel channel{start};
  const auto first_ticket = enqueued(channel, control::Hello{}, start);
  const auto second_ticket =
      enqueued(channel, control::AdmissionPending{}, start);
  EXPECT_NE(first_ticket, second_ticket);
  EXPECT_NE(first_ticket.value, 0U);
  EXPECT_NE(second_ticket.value, 0U);

  const auto records = polled(channel, start);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(decoded(records[0]).message_id, 1U);
  EXPECT_EQ(decoded(records[0]).kind, ControlKind::data);
  EXPECT_TRUE(polled(channel, start + 100 * ms).empty());
  EXPECT_FALSE(channel.take_acknowledged());

  EXPECT_FALSE(received(channel, ack_record(1), start + 100 * ms));
  EXPECT_EQ(channel.take_acknowledged(), first_ticket);
  EXPECT_FALSE(channel.take_acknowledged());

  const auto next = polled(channel, start + 100 * ms);
  ASSERT_EQ(next.size(), 1U);
  EXPECT_EQ(decoded(next[0]).message_id, 2U);
  EXPECT_FALSE(received(channel, ack_record(2), start + 100 * ms));
  EXPECT_EQ(channel.take_acknowledged(), second_ticket);
  EXPECT_TRUE(channel.idle());
  EXPECT_FALSE(channel.next_deadline_us());
}

TEST_F(ControlChannelIds, CountUpOverManyMessages) {
  Pair pair{start};
  std::uint32_t expected_id = 1;
  for (std::uint32_t index = 1; index <= 6; ++index) {
    const auto ticket = enqueued(pair.host, active_with(index), start);
    auto records = polled(pair.host, start);
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(decoded(records[0]).message_id, expected_id++);
    const auto message = received(pair.guest, records[0], start);
    ASSERT_TRUE(message);
    EXPECT_EQ(encoded_text(*message), encoded_text(active_with(index)));
    const auto acks = polled(pair.guest, start);
    ASSERT_EQ(acks.size(), 1U);
    EXPECT_EQ(decoded(acks[0]).kind, ControlKind::ack);
    EXPECT_FALSE(received(pair.host, acks[0], start));
    EXPECT_EQ(pair.host.take_acknowledged(), ticket);
  }
}

TEST_F(ControlChannelIds, RunOutBeforeTheyOverflow) {
  ControlChannel channel{start, 0xffffffffU};
  const auto ticket = enqueued(channel, control::Hello{}, start);
  const auto overflow = channel.enqueue(control::AdmissionPending{}, start);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), Error::protocol);
  const auto records = polled(channel, start);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(decoded(records[0]).message_id, 0xffffffffU);
  EXPECT_FALSE(received(channel, ack_record(0xffffffffU), start));
  EXPECT_EQ(channel.take_acknowledged(), ticket);
  EXPECT_FALSE(channel.enqueue(control::AdmissionPending{}, start));
  EXPECT_FALSE(channel.send_terminal(control::End{}, start));
}

TEST_F(ControlChannelIds, LeaveRoomForTheLastIdOnly) {
  ControlChannel penultimate{start, 0xfffffffeU};
  EXPECT_TRUE(penultimate.enqueue(control::Hello{}, start));
  EXPECT_TRUE(penultimate.enqueue(control::AdmissionPending{}, start));
  EXPECT_FALSE(penultimate.enqueue(control::SourceIdr{}, start));
}

}  // namespace
}  // namespace net_kakusan_test
