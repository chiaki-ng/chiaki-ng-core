#include "src/core/net/protocol/takion/generic_data_exchange.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class GenericDataExchangeRig : public ::testing::Test {
 protected:
  using Type = takion::GenericDataControlType;

  struct Split final {
    std::vector<takion::GenericDataControl> controls;
    std::vector<takion::GenericDataChunk> chunks;
    std::vector<takion::GenericDataDelivered> delivered;
    std::vector<takion::GenericDataOutgoingEnded> ended;
  };

  static constexpr auto raw(Type type) -> std::uint32_t {
    return static_cast<std::uint32_t>(type);
  }

  static auto pattern(std::size_t size) -> std::vector<std::byte> {
    std::vector<std::byte> data(size);
    for (std::size_t index = 0; index < size; ++index)
      data[index] = static_cast<std::byte>((index * 31U + 7U) & 0xffU);
    return data;
  }

  static auto split(std::vector<takion::GenericDataAction> actions) -> Split {
    Split result;
    for (auto& action : actions) {
      if (auto* sent_control =
              std::get_if<takion::GenericDataSendControl>(&action))
        result.controls.push_back(std::move(sent_control->control));
      else if (auto* sent_chunk =
                   std::get_if<takion::GenericDataSendChunk>(&action))
        result.chunks.push_back(std::move(sent_chunk->chunk));
      else if (auto* delivered =
                   std::get_if<takion::GenericDataDelivered>(&action))
        result.delivered.push_back(std::move(*delivered));
      else
        result.ended.push_back(
            std::get<takion::GenericDataOutgoingEnded>(action));
    }
    return result;
  }

  static auto control(std::uint32_t transfer_id, Type type)
      -> takion::GenericDataControl {
    return takion::GenericDataControl{transfer_id, raw(type), {}, {},
                                      {},          {},        {}};
  }

  static auto announcement(std::uint32_t transfer_id, std::uint32_t size,
                           std::uint32_t count, std::string metadata = {})
      -> takion::GenericDataControl {
    return takion::GenericDataControl{
        transfer_id, raw(Type::announce), size, std::move(metadata), count, {},
        {}};
  }

  static auto chunk(std::uint32_t id, std::uint8_t index, std::uint32_t offset,
                    std::size_t size) -> takion::GenericDataChunk {
    return takion::GenericDataChunk{
        {}, 0, id, index, offset, libchiaking::SecretBytes{pattern(size)}};
  }

  template <class Result>
  static auto error_of(const Result& result)
      -> std::optional<takion::GenericDataError> {
    if (result) return std::nullopt;
    return result.error();
  }
};

using GenericDataSubmission = GenericDataExchangeRig;
using GenericDataTransfer = GenericDataExchangeRig;
using GenericDataSenderTimers = GenericDataExchangeRig;
using GenericDataMissingPackets = GenericDataExchangeRig;
using GenericDataRejection = GenericDataExchangeRig;
using GenericDataReceiver = GenericDataExchangeRig;

TEST_F(GenericDataSubmission,
       EnforcesItsLimitsAndQueuesBehindTheActiveTransfer) {
  takion::GenericDataExchange exchange{40};
  EXPECT_EQ(error_of(exchange.submit({}, {}, 0)),
            takion::GenericDataError::empty_data);
  EXPECT_EQ(error_of(exchange.submit(pattern(51200), {}, 0)),
            takion::GenericDataError::data_too_large);
  EXPECT_EQ(error_of(exchange.submit(pattern(1), std::string(2048, 'm'), 0)),
            takion::GenericDataError::metadata_too_large);
  const auto largest =
      exchange.submit(pattern(51199), std::string(2047, 'm'), 0);
  ASSERT_TRUE(largest);
  EXPECT_EQ(largest.value(), 40U);
  const auto first = split(exchange.take_actions());
  ASSERT_EQ(first.controls.size(), 1U);
  EXPECT_EQ(first.controls[0],
            announcement(40, 51199, 99, std::string(2047, 'm')));
  for (std::uint32_t index = 0;
       index < takion::generic_data_outgoing_queue_capacity; ++index) {
    const auto queued = exchange.submit(pattern(1), {}, 0);
    ASSERT_TRUE(queued) << "index " << index;
    EXPECT_EQ(queued.value(), 41 + index) << "index " << index;
  }
  EXPECT_EQ(error_of(exchange.submit(pattern(1), {}, 0)),
            takion::GenericDataError::queue_full);
  EXPECT_EQ(exchange.queued_outgoing(),
            takion::generic_data_outgoing_queue_capacity);
  EXPECT_TRUE(split(exchange.take_actions()).controls.empty());
  EXPECT_EQ(exchange.active_outgoing(), 40U);
}

