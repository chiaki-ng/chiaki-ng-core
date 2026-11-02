#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/kakusan/control_channel_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace net_kakusan_test {
namespace {

namespace control = libchiaking::net::kakusan::control;

class ControlChannelRetransmission : public ControlChannelTest {};

TEST_F(ControlChannelRetransmission, BacksOffToTwoSecondsBetweenRetries) {
  const auto setup = setup_with_headers({512, 512});
  ControlChannel channel{start};
  const auto fragment_count =
      Codec::control_fragment_count(encoded_text(setup).size());
  ASSERT_GE(fragment_count, 2U);
  static_cast<void>(enqueued(channel, setup, start));

  const auto first_time = start + 3 * sec;
  EXPECT_EQ(channel.next_deadline_us(), start);
  const auto first = polled(channel, first_time);
  ASSERT_EQ(first.size(), fragment_count);

  const std::uint64_t waits[] = {250,  500,  1000, 2000, 2000,
                                 2000, 2000, 2000, 2000};
  auto last = first_time;
  for (const auto wait : waits) {
    const auto due = last + wait * ms;
    EXPECT_EQ(channel.next_deadline_us(), due);
    EXPECT_TRUE(polled(channel, due - 1).empty());
    const auto retry = polled(channel, due);
    ASSERT_EQ(retry.size(), fragment_count);
    for (std::size_t index = 0; index < retry.size(); ++index)
      EXPECT_EQ(retry[index], first[index]);
    EXPECT_TRUE(polled(channel, due).empty());
    last = due;
    if (last + 2000 * ms > first_time + 15 * sec) break;
  }
  EXPECT_EQ(last, first_time + (250 + 500 + 1000 + 2000 * 6) * ms);
  EXPECT_EQ(channel.next_deadline_us(), first_time + 15 * sec);
}

TEST_F(ControlChannelRetransmission, GivesUpFifteenSecondsAfterTheFirstSend) {
  ControlChannel channel{start};
  static_cast<void>(enqueued(channel, control::Hello{}, start));
  const auto first_time = start + sec;
  EXPECT_EQ(polled(channel, first_time).size(), 1U);

  auto now = first_time;
  while (now + 2 * sec < first_time + 15 * sec) {
    const auto deadline = channel.next_deadline_us();
    ASSERT_TRUE(deadline);
    EXPECT_GT(*deadline, now);
    now = *deadline;
    EXPECT_EQ(polled(channel, now).size(), 1U);
  }
  EXPECT_EQ(channel.next_deadline_us(), first_time + 15 * sec);
  Records scratch;
  EXPECT_TRUE(channel.poll(first_time + 15 * sec - 1, scratch));
  EXPECT_EQ(poll_error(channel, first_time + 15 * sec), Error::timeout);
  EXPECT_EQ(poll_error(channel, first_time + 16 * sec), Error::timeout);
}

TEST_F(ControlChannelRetransmission, CountsTheDeadlineFromTheFirstSend) {
  ControlChannel channel{start};
  static_cast<void>(enqueued(channel, control::Hello{}, start));
  const auto first_time = start + 4 * sec;
  EXPECT_EQ(polled(channel, first_time).size(), 1U);
  Records scratch;
  EXPECT_TRUE(channel.poll(start + 15 * sec, scratch));
  EXPECT_TRUE(channel.poll(first_time + 15 * sec - 1, scratch));
  EXPECT_FALSE(channel.poll(first_time + 15 * sec, scratch));
}

TEST_F(ControlChannelRetransmission, StopsTheDeadlineOnAnAck) {
  ControlChannel channel{start};
  const auto ticket = enqueued(channel, control::Hello{}, start);
  EXPECT_EQ(polled(channel, start).size(), 1U);
  EXPECT_FALSE(received(channel, ack_record(1), start + 10 * sec));
  EXPECT_EQ(channel.take_acknowledged(), ticket);
  EXPECT_TRUE(channel.idle());
  Records scratch;
  EXPECT_TRUE(channel.poll(start + 20 * sec, scratch));
}

class ControlChannelReceiveDeadline : public ControlChannelTest {
 protected:
  void SetUp() override {
    fragments = fragments_of(1, padded_object(1500));
    ASSERT_EQ(fragments.size(), 2U);
  }

  Records fragments;
};

TEST_F(ControlChannelReceiveDeadline,
       ExpiresFifteenSecondsAfterTheFirstFragment) {
  ControlChannel guest{start};
  EXPECT_FALSE(guest.next_deadline_us());
  EXPECT_FALSE(received(guest, fragments[0], start + sec));
  EXPECT_EQ(guest.next_deadline_us(), start + 16 * sec);
  Records scratch;
  EXPECT_TRUE(guest.poll(start + 16 * sec - 1, scratch));
  EXPECT_FALSE(received(guest, fragments[0], start + 10 * sec));
  EXPECT_EQ(guest.next_deadline_us(), start + 16 * sec);
  EXPECT_EQ(poll_error(guest, start + 16 * sec), Error::timeout);
}

TEST_F(ControlChannelReceiveDeadline, RefusesAFragmentArrivingOnTheDeadline) {
  ControlChannel exact{start};
  EXPECT_FALSE(received(exact, fragments[0], start));
  EXPECT_EQ(receive_error(exact, fragments[1], start + 15 * sec),
            Error::timeout);
}

TEST_F(ControlChannelReceiveDeadline, TakesAFragmentJustBeforeTheDeadline) {
  ControlChannel in_time{start};
  EXPECT_FALSE(received(in_time, fragments[0], start));
  const auto result = in_time.receive(fragments[1], start + 15 * sec - 1);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error(), Error::timeout);
  EXPECT_EQ(polled(in_time, start + 15 * sec - 1).size(), 1U);
}

TEST_F(ControlChannelReceiveDeadline, ClearsOnceTheMessageIsComplete) {
  const auto setup = setup_with_headers({512, 512});
  const auto whole = fragments_of(1, setup);
  ControlChannel guest{start};
  for (const auto& fragment : whole)
    static_cast<void>(received(guest, fragment, start));
  EXPECT_EQ(polled(guest, start + 60 * sec).size(), 1U);
  EXPECT_FALSE(guest.next_deadline_us());
}

TEST_F(ControlChannelReceiveDeadline, AsksForAnImmediatePollToSendAnAck) {
  ControlChannel guest{start};
  EXPECT_TRUE(received(guest, fragments_of(1, ping_with(1))[0], start + sec));
  EXPECT_EQ(guest.next_deadline_us(), start + sec);
  EXPECT_FALSE(guest.idle());
  EXPECT_EQ(polled(guest, start + sec).size(), 1U);
  EXPECT_TRUE(guest.idle());
  EXPECT_FALSE(guest.next_deadline_us());
}

}  // namespace
}  // namespace net_kakusan_test
