#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class ReliableReceive : public ::testing::Test {
 protected:
  using Gaps = std::vector<takion::GapAcknowledgementBlock>;

  static auto bytes(std::initializer_list<unsigned> values)
      -> std::vector<std::byte> {
    std::vector<std::byte> result;
    for (const auto value : values)
      result.push_back(static_cast<std::byte>(value));
    return result;
  }

  static auto data_body(std::uint32_t sequence, std::uint16_t channel,
                        const std::vector<std::byte>& fragment)
      -> std::vector<std::byte> {
    std::vector<std::byte> body{
        static_cast<std::byte>(sequence >> 24U),
        static_cast<std::byte>(sequence >> 16U),
        static_cast<std::byte>(sequence >> 8U),
        static_cast<std::byte>(sequence),
        static_cast<std::byte>(channel >> 8U),
        static_cast<std::byte>(channel),
        std::byte{0},
        std::byte{0},
    };
    body.insert(body.end(), fragment.begin(), fragment.end());
    return body;
  }

  static auto receiver_config(std::uint32_t peer_initial_sequence,
                              std::uint64_t rto_us = 300'000)
      -> takion::ReliableChannelConfig {
    return {
        .initial_local_sequence = 1,
        .initial_remote_sequence = peer_initial_sequence,
        .maximum_message_size = 4096,
        .maximum_fragment_size = 1200,
        .maximum_fragment_count = 64,
        .retransmission_window_size = 16,
        .receive_reorder_size = 64,
        .retransmission_timeout_us = rto_us,
        .maximum_retries = 24,
    };
  }
};

class ReliableReceiveExpiry
    : public ReliableReceive,
      public ::testing::WithParamInterface<
          std::tuple<std::pair<std::uint64_t, std::uint64_t>, std::int64_t>> {};

TEST_F(ReliableReceive, AssemblesAMessageWhoseFinalFragmentArrivedFirst) {
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config(123)).value();
  EXPECT_EQ(receiver.cumulative_acknowledgement(), 122U);

  const auto early =
      receiver.receive(data_body(124, 0, bytes({0x1f})), true, 0);
  EXPECT_FALSE(early.anomaly);
  EXPECT_TRUE(early.messages.empty());
  EXPECT_EQ(early.acknowledgement->cumulative_sequence, 122U);
  EXPECT_EQ(early.acknowledgement->gap_blocks, (Gaps{{2, 2}}));
  EXPECT_EQ(early.acknowledgement->advertised_receive_window, 102400 - 9);

  const auto first =
      receiver.receive(data_body(123, 0, bytes({0x00, 0x08})), false, 1);
  EXPECT_FALSE(first.anomaly);
  ASSERT_EQ(first.messages.size(), 1U);
  EXPECT_EQ(first.messages[0].data_type.raw, 0U);
  EXPECT_EQ(first.messages[0].payload, bytes({0x08, 0x1f}));
  EXPECT_EQ(first.acknowledgement->cumulative_sequence, 124U);
  EXPECT_TRUE(first.acknowledgement->gap_blocks.empty());
  EXPECT_EQ(first.acknowledgement->advertised_receive_window, 102400U);

  const auto duplicate =
      receiver.receive(data_body(123, 0, bytes({0x00, 0x08})), false, 2);
  EXPECT_EQ(duplicate.anomaly, takion::ReliableChannelError::duplicate);
  EXPECT_TRUE(duplicate.messages.empty());
  EXPECT_EQ(duplicate.acknowledgement->cumulative_sequence, 124U);

  const auto next =
      receiver.receive(data_body(125, 0, bytes({0x00, 0x08, 0x20})), true, 3);
  ASSERT_EQ(next.messages.size(), 1U);
  EXPECT_EQ(next.messages[0].payload, bytes({0x08, 0x20}));
  EXPECT_EQ(next.acknowledgement->cumulative_sequence, 125U);
}