TEST_F(GenericDataTransfer, CompletesBetweenTwoExchanges) {
  takion::GenericDataExchange sender{7};
  takion::GenericDataExchange receiver;
  const auto data = pattern(1200);
  const auto submitted = sender.submit(data, "meta", 1000);
  ASSERT_TRUE(submitted);
  EXPECT_EQ(submitted.value(), 7U);
  auto announced = split(sender.take_actions());
  ASSERT_EQ(announced.controls, std::vector{announcement(7, 1200, 3, "meta")});

  EXPECT_TRUE(receiver.receive_control(announced.controls[0], 1000));
  auto ready = split(receiver.take_actions());
  ASSERT_EQ(ready.controls, std::vector{control(7, Type::ready)});
  EXPECT_EQ(receiver.active_incoming(), 7U);

  EXPECT_TRUE(sender.receive_control(ready.controls[0], 1100));
  auto chunks = split(sender.take_actions()).chunks;
  ASSERT_EQ(chunks.size(), 3U);
  for (std::size_t index = 0; index < chunks.size(); ++index) {
    EXPECT_EQ(chunks[index].transfer_id, 7U) << "index " << index;
    EXPECT_EQ(chunks[index].packet_index, index) << "index " << index;
    EXPECT_EQ(chunks[index].transfer_offset, index * 519) << "index " << index;
    EXPECT_EQ(chunks[index].payload.size(), index < 2 ? 519U : 162U)
        << "index " << index;
  }
  EXPECT_TRUE(receiver.receive_chunk(chunks[2]));
  EXPECT_TRUE(receiver.receive_chunk(chunks[0]));
  EXPECT_TRUE(receiver.receive_chunk(chunks[0]));
  EXPECT_TRUE(split(receiver.take_actions()).controls.empty());
  EXPECT_TRUE(receiver.receive_chunk(chunks[1]));
  auto delivered = split(receiver.take_actions());
  ASSERT_EQ(delivered.delivered.size(), 1U);
  EXPECT_EQ(delivered.delivered[0].transfer_id, 7U);
  EXPECT_EQ(delivered.delivered[0].metadata, "meta");
  EXPECT_EQ(delivered.delivered[0].data, data);
  ASSERT_EQ(delivered.controls, std::vector{control(7, Type::complete)});
  EXPECT_FALSE(receiver.active_incoming());

  EXPECT_TRUE(receiver.receive_control(control(7, Type::missing_query), 1200));
  EXPECT_EQ(split(receiver.take_actions()).controls,
            std::vector{control(7, Type::complete)});
  EXPECT_TRUE(receiver.receive_control(control(8, Type::missing_query), 1200));
  EXPECT_TRUE(split(receiver.take_actions()).controls.empty());

  EXPECT_TRUE(sender.receive_control(delivered.controls[0], 1300));
  auto ended = split(sender.take_actions());
  ASSERT_EQ(ended.ended.size(), 1U);
  EXPECT_EQ(ended.ended[0].transfer_id, 7U);
  EXPECT_EQ(ended.ended[0].outcome, takion::GenericDataOutcome::completed);
  EXPECT_FALSE(sender.active_outgoing());
}

