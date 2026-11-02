#include "src/core/net/kakusan/media_receiver.hpp"
#include "src/core/net/kakusan/media_sender.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <type_traits>
#include <utility>
#include <vector>

namespace net_kakusan_test {
namespace {

using libchiaking::net::kakusan::ConnectionId;
using libchiaking::net::kakusan::MediaKey;
using libchiaking::net::kakusan::MediaKind;
using libchiaking::net::kakusan::MediaReceiver;
using libchiaking::net::kakusan::MediaSender;

static_assert(!std::is_copy_constructible_v<MediaKey>);

class MediaPath : public ::testing::Test {
 protected:
  struct Outcome {
    std::uint64_t submitted{};
    std::uint64_t delivered{};
    std::uint64_t datagrams{};
    std::uint64_t dropped_on_link{};
    std::uint64_t acks{};
    bool idr_requested{};
  };

  static constexpr std::uint64_t one_way_us = 8000;
  static constexpr std::uint64_t step_us = 1000;
  static constexpr std::uint64_t packet_interval_us = 4000;

  MediaPath() {
    for (std::size_t index = 0; index < key.size(); ++index)
      key[index] = static_cast<std::byte>(0x40 + index);
    for (std::size_t index = 0; index < connection.size(); ++index)
      connection[index] = static_cast<std::byte>(0x90 + index);
  }

  static auto source_packet(std::uint64_t index) -> std::vector<std::byte> {
    const std::size_t size = 200 + (index * 977) % 3800;
    std::vector<std::byte> bytes(size);
    for (std::size_t offset = 0; offset < size; ++offset)
      bytes[offset] = static_cast<std::byte>((index * 31 + offset * 7) & 0xffU);
    return bytes;
  }

  void run(std::uint64_t drop_every, std::uint64_t duration_us,
           Outcome& outcome) {
    MediaSender sender{crypto, key.clone(), connection, 0};
    MediaReceiver receiver{crypto, key.clone(), connection};
    struct Flight {
      std::uint64_t due_us{};
      std::vector<std::byte> bytes;
    };
    std::deque<Flight> downlink;
    std::deque<Flight> uplink;
    std::map<std::uint64_t, std::vector<std::byte>> expected;
    std::uint64_t last_key_position = 0;

    for (std::uint64_t now = 0; now <= duration_us; now += step_us) {
      if (now % packet_interval_us == 0) {
        const auto key_position = 16 * (outcome.submitted + 1);
        auto bytes = source_packet(outcome.submitted);
        const auto kind =
            outcome.submitted % 5 == 4 ? MediaKind::audio : MediaKind::video;
        ASSERT_TRUE(sender.submit(kind, key_position, bytes, now));
        expected.emplace(key_position, std::move(bytes));
        ++outcome.submitted;
      }

      std::vector<std::vector<std::byte>> out;
      ASSERT_TRUE(sender.poll(now, out));
      for (auto& datagram : out) {
        ASSERT_LE(datagram.size(),
                  libchiaking::net::kakusan::max_datagram_size);
        ++outcome.datagrams;
        if (drop_every != 0 && outcome.datagrams % drop_every == 0) {
          ++outcome.dropped_on_link;
          continue;
        }
        downlink.push_back(Flight{now + one_way_us, std::move(datagram)});
      }
      outcome.idr_requested =
          sender.take_idr_request() || outcome.idr_requested;

      while (!downlink.empty() && downlink.front().due_us <= now) {
        const auto received = receiver.receive(downlink.front().bytes, now);
        ASSERT_TRUE(received);
        if (received.value()) {
          const auto found = expected.find(received.value()->key_position);
          ASSERT_NE(found, expected.end());
          ASSERT_EQ(found->second, received.value()->bytes);
          ASSERT_GT(received.value()->key_position, last_key_position);
          last_key_position = received.value()->key_position;
          expected.erase(found);
          ++outcome.delivered;
        }
        downlink.pop_front();
      }
      while (auto ack = receiver.poll(now)) {
        ++outcome.acks;
        uplink.push_back(Flight{now + one_way_us, std::move(*ack)});
      }
      while (!uplink.empty() && uplink.front().due_us <= now) {
        ASSERT_TRUE(sender.on_ack(uplink.front().bytes, now));
        uplink.pop_front();
      }
    }
    EXPECT_FALSE(sender.failure());
  }

  test_support::SoftwareCrypto crypto;
  MediaKey key{};
  ConnectionId connection{};
};

TEST_F(MediaPath, DeliversEverythingInOrderOverACleanLink) {
  Outcome outcome;
  ASSERT_NO_FATAL_FAILURE(run(0, 3'000'000, outcome));
  EXPECT_GT(outcome.submitted, 700U);
  EXPECT_GE(outcome.delivered + 4, outcome.submitted);
  EXPECT_GT(outcome.acks, 100U);
  EXPECT_FALSE(outcome.idr_requested);
}

TEST_F(MediaPath, KeepsRunningOverALossyLinkWithoutRetransmission) {
  Outcome outcome;
  ASSERT_NO_FATAL_FAILURE(run(97, 3'000'000, outcome));
  EXPECT_GT(outcome.dropped_on_link, 10U);
  EXPECT_LT(outcome.delivered, outcome.submitted);
  EXPECT_GE(outcome.delivered * 100, outcome.submitted * 90);
}

TEST_F(MediaPath, IgnoresDatagramsFromAForeignSpoke) {
  auto other = key.clone();
  other[0] ^= std::byte{1};
  MediaSender sender{crypto, std::move(other), connection, 0};
  MediaReceiver receiver{crypto, key.clone(), connection};
  ASSERT_TRUE(sender.submit(MediaKind::video, 16, source_packet(1), 0));
  std::vector<std::vector<std::byte>> out;
  ASSERT_TRUE(sender.poll(0, out));
  EXPECT_FALSE(out.empty());
  for (const auto& datagram : out) {
    const auto received = receiver.receive(datagram, 0);
    ASSERT_TRUE(received);
    EXPECT_FALSE(received.value());
  }
  EXPECT_FALSE(receiver.poll(1'000'000));
}

TEST_F(MediaPath, MovedKeysLeaveTheirOwnersIndependent) {
  auto sender_key = key.clone();
  auto receiver_key = key.clone();
  MediaSender sender{crypto, std::move(sender_key), connection, 0};
  MediaReceiver receiver{crypto, std::move(receiver_key), connection};
  const auto zero = [](std::byte byte) { return byte == std::byte{}; };
  EXPECT_TRUE(std::ranges::all_of(sender_key, zero));
  EXPECT_TRUE(std::ranges::all_of(receiver_key, zero));
  key.clear();

  const auto original = source_packet(1);
  ASSERT_TRUE(sender.submit(MediaKind::video, 16, original, 0));
  std::size_t delivered = 0;
  for (std::uint64_t now = 0; now <= 50'000; now += 1000) {
    std::vector<std::vector<std::byte>> out;
    ASSERT_TRUE(sender.poll(now, out));
    for (const auto& datagram : out) {
      const auto received = receiver.receive(datagram, now);
      ASSERT_TRUE(received);
      if (received.value()) {
        EXPECT_EQ(received.value()->bytes, original);
        ++delivered;
      }
    }
    if (auto ack = receiver.poll(now)) {
      EXPECT_TRUE(sender.on_ack(*ack, now));
    }
  }
  EXPECT_EQ(delivered, 1U);
}

}  // namespace
}  // namespace net_kakusan_test
