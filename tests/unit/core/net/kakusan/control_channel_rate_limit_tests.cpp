#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/messages.hpp"
#include "tests/unit/core/net/kakusan/control_channel_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace net_kakusan_test {
namespace {

namespace control = libchiaking::net::kakusan::control;
using libchiaking::net::kakusan::control_header_size;
using libchiaking::net::kakusan::control_receive_byte_burst;
using libchiaking::net::kakusan::control_receive_byte_rate;
using libchiaking::net::kakusan::control_send_byte_rate;

class ControlChannelSendLimit : public ControlChannelTest {
 protected:
  static auto setup_filling(std::size_t total) -> SourceSetup {
    return setup_of_json_length(json_length_for_total(total));
  }

  static auto bytes_for(const SourceSetup& setup) -> std::size_t {
    const auto length = encoded_text(setup).size();
    return length + control_header_size * Codec::control_fragment_count(length);
  }
};

TEST_F(ControlChannelSendLimit, AllowsFortyMessagesThenOneEveryFiftyMs) {
  Pair pair{start};
  for (std::uint32_t index = 1; index <= 40; ++index) {
    static_cast<void>(enqueued(pair.host, active_with(index), start));
    EXPECT_GE(pair.settle(start), 2U);
  }
  EXPECT_EQ(pair.at_guest.size(), 40U);

  static_cast<void>(enqueued(pair.host, active_with(41), start));
  EXPECT_EQ(pair.settle(start), 0U);
  EXPECT_EQ(pair.host.next_deadline_us(), start + 50 * ms);
  EXPECT_EQ(pair.settle(start + 50 * ms - 1), 0U);
  EXPECT_EQ(pair.at_guest.size(), 40U);
  EXPECT_GE(pair.settle(start + 50 * ms), 2U);
  EXPECT_EQ(pair.at_guest.size(), 41U);
  EXPECT_TRUE(pair.host.idle());
}

TEST_F(ControlChannelSendLimit, RefillsItsMessageBudgetOnlyToCapacity) {
  Pair pair{start};
  for (std::uint32_t index = 1; index <= 40; ++index) {
    static_cast<void>(enqueued(pair.host, active_with(index), start));
    pair.settle(start);
  }
  EXPECT_EQ(pair.at_guest.size(), 40U);

  const auto later = start + 10 * sec;
  for (std::uint32_t index = 1; index <= 40; ++index) {
    static_cast<void>(enqueued(pair.host, active_with(index), later));
    pair.settle(later);
  }
  EXPECT_EQ(pair.at_guest.size(), 80U);
  static_cast<void>(enqueued(pair.host, active_with(41), later));
  EXPECT_EQ(pair.settle(later), 0U);
  EXPECT_EQ(pair.host.next_deadline_us(), later + 50 * ms);
  EXPECT_GE(pair.settle(later + 50 * ms), 2U);
  EXPECT_EQ(pair.at_guest.size(), 81U);
}

TEST_F(ControlChannelSendLimit, SendsSixteenKibibytesAtOnceThenWaitsForBytes) {
  const auto one = setup_filling(5500);
  const auto two = setup_filling(5500);
  const auto three = setup_filling(16384 - 11000);
  EXPECT_EQ(bytes_for(one) + bytes_for(two) + bytes_for(three), 16384U);

  Pair pair{start};
  std::size_t sent = 0;
  for (const auto* setup : {&one, &two, &three}) {
    static_cast<void>(enqueued(pair.host, *setup, start));
    const auto records = polled(pair.host, start);
    sent += total_bytes(records);
    EXPECT_EQ(records.size(),
              Codec::control_fragment_count(encoded_text(*setup).size()));
    for (const auto& record : records) {
      if (auto message = received(pair.guest, record, start))
        pair.at_guest.push_back(std::move(*message));
    }
    for (const auto& ack : polled(pair.guest, start))
      static_cast<void>(received(pair.host, ack, start));
  }
  EXPECT_EQ(sent, 16384U);
  EXPECT_EQ(pair.at_guest.size(), 3U);

  static_cast<void>(enqueued(pair.host, ping_with(1), start));
  EXPECT_TRUE(polled(pair.host, start).empty());
  const auto ping_bytes = fragments_of(1, ping_with(1))[0].size();
  const auto ready_after =
      (ping_bytes * sec + control_send_byte_rate - 1) / control_send_byte_rate;
  EXPECT_EQ(pair.host.next_deadline_us(), start + ready_after);
  EXPECT_TRUE(polled(pair.host, start + ready_after - 1).empty());
  EXPECT_EQ(polled(pair.host, start + ready_after).size(), 1U);
}

TEST_F(ControlChannelSendLimit, HoldsBackTheFragmentOneByteOverTheBudget) {
  const auto one = setup_filling(5500);
  const auto two = setup_filling(5500);
  const auto three = setup_filling(16384 - 11000 + 1);

  Pair pair{start};
  std::size_t sent = 0;
  for (const auto* setup : {&one, &two}) {
    static_cast<void>(enqueued(pair.host, *setup, start));
    const auto records = polled(pair.host, start);
    sent += total_bytes(records);
    for (const auto& record : records)
      static_cast<void>(received(pair.guest, record, start));
    for (const auto& ack : polled(pair.guest, start))
      static_cast<void>(received(pair.host, ack, start));
  }
  EXPECT_EQ(sent, 11000U);

  const auto three_fragments =
      Codec::control_fragment_count(encoded_text(three).size());
  static_cast<void>(enqueued(pair.host, three, start));
  auto records = polled(pair.host, start);
  EXPECT_EQ(records.size(), three_fragments - 1);
  const auto ready_after =
      (sec + control_send_byte_rate - 1) / control_send_byte_rate;
  EXPECT_EQ(ready_after, 62U);
  EXPECT_EQ(pair.host.next_deadline_us(), start + ready_after);
  EXPECT_TRUE(polled(pair.host, start + ready_after - 1).empty());
  auto rest = polled(pair.host, start + ready_after);
  ASSERT_EQ(rest.size(), 1U);
  EXPECT_EQ(decoded(rest[0]).index, three_fragments - 1);
  EXPECT_EQ(rest[0].size(), 16384 - 11000 - total_bytes(records) + 1);

  std::optional<Message> message;
  for (const auto& record : records)
    message = received(pair.guest, record, start + ready_after);
  for (const auto& record : rest)
    message = received(pair.guest, record, start + ready_after);
  ASSERT_TRUE(message);
  EXPECT_EQ(encoded_text(*message), encoded_text(three));
}

TEST_F(ControlChannelSendLimit, SendsAcksAsBareHeaders) {
  ControlChannel guest{start};
  std::size_t acked = 0;
  for (std::uint32_t id = 1; id <= 42; ++id) {
    EXPECT_TRUE(received(guest, fragments_of(id, ping_with(1))[0], start));
    acked += total_bytes(polled(guest, start));
  }
  EXPECT_EQ(acked, 42 * control_header_size);
}

class ControlChannelReceiveLimit : public ControlChannelTest {
 protected:
  struct ByteBudgetPlan {
    Message first;
    Message repeated;
    std::size_t copies{};
  };