TEST_F(GenericDataSenderTimers, UseStrictComparisons) {
  takion::GenericDataExchange sender;
  EXPECT_TRUE(sender.submit(pattern(600), {}, 0));
  static_cast<void>(sender.take_actions());
  sender.poll(1'000'000);
  EXPECT_TRUE(split(sender.take_actions()).controls.empty());
  sender.poll(1'000'001);
  EXPECT_EQ(split(sender.take_actions()).controls,
            std::vector{announcement(1, 600, 2)});
  sender.poll(2'000'001);
  EXPECT_TRUE(split(sender.take_actions()).controls.empty());
  sender.poll(2'000'002);
  EXPECT_EQ(split(sender.take_actions()).controls.size(), 1U);

  EXPECT_TRUE(sender.receive_control(control(1, Type::ready), 3'000'000));
  EXPECT_EQ(split(sender.take_actions()).chunks.size(), 2U);
  sender.poll(3'250'000);
  EXPECT_TRUE(split(sender.take_actions()).controls.empty());
  sender.poll(3'250'001);
  EXPECT_EQ(split(sender.take_actions()).controls,
            std::vector{control(1, Type::missing_query)});
  sender.poll(13'000'000);
  auto pending = split(sender.take_actions());
  EXPECT_TRUE(pending.ended.empty());
  EXPECT_EQ(pending.controls.size(), 1U);
  sender.poll(13'000'001);
  auto timed_out = split(sender.take_actions());
  ASSERT_EQ(timed_out.ended.size(), 1U);
  EXPECT_EQ(timed_out.ended[0].outcome, takion::GenericDataOutcome::timed_out);
  EXPECT_FALSE(sender.active_outgoing());
}

TEST_F(GenericDataSenderTimers, GiveUpOnASilentPeerStrictlyAfterTenSeconds) {
  takion::GenericDataExchange silent;
  EXPECT_TRUE(silent.submit(pattern(1), {}, 500));
  static_cast<void>(silent.take_actions());
  silent.poll(10'000'500);
  EXPECT_TRUE(split(silent.take_actions()).ended.empty());
  silent.poll(10'000'501);
  EXPECT_EQ(split(silent.take_actions()).ended.size(), 1U);
}

TEST_F(GenericDataSenderTimers, NextPollNamesTheFirstMomentATimerActs) {
  takion::GenericDataExchange sender;
  EXPECT_FALSE(sender.next_poll_us());
  EXPECT_TRUE(sender.submit(pattern(600), {}, 0));
  static_cast<void>(sender.take_actions());
  EXPECT_EQ(sender.next_poll_us(), 1'000'001U);
  sender.poll(1'000'001);
  EXPECT_EQ(split(sender.take_actions()).controls.size(), 1U);
  EXPECT_EQ(sender.next_poll_us(), 2'000'002U);

  EXPECT_TRUE(sender.receive_control(control(1, Type::ready), 3'000'000));
  static_cast<void>(sender.take_actions());
  EXPECT_EQ(sender.next_poll_us(), 3'250'001U);
  sender.poll(3'250'000);
  EXPECT_TRUE(split(sender.take_actions()).controls.empty());
  sender.poll(3'250'001);
  EXPECT_EQ(split(sender.take_actions()).controls.size(), 1U);
  EXPECT_EQ(sender.next_poll_us(), 3'500'002U);
}

TEST_F(GenericDataSenderTimers, ARepeatedReadyRestartsTheTransferTimeout) {
  takion::GenericDataExchange sender;
  EXPECT_TRUE(sender.submit(pattern(600), {}, 0));
  static_cast<void>(sender.take_actions());
  sender.poll(9'000'000);
  EXPECT_EQ(split(sender.take_actions()).controls.size(), 1U);
  EXPECT_TRUE(sender.receive_control(control(1, Type::ready), 9'500'000));
  EXPECT_EQ(split(sender.take_actions()).chunks.size(), 2U);
  EXPECT_TRUE(sender.receive_control(control(1, Type::ready), 12'000'000));
  EXPECT_TRUE(split(sender.take_actions()).chunks.empty());
  sender.poll(19'500'001);
  auto still = split(sender.take_actions());
  EXPECT_TRUE(still.ended.empty());
  EXPECT_EQ(still.controls, std::vector{control(1, Type::missing_query)});
  sender.poll(22'000'000);
  EXPECT_TRUE(split(sender.take_actions()).ended.empty());
  sender.poll(22'000'001);
  auto ended = split(sender.take_actions());
  ASSERT_EQ(ended.ended.size(), 1U);
  EXPECT_EQ(ended.ended[0].outcome, takion::GenericDataOutcome::timed_out);
}

TEST_F(GenericDataMissingPackets, AreIgnoredWhileTheTransferIsAnnounced) {
  takion::GenericDataExchange sender;
  EXPECT_TRUE(sender.submit(pattern(600), {}, 0));
  static_cast<void>(sender.take_actions());
  auto response = control(1, Type::missing_response);
  response.missing_packets = {0};
  EXPECT_TRUE(sender.receive_control(response, 10));
  EXPECT_TRUE(split(sender.take_actions()).chunks.empty());
  sender.poll(11);
  EXPECT_TRUE(split(sender.take_actions()).controls.empty());
}

