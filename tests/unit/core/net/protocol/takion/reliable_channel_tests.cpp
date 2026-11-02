#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class ReliableChannel : public ::testing::Test {
 protected:
  static auto bytes(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const auto character : text)
      result.push_back(
          static_cast<std::byte>(static_cast<unsigned char>(character)));
    return result;
  }

  static auto config(std::uint32_t local = 10, std::uint32_t remote = 20)
      -> takion::ReliableChannelConfig {
    return {
        .initial_local_sequence = local,
        .initial_remote_sequence = remote,
        .maximum_message_size = 64,
        .maximum_fragment_size = 13,
        .maximum_fragment_count = 8,
        .retransmission_window_size = 8,
        .receive_reorder_size = 8,
        .retransmission_timeout_us = 100,
        .maximum_retries = 2,
    };
  }

  static void rejects_acknowledgement(
      const takion::DataAcknowledgement& acknowledgement) {
    takion::ReliableChannel invalid_sender =
        takion::ReliableChannel::create(config(5, 10)).value();
    EXPECT_TRUE(
        invalid_sender.send(7, takion::MessageDataType{0}, bytes("a"), 10)
            .has_value());
    auto result = invalid_sender.acknowledge(
        {.cumulative_sequence = acknowledgement.cumulative_sequence,
         .advertised_receive_window = 256,
         .gap_blocks = acknowledgement.gap_blocks,
         .duplicate_sequences = {}},
        20);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(),
              takion::ReliableChannelError::invalid_acknowledgement);
    EXPECT_EQ(invalid_sender.outstanding_fragment_count(), 1U);
  }
};

TEST_F(ReliableChannel, DeliversAndAcknowledgesASingleFragmentMessage) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(config(10, 20)).value();
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(config(20, 10)).value();
  const auto original = bytes("abc");
  auto sent = sender.send(7, takion::MessageDataType{0x0d}, original, 1'000);
  ASSERT_TRUE(sent.has_value());
  ASSERT_EQ(sent->fragments.size(), 1U);
  EXPECT_EQ(sent->fragments[0].layout, takion::FragmentLayout::first);
  EXPECT_TRUE(sent->fragments[0].end_of_message);
  EXPECT_EQ(sent->fragments[0].payload.size(),
            takion::first_fragment_overhead + original.size());

  auto received = receiver.receive(sent->fragments[0].payload.bytes(),
                                   sent->fragments[0].end_of_message, 1'010);
  EXPECT_FALSE(received.anomaly);
  ASSERT_EQ(received.messages.size(), 1U);
  EXPECT_EQ(received.messages[0].data_type.raw, 0x0dU);
  EXPECT_EQ(received.messages[0].payload, original);
  ASSERT_TRUE(received.acknowledgement.has_value());
  EXPECT_EQ(received.acknowledgement->cumulative_sequence, 10U);

  auto acknowledged = sender.acknowledge({.cumulative_sequence = 10,
                                          .advertised_receive_window = 100,
                                          .gap_blocks = {},
                                          .duplicate_sequences = {}},
                                         1'025);
  ASSERT_TRUE(acknowledged.has_value());
  ASSERT_EQ(acknowledged->acknowledged.size(), 1U);
  EXPECT_EQ(acknowledged->acknowledged[0].sequence, 10U);
  EXPECT_EQ(acknowledged->acknowledged[0].round_trip_time_us, 25U);
  EXPECT_EQ(sender.outstanding_fragment_count(), 0U);
}

TEST_F(ReliableChannel, ReassemblesFragmentsDeliveredOutOfOrder) {
  auto sender_config = config(100, 200);
  sender_config.maximum_fragment_size = 12;
  auto receiver_config = config(200, 100);
  receiver_config.maximum_fragment_size = 12;
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config).value();
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config).value();
  const auto original = bytes("abcdefghij");
  auto sent =
      sender.send(7, takion::MessageDataType{0xa5}, original, 0, std::nullopt,
                  takion::MessageFragmentation::split_to_fragment_size);
  ASSERT_TRUE(sent.has_value());
  ASSERT_EQ(sent->fragments.size(), 3U);
  EXPECT_EQ(sent->fragments[0].layout, takion::FragmentLayout::first);
  EXPECT_EQ(sent->fragments[1].layout, takion::FragmentLayout::continuation);
  EXPECT_FALSE(sent->fragments[1].end_of_message);
  EXPECT_TRUE(sent->fragments[2].end_of_message);
  EXPECT_EQ(sent->fragments[0].payload.size(), 12U);
  EXPECT_EQ(sent->fragments[1].payload.size(), 12U);
  EXPECT_EQ(sent->fragments[2].payload.size(), 11U);

  auto future = receiver.receive(sent->fragments[1].payload.bytes(),
                                 sent->fragments[1].end_of_message, 5);
  EXPECT_FALSE(future.anomaly);
  EXPECT_TRUE(future.messages.empty());
  ASSERT_TRUE(future.acknowledgement);
  EXPECT_EQ(future.acknowledgement->cumulative_sequence, 99U);
  EXPECT_EQ(future.acknowledgement->gap_blocks,
            (std::vector<takion::GapAcknowledgementBlock>{{2, 2}}));

  auto first = receiver.receive(sent->fragments[0].payload.bytes(),
                                sent->fragments[0].end_of_message, 6);
  EXPECT_FALSE(first.anomaly);
  EXPECT_TRUE(first.messages.empty());
  EXPECT_EQ(first.acknowledgement->cumulative_sequence, 101U);
  EXPECT_TRUE(first.acknowledgement->gap_blocks.empty());

  auto last = receiver.receive(sent->fragments[2].payload.bytes(),
                               sent->fragments[2].end_of_message, 7);
  EXPECT_FALSE(last.anomaly);
  ASSERT_EQ(last.messages.size(), 1U);
  EXPECT_EQ(last.messages[0].data_type.raw, 0xa5U);
  EXPECT_EQ(last.messages[0].payload, original);
  EXPECT_EQ(last.acknowledgement->cumulative_sequence, 102U);
}