  static auto plan_byte_budget() -> ByteBudgetPlan {
    std::vector<Message> candidates;
    candidates.emplace_back(ping_with(1));
    for (const auto reason :
         {control::Reason::user, control::Reason::capacity,
          control::Reason::source, control::Reason::timeout,
          control::Reason::network, control::Reason::protocol}) {
      candidates.emplace_back(control::Leave{reason});
      candidates.emplace_back(control::End{reason});
    }
    candidates.emplace_back(control::SourceIdr{});
    candidates.emplace_back(control::AdmissionPending{});
    std::uint32_t generation = 1;
    for (int digits = 1; digits <= 10; ++digits) {
      candidates.emplace_back(active_with(generation));
      candidates.emplace_back(control::Ready{
          libchiaking::net::kakusan::SeatBinding{1, generation}});
      generation = generation > 429496729U ? 4294967295U : generation * 10U;
    }
    for (const auto code :
         {Error::invalid_block, Error::expired, Error::version,
          Error::identity_changed, Error::transcript, Error::pairing_failed,
          Error::no_path, Error::no_capacity, Error::source_join_failed,
          Error::codec, Error::timeout, Error::protocol, Error::source_format,
          Error::congestion, Error::source_ended})
      candidates.emplace_back(control::Failure{code});
    for (const auto& first : candidates) {
      for (const auto& repeated : candidates) {
        const auto first_size = fragments_of(1, first)[0].size();
        const auto repeated_size = fragments_of(2, repeated)[0].size();
        if (control_receive_byte_burst < first_size + repeated_size) continue;
        const auto remaining =
            control_receive_byte_burst - first_size - repeated_size;
        if (remaining % repeated_size == 0)
          return ByteBudgetPlan{
              test_support::kakusan::Messages::clone(first),
              test_support::kakusan::Messages::clone(repeated),
              remaining / repeated_size};
      }
    }
    ADD_FAILURE() << "no pair of messages fills the receive burst";
    return {};
  }