TEST_F(GenericDataMissingPackets, AreResentFromTheEndWithoutDuplicates) {
  takion::GenericDataExchange sender;
  EXPECT_TRUE(sender.submit(pattern(519 * 5), {}, 0));
  static_cast<void>(sender.take_actions());
  EXPECT_TRUE(sender.receive_control(control(1, Type::ready), 10));
  EXPECT_EQ(split(sender.take_actions()).chunks.size(), 5U);
  auto response = control(1, Type::missing_response);
  response.missing_packets = {1, 4, 9, 4, 0};
  EXPECT_TRUE(sender.receive_control(response, 20));
  const auto resent = split(sender.take_actions()).chunks;
  ASSERT_EQ(resent.size(), 3U);
  EXPECT_EQ(resent[0].packet_index, 0U);
  EXPECT_EQ(resent[1].packet_index, 4U);
  EXPECT_EQ(resent[2].packet_index, 1U);
  EXPECT_EQ(resent[1].transfer_offset, 4U * 519U);
  auto other = control(2, Type::missing_response);
  other.missing_packets = {0};
  EXPECT_TRUE(sender.receive_control(other, 30));
  EXPECT_TRUE(split(sender.take_actions()).chunks.empty());
  EXPECT_TRUE(sender.receive_control(control(1, Type::ready), 40));
  EXPECT_TRUE(split(sender.take_actions()).chunks.empty());
}

TEST_F(GenericDataMissingPackets, AResponseListsAtMostTwentyFiveIndices) {
  takion::GenericDataExchange receiver;
  EXPECT_TRUE(receiver.receive_control(announcement(9, 30 * 519, 30), 0));
  static_cast<void>(receiver.take_actions());
  EXPECT_TRUE(receiver.receive_chunk(takion::GenericDataChunk{
      {}, 0, 9, 1, 519, libchiaking::SecretBytes{pattern(519)}}));
  EXPECT_TRUE(receiver.receive_control(control(9, Type::missing_query), 0));
  const auto response = split(receiver.take_actions()).controls;
  ASSERT_EQ(response.size(), 1U);
  const auto& missing = response[0].missing_packets;
  ASSERT_EQ(missing.size(), takion::generic_data_missing_response_limit);
  EXPECT_EQ(missing.front(), 0U);
  EXPECT_EQ(missing[1], 2U);
  EXPECT_EQ(missing.back(), 25U);
}

TEST_F(GenericDataRejection, ReleasesAndActivatesTheNextTransfer) {
  takion::GenericDataExchange sender;
  const auto first = sender.submit(pattern(10), {}, 0);
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value(), 1U);
  const auto second = sender.submit(pattern(20), "second", 0);
  ASSERT_TRUE(second);
  EXPECT_EQ(second.value(), 2U);
  EXPECT_EQ(split(sender.take_actions()).controls.size(), 1U);
  EXPECT_TRUE(sender.receive_control(control(2, Type::reject), 5));
  EXPECT_TRUE(split(sender.take_actions()).ended.empty());
  EXPECT_TRUE(sender.receive_control(control(1, Type::reject), 5));
  auto next = split(sender.take_actions());
  ASSERT_EQ(next.ended.size(), 1U);
  EXPECT_EQ(next.ended[0].outcome, takion::GenericDataOutcome::rejected);
  EXPECT_EQ(next.controls, std::vector{announcement(2, 20, 1, "second")});
  EXPECT_EQ(sender.active_outgoing(), 2U);
  EXPECT_TRUE(sender.receive_control(control(2, Type::complete), 6));
  const auto completed = split(sender.take_actions());
  ASSERT_FALSE(completed.ended.empty());
  EXPECT_EQ(completed.ended[0].outcome, takion::GenericDataOutcome::completed);
  EXPECT_TRUE(sender.receive_control(
      takion::GenericDataControl{2, 99, {}, {}, {}, {}, {}}, 7));
  EXPECT_TRUE(split(sender.take_actions()).controls.empty());
}