TEST_F(ReliableChannel, SendsAnOversizedMessageAsOneDatagramByDefault) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(config(30, 40)).value();
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(config(40, 30)).value();
  const auto original = bytes("abcdefghijklmnopqrstuvwxyz");
  auto sent = sender.send(7, takion::MessageDataType{0}, original, 0);
  ASSERT_TRUE(sent.has_value());
  ASSERT_EQ(sent->fragments.size(), 1U);
  EXPECT_EQ(sent->fragments[0].layout, takion::FragmentLayout::first);
  EXPECT_TRUE(sent->fragments[0].end_of_message);
  EXPECT_EQ(sent->fragments[0].payload.size(),
            takion::first_fragment_overhead + original.size());
  EXPECT_EQ(sender.next_local_sequence(), 31U);
  EXPECT_EQ(sender.outstanding_fragment_count(), 1U);
  auto received = receiver.receive(sent->fragments[0].payload.bytes(), true, 1);
  ASSERT_EQ(received.messages.size(), 1U);
  EXPECT_EQ(received.messages[0].payload, original);
}

TEST_F(ReliableChannel, RejectsATruncatedOrEmptyFragment) {
  takion::ReliableChannel channel =
      takion::ReliableChannel::create(config()).value();
  std::vector<std::byte> short_body(7);
  const auto truncated = channel.receive(short_body, true, 0);
  EXPECT_EQ(truncated.anomaly,
            takion::ReliableChannelError::truncated_fragment);
  EXPECT_FALSE(truncated.acknowledgement);
  const std::vector<std::byte> empty_message{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{20},
      std::byte{0}, std::byte{1}, std::byte{0}, std::byte{0}};
  const auto empty = channel.receive(empty_message, true, 0);
  EXPECT_EQ(empty.anomaly, takion::ReliableChannelError::truncated_fragment);
  EXPECT_TRUE(empty.messages.empty());
  ASSERT_TRUE(empty.acknowledgement);
  EXPECT_EQ(empty.acknowledgement->cumulative_sequence, 20U);
}

TEST_F(ReliableChannel, RefusesAFragmentSizeThatCannotHoldAHeader) {
  auto invalid_config = config();
  invalid_config.maximum_fragment_size = 8;
  const auto invalid = takion::ReliableChannel::create(invalid_config);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(),
            takion::ReliableChannelError::invalid_configuration);
}

TEST_F(ReliableChannel, RefusesAMessageOverItsSizeOrFragmentLimit) {
  auto bounded_config = config();
  bounded_config.maximum_message_size = 5;
  bounded_config.maximum_fragment_size = 10;
  bounded_config.maximum_fragment_count = 2;
  bounded_config.retransmission_window_size = 2;
  takion::ReliableChannel bounded =
      takion::ReliableChannel::create(bounded_config).value();
  const auto too_large =
      bounded.send(7, takion::MessageDataType{0}, bytes("123456"), 0);
  ASSERT_FALSE(too_large);
  EXPECT_EQ(too_large.error(), takion::ReliableChannelError::message_too_large);
  const auto too_many = bounded.send(
      7, takion::MessageDataType{0}, bytes("12345"), 0, std::nullopt,
      takion::MessageFragmentation::split_to_fragment_size);
  ASSERT_FALSE(too_many);
  EXPECT_EQ(too_many.error(),
            takion::ReliableChannelError::fragment_limit_exceeded);
}