  void fill_the_message_budget(ControlChannel& guest, std::uint64_t now) {
    for (std::uint32_t id = 1; id <= 42; ++id)
      EXPECT_TRUE(received(guest, fragments_of(id, ping_with(1))[0], now));
  }
};

TEST_F(ControlChannelReceiveLimit, RefusesTheFortyThirdMessageAtOnce) {
  ControlChannel guest{start};
  fill_the_message_budget(guest, start);
  EXPECT_EQ(receive_error(guest, fragments_of(43, ping_with(1))[0], start),
            Error::protocol);
}

TEST_F(ControlChannelReceiveLimit, TakesOneMoreMessageEveryFiftyMs) {
  ControlChannel guest{start};
  fill_the_message_budget(guest, start);
  EXPECT_EQ(receive_error(guest, fragments_of(43, ping_with(1))[0],
                          start + 50 * ms - 1),
            Error::protocol);
  EXPECT_TRUE(
      received(guest, fragments_of(43, ping_with(1))[0], start + 50 * ms));
  EXPECT_EQ(
      receive_error(guest, fragments_of(44, ping_with(1))[0], start + 50 * ms),
      Error::protocol);
}

TEST_F(ControlChannelReceiveLimit, RefillsItsMessageBudgetOnlyToCapacity) {
  ControlChannel guest{start};
  fill_the_message_budget(guest, start);
  const auto later = start + 100 * sec;
  for (std::uint32_t id = 43; id <= 84; ++id)
    EXPECT_TRUE(received(guest, fragments_of(id, ping_with(1))[0], later));
  EXPECT_EQ(receive_error(guest, fragments_of(85, ping_with(1))[0], later),
            Error::protocol);
}

TEST_F(ControlChannelReceiveLimit,
       CountsNeitherDuplicatesNorContinuationsAsMessages) {
  ControlChannel guest{start};
  const auto record = fragments_of(1, ping_with(1))[0];
  EXPECT_TRUE(received(guest, record, start));
  for (int copy = 0; copy < 100; ++copy)
    EXPECT_FALSE(received(guest, record, start));

  const auto big = fragments_of(2, padded_object(2000));
  ASSERT_EQ(big.size(), 3U);
  for (const auto& fragment : big)
    static_cast<void>(guest.receive(fragment, start));
  for (std::uint32_t id = 3; id <= 42; ++id)
    EXPECT_TRUE(received(guest, fragments_of(id, ping_with(1))[0], start));
  EXPECT_EQ(receive_error(guest, fragments_of(43, ping_with(1))[0], start),
            Error::protocol);
}

TEST_F(ControlChannelReceiveLimit, RefusesBytesPastTheBurstAndRefillsByRate) {
  const auto plan = plan_byte_budget();
  const auto first = fragments_of(1, plan.first)[0];
  const auto second_record = fragments_of(2, plan.repeated)[0];
  EXPECT_EQ(first.size() + second_record.size() * (plan.copies + 1), 18023U);

  ControlChannel guest{start};
  EXPECT_TRUE(received(guest, first, start));
  EXPECT_TRUE(received(guest, second_record, start));
  for (std::size_t copy = 0; copy < plan.copies; ++copy)
    EXPECT_FALSE(received(guest, second_record, start));
  EXPECT_EQ(receive_error(guest, second_record, start), Error::protocol);

  ControlChannel later{start};
  EXPECT_TRUE(received(later, first, start));
  EXPECT_TRUE(received(later, second_record, start));
  for (std::size_t copy = 0; copy < plan.copies; ++copy)
    EXPECT_FALSE(received(later, second_record, start));
  const auto refilled = start + sec;
  const auto budget = control_receive_byte_rate / second_record.size();
  for (std::size_t copy = 0; copy < budget; ++copy)
    EXPECT_FALSE(received(later, second_record, refilled));
  EXPECT_EQ(receive_error(later, second_record, refilled), Error::protocol);
}

TEST_F(ControlChannelReceiveLimit, CountsAckBytes) {
  ControlChannel channel{start};
  static_cast<void>(enqueued(channel, control::Hello{}, start));
  EXPECT_EQ(polled(channel, start).size(), 1U);
  EXPECT_FALSE(received(channel, ack_record(1), start));
  const auto stale = ack_record(1);
  std::size_t consumed = control_header_size;
  while (consumed + stale.size() <= control_receive_byte_burst) {
    EXPECT_FALSE(received(channel, stale, start));
    consumed += stale.size();
  }
  EXPECT_EQ(consumed, 18016U);
  EXPECT_EQ(receive_error(channel, stale, start), Error::protocol);
}

}  // namespace
}  // namespace net_kakusan_test
