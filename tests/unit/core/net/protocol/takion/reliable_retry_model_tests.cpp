#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "src/core/net/protocol/takion/reliable_retry.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class ReliableRetry : public ::testing::Test {
 protected:
  using Sequences = std::vector<std::uint32_t>;

  static auto bytes(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> result;
    for (const auto character : text)
      result.push_back(
          static_cast<std::byte>(static_cast<unsigned char>(character)));
    return result;
  }

  static auto sequences(const std::vector<takion::OutboundFragment>& fragments)
      -> Sequences {
    Sequences result;
    for (const auto& fragment : fragments) result.push_back(fragment.sequence);
    return result;
  }

  static auto sender_config(
      std::uint32_t initial_local_sequence,
      std::uint32_t peer_window = takion::default_advertised_receive_window,
      std::uint64_t rto_us = takion::default_retransmission_timeout_us,
      std::uint32_t maximum_retries = takion::default_maximum_retries)
      -> takion::ReliableChannelConfig {
    return {
        .initial_local_sequence = initial_local_sequence,
        .initial_remote_sequence = 1,
        .maximum_message_size = 4096,
        .maximum_fragment_size = 1200,
        .maximum_fragment_count = 64,
        .retransmission_window_size = 64,
        .receive_reorder_size = 64,
        .retransmission_timeout_us = rto_us,
        .maximum_retries = maximum_retries,
        .initial_peer_receive_window = peer_window,
    };
  }

  static auto sack(std::uint32_t cumulative, std::uint32_t window,
                   std::vector<takion::GapAcknowledgementBlock> gaps = {})
      -> takion::DataAcknowledgement {
    return {cumulative, window, std::move(gaps), {}};
  }
};

