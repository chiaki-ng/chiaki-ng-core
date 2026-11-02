#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/kakusan/control_channel_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>

namespace net_kakusan_test {
namespace {

namespace control = libchiaking::net::kakusan::control;

class ControlChannelDelivery : public ControlChannelTest {};

TEST_F(ControlChannelDelivery, DeliversReorderedAndRetriedFragmentsOnce) {
  const auto setup = setup_with_headers({512, 512, 512});
  Pair pair{start};
  const auto ticket = enqueued(pair.host, setup, start);
  const auto records = polled(pair.host, start);
  ASSERT_GE(records.size(), 3U);
  EXPECT_EQ(records.size(),
            Codec::control_fragment_count(encoded_text(setup).size()));
  for (std::size_t index = 0; index < records.size(); ++index)
    EXPECT_EQ(decoded(records[index]).index, index);

  auto now = start;
  for (std::size_t index = records.size(); index-- > 1;) {
    EXPECT_FALSE(received(pair.guest, records[index], now));
    EXPECT_FALSE(received(pair.guest, records[index], now));
    EXPECT_TRUE(polled(pair.guest, now).empty());
  }
  const auto message = received(pair.guest, records[0], now);
  ASSERT_TRUE(message);
  EXPECT_EQ(encoded_text(*message), encoded_text(setup));
  EXPECT_FALSE(received(pair.guest, records[0], now));
  EXPECT_FALSE(received(pair.guest, records[1], now));

  const auto acks = polled(pair.guest, now);
  ASSERT_EQ(acks.size(), 1U);
  EXPECT_EQ(decoded(acks[0]).message_id, 1U);

  now += 250 * ms;
  const auto retry = polled(pair.host, now);
  ASSERT_EQ(retry.size(), records.size());
  for (std::size_t index = 0; index < retry.size(); ++index)
    EXPECT_EQ(retry[index], records[index]);
  for (const auto& record : retry)
    EXPECT_FALSE(received(pair.guest, record, now));
  const auto reacks = polled(pair.guest, now);
  ASSERT_EQ(reacks.size(), 1U);
  EXPECT_EQ(decoded(reacks[0]).message_id, 1U);

  EXPECT_FALSE(received(pair.host, reacks[0], now));
  EXPECT_EQ(pair.host.take_acknowledged(), ticket);
  EXPECT_FALSE(received(pair.host, acks[0], now));
  EXPECT_FALSE(pair.host.take_acknowledged());
  EXPECT_TRUE(polled(pair.host, now + sec).empty());
}

TEST_F(ControlChannelDelivery, AcksOnlyOnceEveryFragmentArrived) {
  const auto setup = setup_with_headers({512, 512});
  ControlChannel guest{start};
  const auto fragments = fragments_of(1, setup);
  ASSERT_GE(fragments.size(), 2U);
  for (std::size_t index = 0; index + 1 < fragments.size(); ++index) {
    EXPECT_FALSE(received(guest, fragments[index], start));
    EXPECT_TRUE(polled(guest, start).empty());
  }
  EXPECT_TRUE(received(guest, fragments.back(), start));
  EXPECT_EQ(polled(guest, start).size(), 1U);
}

TEST_F(ControlChannelDelivery, AcksADuplicateOfTheLastMessageAgain) {
  ControlChannel guest{start};
  const auto first = fragments_of(1, ping_with(1));
  const auto second = fragments_of(2, ping_with(2));
  const auto third = fragments_of(3, ping_with(3));

  EXPECT_TRUE(received(guest, first[0], start));
  EXPECT_EQ(polled(guest, start).size(), 1U);
  EXPECT_FALSE(received(guest, first[0], start));
  EXPECT_FALSE(received(guest, first[0], start));
  const auto reacked = polled(guest, start);
  ASSERT_EQ(reacked.size(), 1U);
  EXPECT_EQ(decoded(reacked[0]).message_id, 1U);

  EXPECT_TRUE(received(guest, second[0], start));
  EXPECT_EQ(polled(guest, start).size(), 1U);
  EXPECT_FALSE(received(guest, first[0], start));
  EXPECT_TRUE(polled(guest, start).empty());
  EXPECT_FALSE(received(guest, second[0], start));
  EXPECT_EQ(polled(guest, start).size(), 1U);

  EXPECT_TRUE(received(guest, third[0], start));
  EXPECT_EQ(polled(guest, start).size(), 1U);
  EXPECT_FALSE(received(guest, first[0], start));
  EXPECT_FALSE(received(guest, second[0], start));
  EXPECT_TRUE(polled(guest, start).empty());
}

TEST_F(ControlChannelDelivery, NeverAcksAnAck) {
  ControlChannel channel{start};
  static_cast<void>(enqueued(channel, control::Hello{}, start));
  EXPECT_EQ(polled(channel, start).size(), 1U);
  EXPECT_FALSE(received(channel, ack_record(1), start));
  EXPECT_FALSE(received(channel, ack_record(1), start));
  EXPECT_TRUE(polled(channel, start).empty());
  EXPECT_TRUE(channel.idle());
}

TEST_F(ControlChannelDelivery, NeverAcksTextThatIsNotJson) {
  ControlChannel guest{start};
  EXPECT_EQ(receive_error(guest, fragments_of(1, "not json at all")[0], start),
            Error::protocol);
  EXPECT_TRUE(polled(guest, start).empty());
}

TEST_F(ControlChannelDelivery, NeverAcksJsonThatIsNotAnObject) {
  ControlChannel guest{start};
  EXPECT_EQ(receive_error(guest, fragments_of(1, "[1,2]")[0], start),
            Error::protocol);
  EXPECT_TRUE(polled(guest, start).empty());
}

TEST_F(ControlChannelDelivery, NeverAcksJsonWithADuplicateKey) {
  ControlChannel guest{start};
  EXPECT_EQ(
      receive_error(guest, fragments_of(1, "{\"a\":1,\"a\":2}")[0], start),
      Error::protocol);
  EXPECT_TRUE(polled(guest, start).empty());
}

TEST_F(ControlChannelDelivery, AcksAFragmentedObjectOnlyOnceItIsWhole) {
  ControlChannel guest{start};
  const auto body = padded_object(2000);
  const auto fragments = fragments_of(1, body);
  ASSERT_EQ(fragments.size(), 3U);
  EXPECT_FALSE(received(guest, fragments[2], start));
  EXPECT_FALSE(received(guest, fragments[0], start));
  EXPECT_TRUE(polled(guest, start).empty());
  const auto result = guest.receive(fragments[1], start);
  EXPECT_FALSE(result);
  EXPECT_EQ(polled(guest, start).size(), 1U);
}

TEST_F(ControlChannelDelivery, StillAcksAMessageItCannotUnderstand) {
  ControlChannel guest{start};
  const auto result = guest.receive(fragments_of(1, "{\"a\":1}")[0], start);
  EXPECT_FALSE(result);
  const auto acks = polled(guest, start);
  ASSERT_EQ(acks.size(), 1U);
  EXPECT_EQ(decoded(acks[0]).message_id, 1U);
  EXPECT_TRUE(received(guest, fragments_of(2, ping_with(9))[0], start));
}

}  // namespace
}  // namespace net_kakusan_test