TEST_F(ReliableReceive,
       AdvancesTheCumulativeAcknowledgementBeforeTheMessageIsDelivered) {
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config(1)).value();
  const auto partial =
      receiver.receive(data_body(1, 0, bytes({0x00, 0x01})), false, 0);
  EXPECT_TRUE(partial.messages.empty());
  EXPECT_EQ(partial.acknowledgement->cumulative_sequence, 1U);
  EXPECT_TRUE(partial.acknowledgement->gap_blocks.empty());
  EXPECT_EQ(partial.acknowledgement->advertised_receive_window, 102400 - 10);
  EXPECT_TRUE(receiver.has_incomplete_message());

  const auto middle =
      receiver.receive(data_body(2, 0, bytes({0x02})), false, 1);
  EXPECT_TRUE(middle.messages.empty());
  EXPECT_EQ(middle.acknowledgement->cumulative_sequence, 2U);

  const auto last = receiver.receive(data_body(3, 0, bytes({0x03})), true, 2);
  ASSERT_EQ(last.messages.size(), 1U);
  EXPECT_EQ(last.messages[0].payload, bytes({0x01, 0x02, 0x03}));
  EXPECT_FALSE(receiver.has_incomplete_message());
}

TEST_F(ReliableReceive, ChargesEachHeldChunkItsPayloadPlusEight) {
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config(1)).value();
  EXPECT_TRUE(receiver.receive(data_body(2, 1, bytes({0x00})), true, 0)
                  .messages.empty());
  const auto second = receiver.receive(data_body(3, 2, bytes({0x00})), true, 0);
  EXPECT_EQ(second.acknowledgement->cumulative_sequence, 0U);
  EXPECT_EQ(second.acknowledgement->gap_blocks, (Gaps{{2, 3}}));
  EXPECT_EQ(second.acknowledgement->advertised_receive_window, 102400 - 18);
}

TEST_F(ReliableReceive, ReportsGapBlocksAsSerialOffsetsAcrossTheWrap) {
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config(0xfffffffeU)).value();
  EXPECT_EQ(receiver.cumulative_acknowledgement(), 0xfffffffdU);

  auto ack =
      receiver.receive(data_body(0xffffffffU, 0, bytes({0x00, 0xa1})), true, 0);
  EXPECT_EQ(ack.acknowledgement->gap_blocks, (Gaps{{2, 2}}));
  ack = receiver.receive(data_body(1, 0, bytes({0x00, 0xa3})), true, 0);
  EXPECT_EQ(ack.acknowledgement->gap_blocks, (Gaps{{2, 2}, {4, 4}}));
  ack = receiver.receive(data_body(2, 0, bytes({0x00, 0xa4})), true, 0);
  EXPECT_EQ(ack.acknowledgement->gap_blocks, (Gaps{{2, 2}, {4, 5}}));
  EXPECT_EQ(ack.acknowledgement->cumulative_sequence, 0xfffffffdU);

  ack =
      receiver.receive(data_body(0xfffffffeU, 0, bytes({0x00, 0xa0})), true, 0);
  ASSERT_EQ(ack.messages.size(), 2U);
  EXPECT_EQ(ack.messages[0].payload, bytes({0xa0}));
  EXPECT_EQ(ack.messages[1].payload, bytes({0xa1}));
  EXPECT_EQ(ack.acknowledgement->cumulative_sequence, 0xffffffffU);
  EXPECT_EQ(ack.acknowledgement->gap_blocks, (Gaps{{2, 3}}));

  ack = receiver.receive(data_body(0, 0, bytes({0x00, 0xa2})), true, 0);
  ASSERT_EQ(ack.messages.size(), 3U);
  EXPECT_EQ(ack.messages[0].payload, bytes({0xa2}));
  EXPECT_EQ(ack.messages[2].payload, bytes({0xa4}));
  EXPECT_EQ(ack.acknowledgement->cumulative_sequence, 2U);
  EXPECT_TRUE(ack.acknowledgement->gap_blocks.empty());

  const auto old =
      receiver.receive(data_body(0xffffffffU, 0, bytes({0x00, 0xa1})), true, 0);
  EXPECT_EQ(old.anomaly, takion::ReliableChannelError::duplicate);
  EXPECT_TRUE(old.messages.empty());
  EXPECT_EQ(old.acknowledgement->cumulative_sequence, 2U);
}

TEST_F(ReliableReceive, ConcatenatesFragmentsAcrossTheSequenceBoundary) {
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config(0xffffffffU)).value();
  const auto head = receiver.receive(
      data_body(0xffffffffU, 3, bytes({0x07, 0x01})), false, 0);
  EXPECT_TRUE(head.messages.empty());
  const auto tail = receiver.receive(data_body(0, 3, bytes({0x02})), true, 0);
  ASSERT_EQ(tail.messages.size(), 1U);
  EXPECT_EQ(tail.messages[0].channel, 3U);
  EXPECT_EQ(tail.messages[0].data_type.raw, 7U);
  EXPECT_EQ(tail.messages[0].payload, bytes({0x01, 0x02}));
  EXPECT_EQ(tail.acknowledgement->cumulative_sequence, 0U);
}