TEST_F(ReliableRetry, DiscardsOnTheTwentySixthAttemptOrPastTheAgeLimit) {
  constexpr auto limit = takion::DataAttemptDecision::discarding_attempt(
      takion::default_maximum_retries);
  static_assert(limit == 26);
  static_assert(takion::DataAttemptDecision::next(0, 0, limit) ==
                takion::DataAttemptDecision{1, false});
  static_assert(takion::DataAttemptDecision::next(24, 0, limit) ==
                takion::DataAttemptDecision{25, false});
  static_assert(takion::DataAttemptDecision::next(25, 0, limit) ==
                takion::DataAttemptDecision{26, true});
  static_assert(takion::DataAttemptDecision::next(0, 15'000, limit) ==
                takion::DataAttemptDecision{1, false});
  static_assert(takion::DataAttemptDecision::next(0, 15'001, limit) ==
                takion::DataAttemptDecision{1, true});
  static_assert(takion::DataAttemptDecision::next(24, 15'001, limit) ==
                takion::DataAttemptDecision{25, true});
}

TEST_F(ReliableRetry, SendsAFragmentTwentyFiveTimesByDefault) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config(1)).value();
  EXPECT_EQ(sender.retransmission_timeout_us(), 300'000U);
  const auto first = sender.send(1, takion::MessageDataType{0}, bytes("a"), 0);
  EXPECT_EQ(first->fragments.size(), 1U);
  std::size_t transmissions = 1;
  std::uint64_t now = 0;
  for (;;) {
    now += 300'000;
    const auto pass = sender.poll(now);
    transmissions += pass.retransmissions.size();
    if (!pass.exhausted_sequences.empty()) {
      EXPECT_EQ(pass.exhausted_sequences, Sequences{1});
      break;
    }
    ASSERT_EQ(pass.retransmissions.size(), 1U);
  }
  EXPECT_EQ(transmissions, 25U);
  EXPECT_EQ(now, 25U * 300'000U);
  EXPECT_EQ(sender.retransmission_timeout_us(), 300'000U);
  EXPECT_EQ(sender.queued_fragment_count(), 0U);
}

TEST_F(ReliableRetry, EstimatesTheRoundTripWithTheReferenceArithmetic) {
  takion::RoundTripEstimator estimator{300'000, 200'000, 2'000'000};
  EXPECT_EQ(estimator.timeout_us(), 300'000U);
  estimator.observe(2'000);
  EXPECT_EQ(estimator.smoothed_ms(), 0U);
  EXPECT_EQ(estimator.timeout_us(), 300'000U);
  EXPECT_EQ(estimator.samples(), 0U);
  estimator.observe(100);
  EXPECT_EQ(estimator.samples(), 1U);
  EXPECT_EQ(estimator.smoothed_ms(), 100U);
  EXPECT_EQ(estimator.variation_ms(), 50U);
  EXPECT_EQ(estimator.timeout_us(), 300'000U);
  estimator.observe(200);
  EXPECT_EQ(estimator.variation_ms(), 62U);
  EXPECT_EQ(estimator.smoothed_ms(), 112U);
  EXPECT_EQ(estimator.timeout_us(), 360'000U);
  estimator.observe(1'999);
  EXPECT_EQ(estimator.variation_ms(), (3 * 62 + (1'999 - 112)) / 4);
  EXPECT_EQ(estimator.smoothed_ms(), (1'999 + 7 * 112) / 8);
  EXPECT_EQ(estimator.timeout_us(), 2'000'000U);
}

TEST_F(ReliableRetry, KeepsRoundTripPercentilesOfTheLatestSamples) {
  takion::RoundTripSamples samples;
  EXPECT_EQ(samples.spread(0), takion::RoundTripSpread{});
  samples.observe(2'000'000, 0);
  EXPECT_EQ(samples.spread(0).samples, 0U);
  for (std::uint64_t sample = 100; sample >= 1; --sample)
    samples.observe(sample * 1'000, 0);
  EXPECT_EQ(samples.spread(0), (takion::RoundTripSpread{100, 50'000, 90'000,
                                                        99'000, 100'000,
                                                        100'000}));
  for (std::size_t index = 0; index < takion::round_trip_sample_capacity;
       ++index)
    samples.observe(7'000, 1'000'000'000);
  EXPECT_EQ(samples.spread(1'000'000'000),
            (takion::RoundTripSpread{100 + takion::round_trip_sample_capacity,
                                     7'000, 7'000, 7'000, 7'000, 7'000}));
}

TEST_F(ReliableRetry, ReportsTheLargestRoundTripOfTheLastTenSeconds) {
  takion::RoundTripSamples samples;
  samples.observe(5'000, 0);
  samples.observe(150'000, 1'000'000);
  samples.observe(6'000, 2'000'000);
  EXPECT_EQ(samples.spread(2'000'000).recent_max_us, 150'000U);
  const auto edge = 1'000'000 + takion::round_trip_recent_window_us;
  EXPECT_EQ(samples.spread(edge).recent_max_us, 150'000U);
  EXPECT_EQ(samples.spread(edge + 1).recent_max_us, 6'000U);
  EXPECT_EQ(samples.spread(edge + 1).max_us, 150'000U);
  const auto quiet = 2'000'001 + takion::round_trip_recent_window_us;
  EXPECT_EQ(samples.spread(quiet).recent_max_us, 0U);
  EXPECT_EQ(samples.spread(quiet).median_us, 6'000U);
}

TEST_F(ReliableRetry, FloorsTheTimeoutWhenTheRoundTripIsZero) {
  takion::RoundTripEstimator zero{300'000, 200'000, 2'000'000};
  zero.observe(0);
  EXPECT_EQ(zero.smoothed_ms(), 0U);
  EXPECT_EQ(zero.timeout_us(), 200'000U);
  EXPECT_EQ(zero.samples(), 1U);
  zero.observe(40);
  EXPECT_EQ(zero.smoothed_ms(), 40U);
  EXPECT_EQ(zero.variation_ms(), 20U);
  EXPECT_EQ(zero.timeout_us(), 200'000U);
}

TEST_F(ReliableRetry, ChargesTheWindowForEachSendAndRetriesAtTheTimeout) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config(10, 4)).value();
  const auto first = sender.send(1, takion::MessageDataType{0}, bytes("ab"), 0);
  EXPECT_EQ(sequences(first->fragments), Sequences{10});
  EXPECT_EQ(sender.peer_receive_window(), 1U);
  EXPECT_EQ(sender.next_poll_us(), 300'000U);
  EXPECT_TRUE(sender.poll(299'900).retransmissions.empty());
  const auto retry = sender.poll(300'000);
  EXPECT_EQ(sequences(retry.retransmissions), Sequences{10});
  EXPECT_EQ(sender.peer_receive_window(), 0U);
}

TEST_F(ReliableRetry,
       ProbesAZeroWindowWithTheFirstEntryEvenWhenItIsAcknowledged) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config(10, 100, 2'000'000))
          .value();
  EXPECT_EQ(sender.send(1, takion::MessageDataType{0}, bytes("ab"), 0)
                ->fragments.size(),
            1U);
  EXPECT_EQ(sender.send(1, takion::MessageDataType{0}, bytes("cd"), 0)
                ->fragments.size(),
            1U);
  const auto marked = sender.acknowledge(sack(9, 0, {{1, 1}}), 1'000);
  ASSERT_TRUE(marked.has_value());
  EXPECT_EQ(marked->acknowledged.size(), 1U);
  EXPECT_TRUE(marked->retransmissions.empty());
  EXPECT_EQ(sender.outstanding_fragment_count(), 1U);
  EXPECT_EQ(sender.queued_fragment_count(), 2U);
  EXPECT_EQ(sender.retransmission_timeout_us(), 200'000U);

  EXPECT_TRUE(sender.poll(150'000).retransmissions.empty());
  EXPECT_EQ(sender.next_poll_us(), 300'000U);
  const auto probe = sender.poll(300'000);
  EXPECT_EQ(sequences(probe.retransmissions), Sequences{10});
  EXPECT_EQ(sender.next_poll_us(), 500'000U);
  EXPECT_EQ(sender.peer_receive_window(), 0U);
}

TEST_F(ReliableRetry, HoldsNewDataUnderAZeroWindowUntilTheWindowOpens) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config(10, 0)).value();
  const auto held = sender.send(1, takion::MessageDataType{0}, bytes("ab"), 0);
  EXPECT_TRUE(held->fragments.empty());
  EXPECT_EQ(sender.outstanding_fragment_count(), 1U);
  EXPECT_TRUE(sender.poll(299'999).retransmissions.empty());
  EXPECT_TRUE(sender.acknowledge(sack(9, 100), 100'000).has_value());
  EXPECT_EQ(sender.next_poll_us(), 100'000U);
  const auto opened = sender.poll(100'000);
  EXPECT_EQ(sequences(opened.retransmissions), Sequences{10});
  EXPECT_EQ(sender.peer_receive_window(), 97U);
}

TEST_F(ReliableRetry, LetsASmallWindowTakeOneAttemptThenHoldsTheRest) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config(10, 2)).value();
  const auto sent =
      sender.send(1, takion::MessageDataType{0}, bytes("abcdefgh"), 0,
                  std::nullopt, takion::MessageFragmentation::single_datagram);
  EXPECT_EQ(sequences(sent->fragments), Sequences{10});
  EXPECT_EQ(sender.peer_receive_window(), 0U);
  const auto next = sender.send(1, takion::MessageDataType{0}, bytes("ij"), 0);
  EXPECT_TRUE(next->fragments.empty());
  EXPECT_EQ(sender.outstanding_fragment_count(), 2U);
}

TEST_F(ReliableRetry, RemovesTheEntrySkippedAfterARemovalOnTheNextPass) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config(1, 100, 300'000, 0))
          .value();
  for (const auto* text : {"a", "b", "c"})
    EXPECT_EQ(sender.send(1, takion::MessageDataType{0}, bytes(text), 0)
                  ->fragments.size(),
              1U);
  const auto first = sender.poll(300'000);
  EXPECT_TRUE(first.retransmissions.empty());
  EXPECT_EQ(first.exhausted_sequences, (Sequences{1, 3}));
  EXPECT_EQ(sender.queued_fragment_count(), 1U);
  const auto second = sender.poll(600'000);
  EXPECT_EQ(second.exhausted_sequences, Sequences{2});
  EXPECT_EQ(sender.queued_fragment_count(), 0U);
  EXPECT_FALSE(sender.next_poll_us());
}

TEST_F(ReliableRetry, TracksTheHighestSentSequenceAcrossTheWrap) {
  takion::ReliableChannel signed_path =
      takion::ReliableChannel::create(sender_config(0)).value();
  EXPECT_EQ(signed_path.highest_sent_sequence(), 0xffffffffU);
  EXPECT_EQ(signed_path.send(1, takion::MessageDataType{0}, bytes("a"), 0)
                ->fragments.size(),
            1U);
  EXPECT_EQ(signed_path.highest_sent_sequence(), 0U);

  takion::ReliableChannel unsigned_path =
      takion::ReliableChannel::create(
          sender_config(0xffffffffU, 100, 300'000, 0))
          .value();
  EXPECT_EQ(unsigned_path.send(1, takion::MessageDataType{0}, bytes("a"), 0)
                ->fragments.size(),
            1U);
  EXPECT_EQ(unsigned_path.send(1, takion::MessageDataType{0}, bytes("b"), 0)
                ->fragments.size(),
            1U);
  EXPECT_EQ(unsigned_path.highest_sent_sequence(), 0U);
  const auto removed = unsigned_path.poll(300'000);
  EXPECT_EQ(removed.exhausted_sequences, Sequences{0xffffffffU});
  EXPECT_EQ(unsigned_path.highest_sent_sequence(), 0xffffffffU);
}

TEST_F(ReliableRetry, SkipsAcknowledgedEntriesWhenItRetries) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config(10, 100)).value();
  for (const auto* text : {"ab", "cd", "ef"})
    EXPECT_EQ(sender.send(1, takion::MessageDataType{0}, bytes(text), 0)
                  ->fragments.size(),
              1U);
  EXPECT_EQ(sender.peer_receive_window(), 91U);
  const auto result = sender.acknowledge(sack(10, 100, {{2, 2}}), 100'000);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->acknowledged.size(), 2U);
  EXPECT_EQ(result->acknowledged[0].sequence, 10U);
  EXPECT_EQ(result->acknowledged[1].sequence, 12U);
  EXPECT_EQ(sequences(result->retransmissions), Sequences{11});
  EXPECT_EQ(sender.retransmission_timeout_us(), 300'000U);
  EXPECT_EQ(sender.round_trip().smoothed_ms(), 100U);
  EXPECT_EQ(sender.round_trip().variation_ms(), 50U);
  EXPECT_EQ(sender.peer_receive_window(), 97U);

  EXPECT_TRUE(sender.poll(399'999).retransmissions.empty());
  const auto pass = sender.poll(400'000);
  EXPECT_EQ(sequences(pass.retransmissions), Sequences{11});
}

TEST_F(ReliableRetry, RetriesAHoleOnlyWhenItWasSentBeforeTheAcknowledgedData) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config(10, 100)).value();
  for (const auto* text : {"a", "b", "c"})
    EXPECT_EQ(sender.send(1, takion::MessageDataType{0}, bytes(text), 0)
                  ->fragments.size(),
              1U);
  EXPECT_EQ(sequences(sender.poll(300'000).retransmissions),
            (Sequences{10, 11, 12}));
  const auto late = sender.acknowledge(sack(9, 100, {{2, 2}}), 310'000);
  EXPECT_TRUE(late->retransmissions.empty());
  ASSERT_EQ(late->acknowledged.size(), 1U);
  EXPECT_EQ(late->acknowledged[0].sequence, 11U);
}

TEST_F(ReliableRetry, RetriesAnEarlyHoleOncePerAcknowledgementThatReportsIt) {
  takion::ReliableChannel eager =
      takion::ReliableChannel::create(sender_config(10, 100)).value();
  for (const auto* text : {"a", "b", "c"})
    EXPECT_EQ(eager.send(1, takion::MessageDataType{0}, bytes(text), 0)
                  ->fragments.size(),
              1U);
  const auto early = eager.acknowledge(sack(9, 100, {{2, 3}}), 10'000);
  EXPECT_EQ(sequences(early->retransmissions), Sequences{10});
  const auto again = eager.acknowledge(sack(9, 100, {{2, 3}}), 20'000);
  EXPECT_EQ(sequences(again->retransmissions), Sequences{10});
  EXPECT_TRUE(again->acknowledged.empty());
  const auto narrower = eager.acknowledge(sack(9, 100, {{2, 2}}), 30'000);
  EXPECT_TRUE(narrower->retransmissions.empty());
}

TEST_F(ReliableRetry, DoesNotRetryAHoleWhileTheWindowIsClosed) {
  takion::ReliableChannel closed =
      takion::ReliableChannel::create(sender_config(10, 100)).value();
  for (const auto* text : {"a", "b"})
    EXPECT_EQ(closed.send(1, takion::MessageDataType{0}, bytes(text), 0)
                  ->fragments.size(),
              1U);
  const auto shut = closed.acknowledge(sack(9, 0, {{2, 2}}), 10'000);
  EXPECT_TRUE(shut->retransmissions.empty());
  EXPECT_EQ(shut->acknowledged.size(), 1U);
}

TEST_F(ReliableRetry, SamplesTheRoundTripFromTheFastestFirstTransmission) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config(10, 100)).value();
  EXPECT_EQ(sender.send(1, takion::MessageDataType{0}, bytes("a"), 0)
                ->fragments.size(),
            1U);
  EXPECT_EQ(sequences(sender.poll(300'000).retransmissions), Sequences{10});
  EXPECT_EQ(sender.send(1, takion::MessageDataType{0}, bytes("b"), 350'000)
                ->fragments.size(),
            1U);
  EXPECT_EQ(sender.send(1, takion::MessageDataType{0}, bytes("c"), 380'000)
                ->fragments.size(),
            1U);
  EXPECT_TRUE(sender.acknowledge(sack(11, 100, {{1, 1}}), 400'500).has_value());
  EXPECT_EQ(sender.round_trip().smoothed_ms(), 20U);
  EXPECT_EQ(sender.round_trip().variation_ms(), 10U);
  EXPECT_EQ(sender.retransmission_timeout_us(), 200'000U);
}

TEST_F(ReliableRetry, TakesNoRoundTripSampleFromARetransmittedEntry) {
  takion::ReliableChannel retransmitted =
      takion::ReliableChannel::create(sender_config(10, 100)).value();
  EXPECT_EQ(retransmitted.send(1, takion::MessageDataType{0}, bytes("a"), 0)
                ->fragments.size(),
            1U);
  EXPECT_EQ(retransmitted.poll(300'000).retransmissions.size(), 1U);
  EXPECT_EQ(
      retransmitted.acknowledge(sack(10, 100), 310'000)->acknowledged.size(),
      1U);
  EXPECT_EQ(retransmitted.round_trip().smoothed_ms(), 0U);
  EXPECT_EQ(retransmitted.retransmission_timeout_us(), 300'000U);
}

TEST_F(ReliableRetry, IgnoresATwoSecondRoundTripButSamplesOneJustBelow) {
  takion::ReliableChannel slow =
      takion::ReliableChannel::create(sender_config(10, 100)).value();
  EXPECT_EQ(
      slow.send(1, takion::MessageDataType{0}, bytes("a"), 0)->fragments.size(),
      1U);
  EXPECT_EQ(slow.acknowledge(sack(10, 100), 2'000'000)->acknowledged.size(),
            1U);
  EXPECT_EQ(slow.round_trip().smoothed_ms(), 0U);
  EXPECT_EQ(slow.send(1, takion::MessageDataType{0}, bytes("b"), 2'000'000)
                ->fragments.size(),
            1U);
  EXPECT_EQ(slow.acknowledge(sack(11, 100), 3'999'999)->acknowledged.size(),
            1U);
  EXPECT_EQ(slow.round_trip().smoothed_ms(), 1'999U);
  EXPECT_EQ(slow.retransmission_timeout_us(), 2'000'000U);
}

TEST_F(ReliableRetry, RefreshesTheWindowFromAStaleAcknowledgement) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config(12, 1)).value();
  EXPECT_EQ(sender.send(1, takion::MessageDataType{0}, bytes("ab"), 0)
                ->fragments.size(),
            1U);
  const auto stale = sender.acknowledge(sack(10, 200, {{2, 2}}), 1'000);
  ASSERT_TRUE(stale.has_value());
  EXPECT_TRUE(stale->acknowledged.empty());
  EXPECT_EQ(sender.peer_receive_window(), 200U);
  EXPECT_EQ(sender.outstanding_fragment_count(), 1U);
}

TEST_F(ReliableRetry, RejectsNoncanonicalGapsBeforeTakingTheWindow) {
  std::size_t index = 0;
  for (const auto& gaps :
       {std::vector<takion::GapAcknowledgementBlock>{{1, 2}, {2, 3}},
        std::vector<takion::GapAcknowledgementBlock>{{1, 1}, {2, 2}},
        std::vector<takion::GapAcknowledgementBlock>{{32768, 32768}},
        std::vector<takion::GapAcknowledgementBlock>{{0, 1}},
        std::vector<takion::GapAcknowledgementBlock>{{3, 2}}}) {
    SCOPED_TRACE(index++);
    takion::ReliableChannel sender =
        takion::ReliableChannel::create(sender_config(10, 17)).value();
    const auto result = sender.acknowledge(sack(10, 200, gaps), 0);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(),
              takion::ReliableChannelError::invalid_acknowledgement);
    EXPECT_EQ(sender.peer_receive_window(), 17U);
  }
}

TEST_F(ReliableRetry, RemovesAnExhaustedHoleAndSkipsTheNextEntry) {
  takion::ReliableChannel sender =
      takion::ReliableChannel::create(sender_config(1, 100, 300'000, 0))
          .value();
  for (const auto* text : {"a", "b", "c", "d", "e", "f"})
    EXPECT_EQ(sender.send(1, takion::MessageDataType{0}, bytes(text), 0)
                  ->fragments.size(),
              1U);
  const auto result = sender.acknowledge(sack(0, 100, {{5, 6}}), 1'000);
  EXPECT_EQ(result->exhausted_sequences, (Sequences{1, 3}));
  EXPECT_TRUE(result->retransmissions.empty());
  ASSERT_EQ(result->acknowledged.size(), 2U);
  EXPECT_EQ(result->acknowledged[0].sequence, 5U);
  EXPECT_EQ(result->acknowledged[1].sequence, 6U);
  EXPECT_EQ(sender.queued_fragment_count(), 4U);
  EXPECT_EQ(sender.outstanding_fragment_count(), 2U);

  const auto stale = sender.acknowledge(sack(0, 100, {{5, 6}}), 2'000);
  ASSERT_TRUE(stale.has_value());
  EXPECT_TRUE(stale->exhausted_sequences.empty());
  EXPECT_TRUE(stale->acknowledged.empty());
  EXPECT_EQ(sender.queued_fragment_count(), 4U);
}

}  // namespace
}  // namespace takion_test