TEST_F(GenericDataReceiver, RejectsInvalidAnnouncements) {
  takion::GenericDataExchange receiver;
  const std::vector<takion::GenericDataControl> invalid{
      announcement(3, 0, 1),
      announcement(3, 51200, 99),
      announcement(3, 10, 0),
      announcement(3, 10, 11),
      announcement(3, 50000, 257),
      announcement(3, 10, 1, std::string(2048, 'x')),
      takion::GenericDataControl{3, raw(Type::announce), {}, {}, 1U, {}, {}},
  };
  for (std::size_t index = 0; index < invalid.size(); ++index) {
    EXPECT_EQ(error_of(receiver.receive_control(invalid[index], 0)),
              takion::GenericDataError::invalid_announcement)
        << "index " << index;
    const auto answered = split(receiver.take_actions()).controls;
    ASSERT_EQ(answered.size(), 1U) << "index " << index;
    EXPECT_EQ(answered[0].type, raw(Type::reject)) << "index " << index;
    EXPECT_TRUE(answered[0].error) << "index " << index;
    EXPECT_FALSE(receiver.active_incoming()) << "index " << index;
  }
}

TEST_F(GenericDataReceiver, AcceptsARepeatedAnnouncementAndOnlyNewerTransfers) {
  takion::GenericDataExchange receiver;
  EXPECT_TRUE(receiver.receive_control(announcement(3, 51199, 256), 0));
  EXPECT_TRUE(receiver.receive_control(announcement(3, 51199, 256), 0));
  EXPECT_EQ(split(receiver.take_actions()).controls,
            (std::vector{control(3, Type::ready), control(3, Type::ready)}));
  EXPECT_FALSE(receiver.receive_control(announcement(2, 10, 1), 0));
  const auto stale = split(receiver.take_actions()).controls;
  ASSERT_EQ(stale.size(), 1U);
  EXPECT_EQ(stale[0].type, raw(Type::reject));
  EXPECT_TRUE(stale[0].error);
  EXPECT_EQ(receiver.active_incoming(), 3U);
  EXPECT_TRUE(receiver.receive_control(announcement(4, 10, 1), 0));
  EXPECT_EQ(receiver.active_incoming(), 4U);
}

TEST_F(GenericDataReceiver, ValidatesChunksAgainstTheAnnouncement) {
  takion::GenericDataExchange receiver;
  EXPECT_TRUE(receiver.receive_control(announcement(5, 1000, 2), 0));
  static_cast<void>(receiver.take_actions());
  EXPECT_EQ(error_of(receiver.receive_chunk(chunk(5, 2, 0, 10))),
            takion::GenericDataError::invalid_chunk);
  EXPECT_EQ(error_of(receiver.receive_chunk(chunk(5, 0, 990, 11))),
            takion::GenericDataError::invalid_chunk);
  EXPECT_EQ(error_of(receiver.receive_chunk(chunk(5, 0, 0xffffffffU, 2))),
            takion::GenericDataError::invalid_chunk);
  EXPECT_EQ(error_of(receiver.receive_chunk(chunk(5, 0, 0, 0))),
            takion::GenericDataError::invalid_chunk);
  EXPECT_TRUE(receiver.receive_chunk(chunk(6, 0, 0, 10)));
  EXPECT_TRUE(receiver.receive_chunk(chunk(5, 0, 0, 600)));
  EXPECT_EQ(error_of(receiver.receive_chunk(chunk(5, 1, 599, 401))),
            takion::GenericDataError::invalid_chunk);
  EXPECT_TRUE(split(receiver.take_actions()).controls.empty());

  EXPECT_TRUE(receiver.receive_control(control(5, Type::missing_query), 0));
  auto response = split(receiver.take_actions()).controls;
  ASSERT_EQ(response.size(), 1U);
  EXPECT_EQ(response[0].type, raw(Type::missing_response));
  EXPECT_EQ(response[0].missing_packets, std::vector<std::uint32_t>{1});

  EXPECT_EQ(error_of(receiver.receive_chunk(chunk(5, 1, 700, 300))),
            takion::GenericDataError::incomplete_coverage);
  auto rejected = split(receiver.take_actions());
  EXPECT_TRUE(rejected.delivered.empty());
  ASSERT_EQ(rejected.controls.size(), 1U);
  EXPECT_EQ(rejected.controls[0].type, raw(Type::reject));
  EXPECT_FALSE(receiver.active_incoming());
}

}  // namespace
}  // namespace takion_test