TEST_F(ReliableChannel, SendsAMessageUntrackedOnceItsWindowIsFull) {
  auto window_config = config();
  window_config.retransmission_window_size = 1;
  takion::ReliableChannel window =
      takion::ReliableChannel::create(window_config).value();
  EXPECT_TRUE(
      window.send(7, takion::MessageDataType{0}, bytes("a"), 0).has_value());
  const auto sequence_after_first = window.next_local_sequence();
  const auto overflow =
      window.send(7, takion::MessageDataType{0}, bytes("b"), 0);
  ASSERT_TRUE(overflow.has_value());
  ASSERT_EQ(overflow->fragments.size(), 1U);
  EXPECT_EQ(overflow->fragments[0].sequence, sequence_after_first);
  EXPECT_EQ(overflow->untracked_fragments, 1U);
  EXPECT_EQ(window.next_local_sequence(), sequence_after_first + 1U);
  EXPECT_EQ(window.outstanding_fragment_count(), 1U);
}

TEST_F(ReliableChannel,
       SendsAMessageBeyondAFullWindowOnceWithoutRetransmitting) {
  auto window_config = config(40, 20);
  window_config.retransmission_window_size = 2;
  takion::ReliableChannel window =
      takion::ReliableChannel::create(window_config).value();
  EXPECT_EQ(window.send(7, takion::MessageDataType{0}, bytes("a"), 0)
                ->untracked_fragments,
            0U);
  EXPECT_EQ(window.send(7, takion::MessageDataType{0}, bytes("b"), 0)
                ->untracked_fragments,
            0U);
  const auto overflow =
      window.send(7, takion::MessageDataType{0}, bytes("c"), 0);
  ASSERT_TRUE(overflow.has_value());
  ASSERT_EQ(overflow->fragments.size(), 1U);
  EXPECT_EQ(overflow->fragments[0].sequence, 42U);
  EXPECT_EQ(overflow->untracked_fragments, 1U);
  EXPECT_EQ(window.outstanding_fragment_count(), 2U);
  const auto resent = window.poll(150);
  ASSERT_EQ(resent.retransmissions.size(), 2U);
  EXPECT_EQ(resent.retransmissions[0].sequence, 40U);
  EXPECT_EQ(resent.retransmissions[1].sequence, 41U);
}

