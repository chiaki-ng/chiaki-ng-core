#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/kakusan/control_channel_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace net_kakusan_test {
namespace {

namespace control = libchiaking::net::kakusan::control;

class ControlChannelConflictingFragments : public ControlChannelTest {
 protected:
  void SetUp() override {
    fragments = fragments_of(1, padded_object(1500));
    ASSERT_EQ(fragments.size(), 2U);
    good = fragments_of(1, setup_with_headers({512, 512}));
    ASSERT_GE(good.size(), 2U);
  }

  Records fragments;
  Records good;
};

TEST_F(ControlChannelConflictingFragments, FailWhenTheCountChanges) {
  ControlChannel guest{start};
  EXPECT_FALSE(received(guest, fragments[0], start));
  const auto other = fragments_of(1, padded_object(3000));
  ASSERT_EQ(other.size(), 4U);
  EXPECT_EQ(receive_error(guest, other[1], start), Error::protocol);
}

TEST_F(ControlChannelConflictingFragments, FailWhenARepeatedFragmentDiffers) {
  ControlChannel guest{start};
  EXPECT_FALSE(received(guest, fragments[0], start));
  auto altered = fragments[0];
  altered.back() ^= std::byte{1};
  EXPECT_EQ(receive_error(guest, altered, start), Error::protocol);
}

TEST_F(ControlChannelConflictingFragments, FailWhenARepeatedFragmentResizes) {
  ControlChannel guest{start};
  EXPECT_FALSE(received(guest, raw_record(1, 1, 2, 500), start));
  EXPECT_FALSE(received(guest, raw_record(1, 1, 2, 500), start));
  EXPECT_EQ(receive_error(guest, raw_record(1, 1, 2, 501), start),
            Error::protocol);
}

TEST_F(ControlChannelConflictingFragments, FailWhenTheLastFragmentShrinks) {
  ControlChannel guest{start};
  EXPECT_FALSE(received(guest, fragments[1], start));
  auto shorter = fragments_of(1, padded_object(1499));
  ASSERT_EQ(shorter.size(), 2U);
  EXPECT_EQ(receive_error(guest, shorter[1], start), Error::protocol);
}

TEST_F(ControlChannelConflictingFragments,
       FailWhenADeliveredMessageRepeatsDifferently) {
  ControlChannel guest{start};
  for (const auto& record : good)
    static_cast<void>(received(guest, record, start));
  auto altered = good[0];
  altered.back() ^= std::byte{1};
  EXPECT_EQ(receive_error(guest, altered, start), Error::protocol);
}

TEST_F(ControlChannelConflictingFragments,
       FailWhenADeliveredMessageRepeatsWithAnotherSize) {
  ControlChannel guest{start};
  for (const auto& record : good)
    static_cast<void>(received(guest, record, start));
  const auto resized = fragments_of(1, padded_object(1500));
  EXPECT_EQ(receive_error(guest, resized[0], start), Error::protocol);
}

class ControlChannelRejection : public ControlChannelTest {};

TEST_F(ControlChannelRejection, FailsOnMalformedRecords) {
  ControlChannel guest{start};
  const auto good = fragments_of(1, ping_with(1))[0];

  auto bad_prefix = good;
  bad_prefix[3] = std::byte{'2'};
  EXPECT_EQ(receive_error(guest, bad_prefix, start), Error::protocol);
  EXPECT_EQ(
      receive_error(guest, std::span<const std::byte>{good}.first(15), start),
      Error::protocol);
  EXPECT_EQ(receive_error(guest, std::span<const std::byte>{}, start),
            Error::protocol);
  auto truncated = good;
  truncated.pop_back();
  EXPECT_EQ(receive_error(guest, truncated, start), Error::protocol);
  auto zero_id = good;
  for (std::size_t index = 8; index < 12; ++index) zero_id[index] = std::byte{};
  EXPECT_EQ(receive_error(guest, zero_id, start), Error::protocol);
  auto bad_kind = good;
  bad_kind[4] = std::byte{3};
  EXPECT_EQ(receive_error(guest, bad_kind, start), Error::protocol);
  auto reserved = good;
  reserved[5] = std::byte{1};
  EXPECT_EQ(receive_error(guest, reserved, start), Error::protocol);
  auto bad_ack = ack_record(1);
  bad_ack[13] = std::byte{1};
  EXPECT_EQ(receive_error(guest, bad_ack, start), Error::protocol);
}

TEST_F(ControlChannelRejection, FailsOnAFinalFragmentPastTheMessageLimit) {
  ControlChannel guest{start};
  EXPECT_EQ(receive_error(guest, raw_record(1, 8, 9, 513), start),
            Error::protocol);
  EXPECT_FALSE(received(guest, raw_record(1, 8, 9, 512), start));
}

TEST_F(ControlChannelRejection, FailsOnFragmentSizesThatCannotAddUp) {
  ControlChannel guest{start};
  EXPECT_EQ(receive_error(guest, raw_record(1, 0, 10, 960), start),
            Error::protocol);
  EXPECT_EQ(receive_error(guest, raw_record(1, 1, 2, 961), start),
            Error::protocol);
  EXPECT_EQ(receive_error(guest, raw_record(1, 0, 2, 959), start),
            Error::protocol);
  EXPECT_EQ(receive_error(guest, raw_record(1, 1, 2, 0), start),
            Error::protocol);
  EXPECT_EQ(receive_error(guest, raw_record(1, 2, 2, 10), start),
            Error::protocol);
  EXPECT_EQ(receive_error(guest, raw_record(1, 0, 0, 10), start),
            Error::protocol);
}

TEST_F(ControlChannelRejection, AcceptsAMessageOfTheLargestSize) {
  ControlChannel guest{start};
  const auto fragments = fragments_of(1, padded_object(8192));
  ASSERT_EQ(fragments.size(), 9U);
  EXPECT_EQ(fragments.back().size(),
            libchiaking::net::kakusan::control_header_size + 512);
  for (std::size_t index = 0; index + 1 < fragments.size(); ++index)
    EXPECT_FALSE(received(guest, fragments[index], start));
  EXPECT_FALSE(guest.receive(fragments.back(), start));
  EXPECT_EQ(polled(guest, start).size(), 1U);
}

TEST_F(ControlChannelRejection, FailsOnAFirstMessageIdPastOne) {
  ControlChannel guest{start};
  EXPECT_EQ(receive_error(guest, fragments_of(2, ping_with(2))[0], start),
            Error::protocol);
}

TEST_F(ControlChannelRejection, FailsOnTheNextIdWhileAMessageIsIncomplete) {
  ControlChannel guest{start};
  const auto partial = fragments_of(1, padded_object(1500));
  EXPECT_FALSE(received(guest, partial[0], start));
  EXPECT_EQ(receive_error(guest, fragments_of(2, ping_with(2))[0], start),
            Error::protocol);
}

TEST_F(ControlChannelRejection, FailsOnAMessageIdThatSkipsAhead) {
  ControlChannel guest{start};
  EXPECT_TRUE(received(guest, fragments_of(1, ping_with(1))[0], start));
  EXPECT_EQ(receive_error(guest, fragments_of(3, ping_with(3))[0], start),
            Error::protocol);
  EXPECT_EQ(
      receive_error(guest, fragments_of(0xffffffffU, ping_with(3))[0], start),
      Error::protocol);
}

TEST_F(ControlChannelRejection, FailsOnAnAckWithNothingSent) {
  ControlChannel channel{start};
  EXPECT_EQ(receive_error(channel, ack_record(1), start), Error::protocol);
}

TEST_F(ControlChannelRejection, FailsOnAnAckForAMessageNotYetSent) {
  ControlChannel channel{start};
  static_cast<void>(enqueued(channel, control::Hello{}, start));
  EXPECT_EQ(receive_error(channel, ack_record(1), start), Error::protocol);
  EXPECT_EQ(polled(channel, start).size(), 1U);
  EXPECT_EQ(receive_error(channel, ack_record(2), start), Error::protocol);
  EXPECT_EQ(receive_error(channel, ack_record(0xffffffffU), start),
            Error::protocol);
  EXPECT_FALSE(received(channel, ack_record(1), start));
}

TEST_F(ControlChannelRejection, IgnoresARepeatedAckButNotAFutureOne) {
  ControlChannel channel{start};
  const auto ticket = enqueued(channel, control::Hello{}, start);
  EXPECT_EQ(polled(channel, start).size(), 1U);
  EXPECT_FALSE(received(channel, ack_record(1), start));
  EXPECT_EQ(channel.take_acknowledged(), ticket);
  EXPECT_FALSE(received(channel, ack_record(1), start));
  EXPECT_FALSE(received(channel, ack_record(1), start + sec));
  EXPECT_FALSE(channel.take_acknowledged());
  EXPECT_EQ(receive_error(channel, ack_record(2), start), Error::protocol);
}

}  // namespace
}  // namespace net_kakusan_test