TEST_F(ReliableReceive,
       AcknowledgesADuplicateOfABufferedSequenceWithoutDelivery) {
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config(10)).value();
  EXPECT_TRUE(receiver.receive(data_body(12, 0, bytes({0x00})), true, 0)
                  .messages.empty());
  const auto repeated =
      receiver.receive(data_body(12, 0, bytes({0x00})), true, 1);
  EXPECT_EQ(repeated.anomaly, takion::ReliableChannelError::duplicate);
  EXPECT_EQ(repeated.acknowledgement->cumulative_sequence, 9U);
  EXPECT_EQ(repeated.acknowledgement->gap_blocks, (Gaps{{3, 3}}));
  EXPECT_EQ(repeated.acknowledgement->advertised_receive_window, 102400 - 9);
}

TEST_P(ReliableReceiveExpiry,
       SkipsAMissingSequenceOnceItsRtoThresholdHasPassed) {
  const auto [band, delta_us] = GetParam();
  const auto [rto_us, threshold_us] = band;
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config(10, rto_us)).value();
  const std::uint64_t start = 1'000'000;
  const auto gap =
      receiver.receive(data_body(11, 0, bytes({0x00, 0x11})), true, start);
  EXPECT_TRUE(gap.messages.empty());
  EXPECT_EQ(gap.acknowledgement->gap_blocks, (Gaps{{2, 2}}));

  const auto arrival = static_cast<std::uint64_t>(
      static_cast<std::int64_t>(start + threshold_us) + delta_us);
  const auto later =
      receiver.receive(data_body(12, 0, bytes({0x00, 0x12})), true, arrival);
  if (delta_us < 0) {
    EXPECT_FALSE(later.anomaly);
    EXPECT_TRUE(later.messages.empty());
    EXPECT_EQ(later.acknowledgement->cumulative_sequence, 9U);
    EXPECT_EQ(later.acknowledgement->gap_blocks, (Gaps{{2, 3}}));
    return;
  }
  EXPECT_EQ(later.anomaly, takion::ReliableChannelError::reassembly_expired);
  ASSERT_EQ(later.messages.size(), 2U);
  EXPECT_EQ(later.messages[0].payload, bytes({0x11}));
  EXPECT_EQ(later.messages[1].payload, bytes({0x12}));
  EXPECT_EQ(later.acknowledgement->cumulative_sequence, 12U);
  EXPECT_TRUE(later.acknowledgement->gap_blocks.empty());

  const auto late = receiver.receive(data_body(10, 0, bytes({0x00, 0x10})),
                                     true, arrival + 1);
  EXPECT_EQ(late.anomaly, takion::ReliableChannelError::duplicate);
  EXPECT_TRUE(late.messages.empty());
  EXPECT_EQ(late.acknowledgement->cumulative_sequence, 12U);
}