TEST_F(ReliableChannel, DiscardsOnlyTheExhaustedEntry) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(config(1, 10)).value();
  EXPECT_TRUE(sender.send(7, takion::MessageDataType{0}, bytes("x"), 1'000)
                  .has_value());
  EXPECT_TRUE(
      sender.send(7, takion::MessageDataType{0}, bytes("y"), 0).has_value());
  EXPECT_EQ(sender.poll(1'050).retransmissions.size(), 1U);
  EXPECT_EQ(sender.poll(1'151).retransmissions.size(), 2U);
  const auto exhausted = sender.poll(1'252);
  ASSERT_EQ(exhausted.retransmissions.size(), 1U);
  EXPECT_EQ(exhausted.retransmissions[0].sequence, 1U);
  ASSERT_EQ(exhausted.exhausted_sequences.size(), 1U);
  EXPECT_EQ(exhausted.exhausted_sequences[0], 2U);
  EXPECT_EQ(sender.outstanding_fragment_count(), 1U);
  const auto last = sender.poll(1'353);
  EXPECT_TRUE(last.retransmissions.empty());
  ASSERT_EQ(last.exhausted_sequences.size(), 1U);
  EXPECT_EQ(last.exhausted_sequences[0], 1U);
  EXPECT_EQ(sender.outstanding_fragment_count(), 0U);
}

TEST_F(ReliableChannel, AcceptsServerDataLargerThanItsOwnFragmentBudget) {
  takion::ReliableChannel channel =
      takion::ReliableChannel::create(config()).value();
  std::vector<std::byte> datagram{std::byte{0},  std::byte{0}, std::byte{0},
                                  std::byte{20}, std::byte{0}, std::byte{1},
                                  std::byte{0},  std::byte{0}, std::byte{0x0d}};
  const auto body = bytes("abcdefghijklmnopqrstuvwxyz");
  datagram.insert(datagram.end(), body.begin(), body.end());
  EXPECT_GT(datagram.size(), config().maximum_fragment_size);
  const auto received = channel.receive(datagram, true, 0);
  EXPECT_FALSE(received.anomaly);
  ASSERT_EQ(received.messages.size(), 1U);
  EXPECT_EQ(received.messages[0].payload, body);
  ASSERT_TRUE(received.acknowledgement);
  EXPECT_EQ(received.acknowledgement->cumulative_sequence, 20U);
  EXPECT_EQ(channel.next_remote_sequence(), 21U);
}

TEST_F(ReliableChannel, SuppressesADuplicateFragmentButAcknowledgesIt) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(config(40, 80)).value();
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(config(80, 40)).value();
  auto sent = sender.send(7, takion::MessageDataType{7}, bytes("x"), 0);
  auto first = receiver.receive(sent->fragments[0].payload.bytes(), true, 0);
  EXPECT_EQ(first.messages.size(), 1U);
  auto duplicate =
      receiver.receive(sent->fragments[0].payload.bytes(), true, 1);
  EXPECT_EQ(duplicate.anomaly, takion::ReliableChannelError::duplicate);
  EXPECT_TRUE(duplicate.messages.empty());
  ASSERT_TRUE(duplicate.acknowledgement);
  EXPECT_EQ(duplicate.acknowledgement->cumulative_sequence, 40U);
  EXPECT_TRUE(duplicate.acknowledgement->gap_blocks.empty());
  EXPECT_EQ(duplicate.acknowledgement->advertised_receive_window,
            takion::default_advertised_receive_window);
}

TEST_F(ReliableChannel, BoundsItsReceiveWindowLikeTheReferenceDataQueue) {
  auto receiver_config = config(80, 40);
  receiver_config.receive_reorder_size = 16;
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(config(40, 80)).value();
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config).value();
  std::vector<takion::OutboundFragment> fragments;
  for (int index = 0; index < 18; ++index) {
    auto sent = sender.send(7, takion::MessageDataType{0}, bytes("x"), 0);
    ASSERT_EQ(sent->fragments.size(), 1U);
    fragments.push_back(sent->fragments[0]);
  }
  auto last_in_window =
      receiver.receive(fragments[15].payload.bytes(), true, 0);
  EXPECT_FALSE(last_in_window.anomaly);
  const std::vector<takion::GapAcknowledgementBlock> tail_gap{{16, 16}};
  ASSERT_TRUE(last_in_window.acknowledgement);
  EXPECT_EQ(last_in_window.acknowledgement->cumulative_sequence, 39U);
  EXPECT_EQ(last_in_window.acknowledgement->gap_blocks, tail_gap);
  auto beyond = receiver.receive(fragments[16].payload.bytes(), true, 0);
  EXPECT_EQ(beyond.anomaly,
            takion::ReliableChannelError::receive_window_exceeded);
  ASSERT_TRUE(beyond.acknowledgement);
  EXPECT_EQ(beyond.acknowledgement->gap_blocks, tail_gap);
  auto again = receiver.receive(fragments[15].payload.bytes(), true, 0);
  EXPECT_EQ(again.anomaly, takion::ReliableChannelError::duplicate);
  ASSERT_TRUE(again.acknowledgement);
  EXPECT_EQ(again.acknowledgement->gap_blocks, tail_gap);
  auto head = receiver.receive(fragments[0].payload.bytes(), true, 0);
  ASSERT_TRUE(head.acknowledgement);
  EXPECT_EQ(head.acknowledgement->cumulative_sequence, 40U);
  EXPECT_EQ(head.acknowledgement->gap_blocks,
            (std::vector<takion::GapAcknowledgementBlock>{{15, 15}}));
  EXPECT_EQ(head.messages.size(), 1U);
  auto stale = receiver.receive(fragments[0].payload.bytes(), true, 0);
  EXPECT_EQ(stale.anomaly, takion::ReliableChannelError::duplicate);
  ASSERT_TRUE(stale.acknowledgement);
  EXPECT_EQ(stale.acknowledgement->cumulative_sequence, 40U);
}

TEST_F(ReliableChannel, IdentifiesAContinuationOnlyByTheFinalFlag) {
  auto sender_config = config(0, 50);
  sender_config.maximum_fragment_size = 10;
  takion::ReliableChannel first_sender =
      takion::ReliableChannel::create(sender_config).value();
  auto fragments = first_sender.send(
      7, takion::MessageDataType{1}, bytes("abc"), 0, std::nullopt,
      takion::MessageFragmentation::split_to_fragment_size);
  ASSERT_EQ(fragments->fragments.size(), 2U);

  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(config(50, 0)).value();
  auto partial =
      receiver.receive(fragments->fragments[0].payload.bytes(), false, 0);
  EXPECT_FALSE(partial.anomaly);
  EXPECT_TRUE(receiver.has_incomplete_message());

  auto replacement_config = config(1, 50);
  takion::ReliableChannel replacement_sender =
      takion::ReliableChannel::create(replacement_config).value();
  auto replacement =
      replacement_sender.send(7, takion::MessageDataType{2}, bytes("z"), 1);
  auto joined =
      receiver.receive(replacement->fragments[0].payload.bytes(), true, 1);
  EXPECT_FALSE(joined.anomaly);
  ASSERT_EQ(joined.messages.size(), 1U);
  EXPECT_EQ(joined.messages[0].data_type.raw, 1U);
  EXPECT_EQ(
      joined.messages[0].payload,
      (std::vector<std::byte>{std::byte{'a'}, std::byte{2}, std::byte{'z'}}));
  EXPECT_FALSE(receiver.has_incomplete_message());
}

TEST_F(ReliableChannel, AppliesCumulativeAndSelectiveAcknowledgements) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(config(5, 10)).value();
  for (const auto value : {"a", "b", "c", "d", "e", "f"})
    EXPECT_TRUE(sender.send(7, takion::MessageDataType{0}, bytes(value), 10)
                    .has_value());
  auto ack = sender.acknowledge(
      {
          .cumulative_sequence = 5,
          .advertised_receive_window = 256,
          .gap_blocks = {{2, 3}, {5, 5}},
          .duplicate_sequences = {6, 9},
      },
      20);
  ASSERT_TRUE(ack.has_value());
  ASSERT_EQ(ack->acknowledged.size(), 4U);
  EXPECT_EQ(ack->acknowledged[0].sequence, 5U);
  EXPECT_EQ(ack->acknowledged[1].sequence, 7U);
  EXPECT_EQ(ack->acknowledged[2].sequence, 8U);
  EXPECT_EQ(ack->acknowledged[3].sequence, 10U);
  ASSERT_EQ(ack->retransmissions.size(), 2U);
  EXPECT_EQ(ack->retransmissions[0].sequence, 6U);
  EXPECT_EQ(ack->retransmissions[1].sequence, 9U);
  EXPECT_EQ(sender.outstanding_fragment_count(), 2U);
  EXPECT_EQ(sender.queued_fragment_count(), 5U);

  auto remaining = sender.acknowledge(
      {
          .cumulative_sequence = 9,
          .advertised_receive_window = 256,
          .gap_blocks = {{1, 1}},
          .duplicate_sequences = {},
      },
      21);
  ASSERT_TRUE(remaining.has_value());
  ASSERT_EQ(remaining->acknowledged.size(), 2U);
  EXPECT_EQ(remaining->acknowledged[0].sequence, 6U);
  EXPECT_EQ(remaining->acknowledged[1].sequence, 9U);
  EXPECT_EQ(sender.outstanding_fragment_count(), 0U);
}

TEST_F(ReliableChannel, RejectsOverlappingGapBlocksWithoutTakingTheWindow) {
  takion::ReliableChannel noncanonical =
      takion::ReliableChannel::create(config(5, 10)).value();
  EXPECT_TRUE(noncanonical.send(7, takion::MessageDataType{0}, bytes("a"), 10)
                  .has_value());
  EXPECT_TRUE(noncanonical.send(7, takion::MessageDataType{0}, bytes("b"), 10)
                  .has_value());
  const auto overlapping =
      noncanonical.acknowledge({.cumulative_sequence = 4,
                                .advertised_receive_window = 7,
                                .gap_blocks = {{2, 2}, {1, 1}},
                                .duplicate_sequences = {}},
                               20);
  ASSERT_FALSE(overlapping);
  EXPECT_EQ(overlapping.error(),
            takion::ReliableChannelError::invalid_acknowledgement);
  EXPECT_NE(noncanonical.peer_receive_window(), 7U);
  EXPECT_EQ(noncanonical.outstanding_fragment_count(), 2U);
}

TEST_F(ReliableChannel, RejectsAcknowledgementsOutsideWhatWasSent) {
  EXPECT_NO_FATAL_FAILURE(
      rejects_acknowledgement({.cumulative_sequence = 5,
                               .advertised_receive_window = 0,
                               .gap_blocks = {{0, 1}},
                               .duplicate_sequences = {}}));
  EXPECT_NO_FATAL_FAILURE(
      rejects_acknowledgement({.cumulative_sequence = 5,
                               .advertised_receive_window = 0,
                               .gap_blocks = {{2, 1}},
                               .duplicate_sequences = {}}));
  EXPECT_NO_FATAL_FAILURE(
      rejects_acknowledgement({.cumulative_sequence = 5,
                               .advertised_receive_window = 0,
                               .gap_blocks = {{2, 7}},
                               .duplicate_sequences = {}}));
  EXPECT_NO_FATAL_FAILURE(
      rejects_acknowledgement({.cumulative_sequence = 6,
                               .advertised_receive_window = 0,
                               .gap_blocks = {},
                               .duplicate_sequences = {}}));
  EXPECT_NO_FATAL_FAILURE(rejects_acknowledgement(
      {.cumulative_sequence = static_cast<std::uint32_t>(5 + (1U << 31U)),
       .advertised_receive_window = 0,
       .gap_blocks = {},
       .duplicate_sequences = {}}));
}

TEST_F(ReliableChannel, IgnoresAStaleCumulativeButHonoursItsValidGapBlocks) {
  takion::ReliableChannel stale_sender =
      takion::ReliableChannel::create(config(5, 10)).value();
  EXPECT_TRUE(stale_sender.send(7, takion::MessageDataType{0}, bytes("a"), 10)
                  .has_value());
  auto stale = stale_sender.acknowledge(
      {
          .cumulative_sequence = 3,
          .advertised_receive_window = 256,
          .gap_blocks = {{1, 1}},
          .duplicate_sequences = {},
      },
      22);
  ASSERT_TRUE(stale.has_value());
  EXPECT_TRUE(stale->acknowledged.empty());
  EXPECT_EQ(stale_sender.outstanding_fragment_count(), 1U);

  auto selectively_valid = stale_sender.acknowledge(
      {
          .cumulative_sequence = 4,
          .advertised_receive_window = 256,
          .gap_blocks = {{1, 1}},
          .duplicate_sequences = {},
      },
      23);
  ASSERT_TRUE(selectively_valid.has_value());
  ASSERT_EQ(selectively_valid->acknowledged.size(), 1U);
  EXPECT_EQ(selectively_valid->acknowledged[0].sequence, 5U);
  EXPECT_EQ(stale_sender.outstanding_fragment_count(), 0U);
}

TEST_F(ReliableChannel, RetriesOnTimeoutUntilTheRetriesAreExhausted) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(config(1, 10)).value();
  EXPECT_TRUE(sender.send(7, takion::MessageDataType{0}, bytes("x"), 1'000)
                  .has_value());
  EXPECT_TRUE(sender.poll(1'049).retransmissions.empty());
  EXPECT_TRUE(sender.poll(1'099).retransmissions.empty());
  auto retry_one = sender.poll(1'100);
  EXPECT_EQ(retry_one.retransmissions.size(), 1U);
  EXPECT_TRUE(retry_one.exhausted_sequences.empty());
  EXPECT_TRUE(sender.poll(1'199).retransmissions.empty());
  auto retry_two = sender.poll(1'200);
  EXPECT_EQ(retry_two.retransmissions.size(), 1U);
  EXPECT_TRUE(sender.poll(1'299).retransmissions.empty());
  auto exhausted = sender.poll(1'300);
  EXPECT_TRUE(exhausted.retransmissions.empty());
  ASSERT_EQ(exhausted.exhausted_sequences.size(), 1U);
  EXPECT_EQ(exhausted.exhausted_sequences[0], 1U);
  EXPECT_EQ(sender.outstanding_fragment_count(), 0U);
  EXPECT_FALSE(sender.next_poll_us());
}

TEST_F(ReliableChannel, HandlesSequenceWrapWithSerialNumberArithmetic) {
  const auto maximum = std::numeric_limits<std::uint32_t>::max();
  auto sender_config = config(maximum - 1, 100);
  sender_config.maximum_fragment_size = 10;
  auto receiver_config = config(100, maximum - 1);
  receiver_config.maximum_fragment_size = 10;
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config).value();
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config).value();
  auto sent = sender.send(7, takion::MessageDataType{0xee}, bytes("abcde"), 0,
                          std::nullopt,
                          takion::MessageFragmentation::split_to_fragment_size);
  ASSERT_EQ(sent->fragments.size(), 3U);
  EXPECT_EQ(sent->fragments[0].sequence, maximum - 1);
  EXPECT_EQ(sent->fragments[1].sequence, maximum);
  EXPECT_EQ(sent->fragments[2].sequence, 0U);
  EXPECT_EQ(sender.next_local_sequence(), 1U);

  auto future = receiver.receive(sent->fragments[2].payload.bytes(), true, 1);
  EXPECT_FALSE(future.anomaly);
  const auto first =
      receiver.receive(sent->fragments[0].payload.bytes(), false, 2);
  EXPECT_FALSE(first.anomaly);
  auto complete =
      receiver.receive(sent->fragments[1].payload.bytes(), false, 3);
  ASSERT_EQ(complete.messages.size(), 1U);
  EXPECT_EQ(complete.messages[0].payload, bytes("abcde"));
  EXPECT_EQ(complete.acknowledgement->cumulative_sequence, 0U);

  auto selective = sender.acknowledge(
      {
          .cumulative_sequence = maximum - 1,
          .advertised_receive_window = 100,
          .gap_blocks = {{1, 2}},
          .duplicate_sequences = {},
      },
      10);
  ASSERT_TRUE(selective.has_value());
  ASSERT_EQ(selective->acknowledged.size(), 3U);
  EXPECT_EQ(selective->acknowledged[0].sequence, maximum - 1);
  EXPECT_EQ(selective->acknowledged[1].sequence, maximum);
  EXPECT_EQ(selective->acknowledged[2].sequence, 0U);
  EXPECT_EQ(sender.outstanding_fragment_count(), 0U);

  auto ack = sender.acknowledge({.cumulative_sequence = 0,
                                 .advertised_receive_window = 100,
                                 .gap_blocks = {},
                                 .duplicate_sequences = {}},
                                11);
  ASSERT_TRUE(ack.has_value());
  EXPECT_TRUE(ack->acknowledged.empty());
}

TEST_F(ReliableChannel, ReassemblesEachChannelOnItsOwn) {
  auto sender_config = config(30, 40);
  sender_config.maximum_fragment_size = 12;
  auto receiver_config = config(40, 30);
  receiver_config.maximum_fragment_size = 12;
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config).value();
  takion::ReliableChannel receiver =
      takion::ReliableChannel::create(receiver_config).value();

  auto general = sender.send(1, takion::MessageDataType{0x11}, bytes("ab"), 0);
  ASSERT_TRUE(general.has_value());
  ASSERT_EQ(general->fragments.size(), 1U);
  EXPECT_EQ(general->fragments[0].sequence, 30U);
  EXPECT_EQ(general->fragments[0].channel, 1U);

  auto senkusha = sender.send(8, takion::MessageDataType{0x22}, bytes("cd"), 1);
  ASSERT_TRUE(senkusha.has_value());
  ASSERT_EQ(senkusha->fragments.size(), 1U);
  EXPECT_EQ(senkusha->fragments[0].sequence, 31U);
  EXPECT_EQ(senkusha->fragments[0].channel, 8U);

  auto received_general =
      receiver.receive(general->fragments[0].payload.bytes(), true, 2);
  EXPECT_FALSE(received_general.anomaly);
  ASSERT_EQ(received_general.messages.size(), 1U);
  EXPECT_EQ(received_general.messages[0].channel, 1U);
  EXPECT_EQ(received_general.messages[0].payload, bytes("ab"));

  auto received_senkusha =
      receiver.receive(senkusha->fragments[0].payload.bytes(), true, 3);
  EXPECT_FALSE(received_senkusha.anomaly);
  ASSERT_EQ(received_senkusha.messages.size(), 1U);
  EXPECT_EQ(received_senkusha.messages[0].channel, 8U);
  EXPECT_EQ(received_senkusha.messages[0].payload, bytes("cd"));

  auto fragmented = sender.send(
      1, takion::MessageDataType{0x33}, bytes("abcde"), 4, std::nullopt,
      takion::MessageFragmentation::split_to_fragment_size);
  ASSERT_TRUE(fragmented.has_value());
  ASSERT_EQ(fragmented->fragments.size(), 2U);
  auto partial =
      receiver.receive(fragmented->fragments[0].payload.bytes(), false, 5);
  EXPECT_FALSE(partial.anomaly);
  EXPECT_TRUE(receiver.has_incomplete_message());

  auto mismatch_payload = fragmented->fragments[1].payload.clone();
  mismatch_payload[4] = std::byte{0};
  mismatch_payload[5] = std::byte{8};
  auto mismatch = receiver.receive(mismatch_payload.bytes(), true, 6);
  EXPECT_FALSE(mismatch.anomaly);
  ASSERT_EQ(mismatch.messages.size(), 1U);
  EXPECT_EQ(mismatch.messages[0].channel, 8U);
  EXPECT_EQ(mismatch.messages[0].data_type.raw, 'd');
  EXPECT_EQ(mismatch.messages[0].payload, bytes("e"));
  EXPECT_TRUE(receiver.has_incomplete_message());
}

TEST_F(ReliableChannel,
       CompletesAnOperationWhenItsFinalFragmentIsAcknowledged) {
  auto sender_config = config(5, 10);
  sender_config.maximum_fragment_size = 12;
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config).value();

  EXPECT_TRUE(
      sender.send(7, takion::MessageDataType{1}, bytes("a"), 0).has_value());
  auto multi =
      sender.send(7, takion::MessageDataType{2}, bytes("bcdefghij"), 0, 4242,
                  takion::MessageFragmentation::split_to_fragment_size);
  ASSERT_TRUE(multi.has_value());
  EXPECT_EQ(multi->fragments.size(), 3U);

  auto head = sender.acknowledge({.cumulative_sequence = 5,
                                  .advertised_receive_window = 256,
                                  .gap_blocks = {},
                                  .duplicate_sequences = {}},
                                 10);
  ASSERT_EQ(head->acknowledged.size(), 1U);
  EXPECT_FALSE(head->acknowledged[0].operation_id);
  EXPECT_FALSE(head->acknowledged[0].operation_complete);

  auto partial = sender.acknowledge(
      {
          .cumulative_sequence = 5,
          .advertised_receive_window = 256,
          .gap_blocks = {{1, 2}},
          .duplicate_sequences = {},
      },
      11);
  ASSERT_TRUE(partial.has_value());
  ASSERT_EQ(partial->acknowledged.size(), 2U);
  EXPECT_EQ(partial->acknowledged[0].sequence, 6U);
  EXPECT_EQ(partial->acknowledged[1].sequence, 7U);
  EXPECT_EQ(partial->acknowledged[0].operation_id, 4242U);
  EXPECT_EQ(partial->acknowledged[1].operation_id, 4242U);
  EXPECT_FALSE(partial->acknowledged[0].operation_complete);
  EXPECT_FALSE(partial->acknowledged[1].operation_complete);

  auto final_ack = sender.acknowledge({.cumulative_sequence = 8,
                                       .advertised_receive_window = 256,
                                       .gap_blocks = {},
                                       .duplicate_sequences = {}},
                                      12);
  ASSERT_EQ(final_ack->acknowledged.size(), 1U);
  EXPECT_EQ(final_ack->acknowledged[0].sequence, 8U);
  EXPECT_EQ(final_ack->acknowledged[0].operation_id, 4242U);
  EXPECT_TRUE(final_ack->acknowledged[0].operation_complete);
  EXPECT_EQ(sender.outstanding_fragment_count(), 0U);

  takion::ReliableChannel single =
      takion::ReliableChannel::create(config(50, 60)).value();
  EXPECT_TRUE(
      single.send(7, takion::MessageDataType{3}, bytes("z"), 0, 7).has_value());
  auto done = single.acknowledge({.cumulative_sequence = 50,
                                  .advertised_receive_window = 256,
                                  .gap_blocks = {},
                                  .duplicate_sequences = {}},
                                 1);
  ASSERT_EQ(done->acknowledged.size(), 1U);
  EXPECT_EQ(done->acknowledged[0].operation_id, 7U);
  EXPECT_TRUE(done->acknowledged[0].operation_complete);
}

TEST_F(ReliableChannel, WipesAnOutboundPayloadAndKeepsItCopyable) {
  takion::OutboundFragment fragment;
  fragment.sequence = 7;
  fragment.payload.assign(bytes("sensitive payload"));
  auto copied = fragment;
  EXPECT_TRUE(
      std::ranges::equal(copied.payload.bytes(), fragment.payload.bytes()));
  const std::span<const std::byte> released{fragment.payload.data(),
                                            fragment.payload.size()};
  fragment.payload.clear();
  for (const auto byte : released) EXPECT_EQ(byte, std::byte{});
  EXPECT_TRUE(
      std::ranges::equal(copied.payload.bytes(), bytes("sensitive payload")));

  fragment = copied;
  EXPECT_TRUE(
      std::ranges::equal(fragment.payload.bytes(), copied.payload.bytes()));
  auto& same = fragment;
  fragment = same;
  fragment = std::move(same);
  EXPECT_TRUE(
      std::ranges::equal(fragment.payload.bytes(), copied.payload.bytes()));
  auto moved = std::move(fragment);
  EXPECT_TRUE(
      std::ranges::equal(moved.payload.bytes(), copied.payload.bytes()));
  moved = std::move(copied);
  EXPECT_TRUE(
      std::ranges::equal(moved.payload.bytes(), bytes("sensitive payload")));
  const std::span<const std::byte> moved_released{moved.payload.data(),
                                                  moved.payload.size()};
  moved.payload.clear();
  for (const auto byte : moved_released) EXPECT_EQ(byte, std::byte{});
}

TEST_F(ReliableChannel, WaitsOneRetransmissionTimeoutFromEachAttempt) {
  auto reference = config(1, 10);
  reference.retransmission_timeout_us = 200'000;
  reference.maximum_retries = 24;
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(reference).value();
  EXPECT_TRUE(
      sender.send(7, takion::MessageDataType{0}, bytes("a"), 0).has_value());
  EXPECT_TRUE(sender.send(7, takion::MessageDataType{0}, bytes("b"), 150'000)
                  .has_value());
  EXPECT_TRUE(sender.poll(199'999).retransmissions.empty());
  const auto first = sender.poll(200'000);
  ASSERT_EQ(first.retransmissions.size(), 1U);
  EXPECT_EQ(first.retransmissions[0].sequence, 1U);
  EXPECT_EQ(sender.next_poll_us(), 350'000U);
  EXPECT_TRUE(sender.poll(349'999).retransmissions.empty());
  const auto second = sender.poll(350'000);
  ASSERT_EQ(second.retransmissions.size(), 1U);
  EXPECT_EQ(second.retransmissions[0].sequence, 2U);
  const auto third = sender.poll(400'000);
  ASSERT_EQ(third.retransmissions.size(), 1U);
  EXPECT_EQ(third.retransmissions[0].sequence, 1U);

  EXPECT_TRUE(sender
                  .acknowledge({.cumulative_sequence = 2,
                                .advertised_receive_window = 100,
                                .gap_blocks = {},
                                .duplicate_sequences = {}},
                               450'000)
                  .has_value());
  EXPECT_EQ(sender.retransmission_timeout_us(), 200'000U);
  EXPECT_TRUE(sender.poll(500'000).retransmissions.empty());
  EXPECT_TRUE(sender.send(7, takion::MessageDataType{0}, bytes("c"), 520'000)
                  .has_value());
  EXPECT_TRUE(sender.poll(719'999).retransmissions.empty());
  const auto fourth = sender.poll(720'000);
  ASSERT_EQ(fourth.retransmissions.size(), 1U);
  EXPECT_EQ(fourth.retransmissions[0].sequence, 3U);
}

}  // namespace
}  // namespace takion_test