INSTANTIATE_TEST_SUITE_P(
    AcrossTheRtoBands, ReliableReceiveExpiry,
    ::testing::Combine(
        ::testing::Values(
            std::pair<std::uint64_t, std::uint64_t>{300'000, 2'500'000},
            std::pair<std::uint64_t, std::uint64_t>{1'000'000, 2'500'000},
            std::pair<std::uint64_t, std::uint64_t>{1'001'000, 5'000'000},
            std::pair<std::uint64_t, std::uint64_t>{2'000'000, 5'000'000}),
        ::testing::Values(std::int64_t{-1'000}, std::int64_t{0},
                          std::int64_t{1'000})));

TEST_F(ReliableReceive, ExpiresOnArrivalMeasuredFromTheLowestBufferedSequence) {
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config(10)).value();
  EXPECT_TRUE(
      receiver.receive(data_body(13, 0, bytes({0x00, 0x13})), true, 2'000'000)
          .messages.empty());
  EXPECT_TRUE(
      receiver.receive(data_body(11, 0, bytes({0x00, 0x11})), true, 3'000'000)
          .messages.empty());
  const auto still_held =
      receiver.receive(data_body(14, 0, bytes({0x00, 0x14})), true, 5'400'000);
  EXPECT_TRUE(still_held.messages.empty());
  EXPECT_EQ(still_held.acknowledgement->gap_blocks, (Gaps{{2, 2}, {4, 5}}));
  EXPECT_TRUE(receiver.poll(10'000'000).retransmissions.empty());
  EXPECT_EQ(receiver.cumulative_acknowledgement(), 9U);

  const auto skipped =
      receiver.receive(data_body(16, 0, bytes({0x00, 0x16})), true, 5'500'000);
  EXPECT_EQ(skipped.anomaly, takion::ReliableChannelError::reassembly_expired);
  ASSERT_EQ(skipped.messages.size(), 1U);
  EXPECT_EQ(skipped.messages[0].payload, bytes({0x11}));
  EXPECT_EQ(skipped.acknowledgement->cumulative_sequence, 11U);
  EXPECT_EQ(skipped.acknowledgement->gap_blocks, (Gaps{{2, 3}, {5, 5}}));

  const auto again =
      receiver.receive(data_body(17, 0, bytes({0x00, 0x17})), true, 5'500'001);
  EXPECT_EQ(again.anomaly, takion::ReliableChannelError::reassembly_expired);
  EXPECT_EQ(again.messages.size(), 2U);
  EXPECT_EQ(again.acknowledgement->cumulative_sequence, 14U);
  EXPECT_EQ(again.acknowledgement->gap_blocks, (Gaps{{2, 3}}));
}

TEST_F(ReliableReceive, JumpsToFragmentsThatKeepArrivingBeyondTheWindow) {
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config(10)).value();
  const auto first =
      receiver.receive(data_body(140, 0, bytes({0x00, 0x40})), true, 0);
  EXPECT_EQ(first.anomaly,
            takion::ReliableChannelError::receive_window_exceeded);
  const auto held =
      receiver.receive(data_body(141, 0, bytes({0x00, 0x41})), true, 2'400'000);
  EXPECT_EQ(held.anomaly,
            takion::ReliableChannelError::receive_window_exceeded);
  EXPECT_TRUE(held.messages.empty());
  EXPECT_EQ(receiver.cumulative_acknowledgement(), 9U);
  const auto jumped =
      receiver.receive(data_body(142, 0, bytes({0x00, 0x42})), true, 2'500'000);
  EXPECT_EQ(jumped.anomaly, takion::ReliableChannelError::reassembly_expired);
  ASSERT_EQ(jumped.messages.size(), 1U);
  EXPECT_EQ(jumped.messages[0].payload, bytes({0x42}));
  EXPECT_EQ(receiver.cumulative_acknowledgement(), 142U);
  const auto next =
      receiver.receive(data_body(143, 0, bytes({0x00, 0x43})), true, 2'500'001);
  EXPECT_FALSE(next.anomaly);
  ASSERT_EQ(next.messages.size(), 1U);
  EXPECT_EQ(next.messages[0].payload, bytes({0x43}));
}

TEST_F(ReliableReceive, ForgetsTheOutOfWindowRunOnceAFragmentFitsAgain) {
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config(10)).value();
  EXPECT_EQ(
      receiver.receive(data_body(140, 0, bytes({0x00, 0x40})), true, 0).anomaly,
      takion::ReliableChannelError::receive_window_exceeded);
  EXPECT_EQ(
      receiver.receive(data_body(10, 0, bytes({0x00, 0x10})), true, 1'000'000)
          .messages.size(),
      1U);
  EXPECT_EQ(
      receiver.receive(data_body(141, 0, bytes({0x00, 0x41})), true, 3'000'000)
          .anomaly,
      takion::ReliableChannelError::receive_window_exceeded);
  EXPECT_EQ(receiver.cumulative_acknowledgement(), 10U);
}

TEST_F(ReliableReceive, DiscardsAPartialMessageSpanningAnExpiredGap) {
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config(10)).value();
  EXPECT_TRUE(receiver.receive(data_body(10, 0, bytes({0x00, 0x01})), false, 0)
                  .messages.empty());
  EXPECT_TRUE(receiver.has_incomplete_message());
  EXPECT_TRUE(receiver.receive(data_body(12, 0, bytes({0x00, 0x12})), true, 0)
                  .messages.empty());
  const auto skipped =
      receiver.receive(data_body(13, 0, bytes({0x00, 0x13})), true, 2'500'000);
  EXPECT_EQ(skipped.anomaly, takion::ReliableChannelError::reassembly_expired);
  ASSERT_EQ(skipped.messages.size(), 2U);
  EXPECT_EQ(skipped.messages[0].payload, bytes({0x12}));
  EXPECT_EQ(skipped.messages[1].payload, bytes({0x13}));
  EXPECT_FALSE(receiver.has_incomplete_message());
}

TEST_F(ReliableReceive, BoundsItsGapBlocksByTheDatagramLimit) {
  auto config = receiver_config(1);
  config.receive_reorder_size = 1024;
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(config).value();
  EXPECT_EQ(takion::maximum_gap_blocks, 136U);
  for (std::uint32_t index = 1; index <= 140; ++index)
    static_cast<void>(
        receiver.receive(data_body(2 * index, 0, bytes({0x00})), true, 0));
  const auto ack = receiver.selective_acknowledgement();
  EXPECT_EQ(ack.cumulative_sequence, 0U);
  ASSERT_EQ(ack.gap_blocks.size(), 136U);
  EXPECT_EQ(ack.gap_blocks.front(), (takion::GapAcknowledgementBlock{2, 2}));
  EXPECT_EQ(ack.gap_blocks.back(), (takion::GapAcknowledgementBlock{272, 272}));
}

TEST_F(ReliableReceive, ReassemblesInterleavedChannelsIndependently) {
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config(1)).value();
  const auto first =
      receiver.receive(data_body(1, 1, bytes({0x00, 0x0a})), false, 0);
  EXPECT_TRUE(first.messages.empty());
  const auto other =
      receiver.receive(data_body(2, 2, bytes({0x00, 0x0b})), true, 0);
  ASSERT_EQ(other.messages.size(), 1U);
  EXPECT_EQ(other.messages[0].channel, 2U);
  EXPECT_EQ(other.messages[0].payload, bytes({0x0b}));
  const auto last = receiver.receive(data_body(3, 1, bytes({0x0c})), true, 0);
  ASSERT_EQ(last.messages.size(), 1U);
  EXPECT_EQ(last.messages[0].channel, 1U);
  EXPECT_EQ(last.messages[0].payload, bytes({0x0a, 0x0c}));
  EXPECT_EQ(last.acknowledgement->cumulative_sequence, 3U);
  EXPECT_EQ(last.acknowledgement->advertised_receive_window, 102400U);
}

TEST_F(ReliableReceive, DeliversReorderedChannelsWhenTheGapCloses) {
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config(1)).value();
  const auto early =
      receiver.receive(data_body(2, 2, bytes({0x00, 0x22})), true, 0);
  EXPECT_TRUE(early.messages.empty());
  const auto closing =
      receiver.receive(data_body(1, 1, bytes({0x00, 0x11})), true, 0);
  ASSERT_EQ(closing.messages.size(), 2U);
  EXPECT_EQ(closing.messages[0].channel, 1U);
  EXPECT_EQ(closing.messages[1].channel, 2U);
  EXPECT_EQ(closing.acknowledgement->cumulative_sequence, 2U);
}

TEST_F(ReliableReceive, DropsAnOversizedReassemblyThroughItsFinalFragment) {
  auto config = receiver_config(1);
  config.maximum_message_size = 3;
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(config).value();
  EXPECT_EQ(
      receiver.receive(data_body(1, 0, bytes({0x00, 0x01, 0x02})), false, 0)
          .anomaly,
      std::nullopt);
  const auto overflow =
      receiver.receive(data_body(2, 0, bytes({0x03, 0x04})), false, 0);
  EXPECT_EQ(overflow.anomaly, takion::ReliableChannelError::message_too_large);
  const auto end = receiver.receive(data_body(3, 0, bytes({0x05})), true, 0);
  EXPECT_FALSE(end.anomaly);
  EXPECT_TRUE(end.messages.empty());
  const auto fresh = receiver.receive(
      data_body(4, 0, bytes({0x00, 0x01, 0x02, 0x03})), true, 0);
  ASSERT_EQ(fresh.messages.size(), 1U);
  EXPECT_EQ(fresh.messages[0].payload, bytes({0x01, 0x02, 0x03}));
}

}  // namespace
}  // namespace takion_test
