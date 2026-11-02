#include "src/core/net/kakusan/media_receiver.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/kakusan/media_receiver_rig.hpp"
#include "tests/unit/core/net/kakusan/wire_vectors.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;

class MediaReceiverReassembly : public MediaReceiverTest {};
class MediaReceiverLifetime : public MediaReceiverTest {};

class MediaReceiverVectors : public MediaReceiverTest {
 protected:
  void SetUp() override {
    ASSERT_EQ(vectors.fragments().size(), 2U);
    first = vectors.fragment(0);
    second = vectors.fragment(1);
    probe_datagram = test_support::Bytes::unhex(vectors.text("probe_hex"));
  }

  WireVectors vectors;
  Bytes first;
  Bytes second;
  Bytes probe_datagram;
};

TEST_F(MediaReceiverVectors, ReassembleInOrderAndAckAsTheVectorSays) {
  MediaReceiver receiver{crypto, vectors.key().clone(), vectors.connection()};
  EXPECT_FALSE(accept(receiver, first, 0));
  const auto packet = accept(receiver, second, 0);
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->kind, MediaKind::video);
  EXPECT_EQ(packet->key_position, (std::uint64_t{1} << 32U) + 32);
  EXPECT_EQ(packet->bytes, vectors.original());
  EXPECT_FALSE(accept(receiver, probe_datagram, 0));

  const auto ack = receiver.poll(1000);
  ASSERT_TRUE(ack);
  EXPECT_EQ(test_support::Bytes::hex(*ack), vectors.text("ack_hex"));
  EXPECT_EQ(receiver.incomplete_packets(), 0U);
  EXPECT_EQ(receiver.reassembly_bytes(), 0U);
}

TEST_F(MediaReceiverVectors, ReassembleInReverseOrder) {
  MediaReceiver receiver{crypto, vectors.key().clone(), vectors.connection()};
  EXPECT_FALSE(accept(receiver, second, 0));
  const auto packet = accept(receiver, first, 0);
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->bytes, vectors.original());
}

TEST_F(MediaReceiverVectors, AreDroppedUnderAnotherKey) {
  MediaReceiver receiver{crypto, key.clone(), connection};
  EXPECT_FALSE(accept(receiver, first, 0));
  EXPECT_FALSE(accept(receiver, second, 0));
  EXPECT_EQ(receiver.incomplete_packets(), 0U);
  expect_quiet(receiver, 0);
}

TEST_F(MediaReceiverReassembly, RebuildsEverySizeInAnyOrder) {
  for (const std::size_t size :
       {std::size_t{1}, std::size_t{2}, std::size_t{1123}, std::size_t{1124},
        std::size_t{1125}, std::size_t{2248}, std::size_t{2249},
        std::size_t{4000}, max_source_datagram_size - 1,
        max_source_datagram_size}) {
    const auto original = pattern(size, static_cast<std::uint8_t>(size));
    const auto count = TransportFrameCodec::media_fragment_count(size);
    for (const auto kind : {MediaKind::video, MediaKind::audio}) {
      const auto datagrams =
          fragments_of(kind, 9, 0xfeedfacecafebeefULL, original, 0);
      EXPECT_EQ(datagrams.size(), count);
      for (const auto& datagram : datagrams)
        EXPECT_LE(datagram.size(), max_datagram_size);

      std::vector<std::size_t> order(count);
      for (std::size_t index = 0; index < count; ++index) order[index] = index;
      for (int variant = 0; variant < 3; ++variant) {
        if (variant == 1) std::reverse(order.begin(), order.end());
        if (variant == 2 && count > 2)
          std::rotate(order.begin(), order.begin() + 1, order.end());
        MediaReceiver receiver{crypto, key.clone(), connection};
        std::optional<SourcePacket> delivered;
        for (std::size_t step = 0; step < count; ++step) {
          EXPECT_FALSE(delivered);
          delivered = accept(receiver, datagrams[order[step]], step);
          EXPECT_EQ(receiver.incomplete_packets(),
                    (step + 1 == count || count == 1 ? 0 : 1));
        }
        ASSERT_TRUE(delivered);
        EXPECT_EQ(delivered->kind, kind);
        EXPECT_EQ(delivered->key_position, 0xfeedfacecafebeefULL);
        EXPECT_EQ(delivered->bytes, original);
        EXPECT_EQ(receiver.reassembly_bytes(), 0);
      }
    }
  }
}

TEST_F(MediaReceiverReassembly, WaitsForEveryFragment) {
  const auto original = pattern(max_source_datagram_size, 3);
  const auto datagrams = fragments_of(MediaKind::video, 0, 5, original, 0);
  MediaReceiver receiver{crypto, key.clone(), connection};
  for (std::size_t index = 0; index + 1 < datagrams.size(); ++index) {
    EXPECT_FALSE(accept(receiver, datagrams[index], 0));
    EXPECT_EQ(receiver.incomplete_packets(), 1);
    EXPECT_EQ(receiver.reassembly_bytes(), max_source_datagram_size);
  }
  const auto packet = accept(receiver, datagrams.back(), 0);
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->bytes, original);
}

TEST_F(MediaReceiverReassembly, NeverRedeliversACompletedPacket) {
  const auto original = pattern(2000, 3);
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    EXPECT_FALSE(
        accept(receiver,
               fragment_datagram(MediaKind::video, 7, 1, original, 0, 0), 0));
    EXPECT_TRUE(
        accept(receiver,
               fragment_datagram(MediaKind::video, 7, 1, original, 1, 1), 0));
    EXPECT_TRUE(fatal(receiver,
                      fragment_datagram(MediaKind::video, 7, 1, original, 0, 2),
                      0));
    EXPECT_EQ(receiver.incomplete_packets(), 0);
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    EXPECT_TRUE(accept(receiver, single(3, 0), 0));
    EXPECT_TRUE(fatal(receiver, single(3, 1), 0));
    EXPECT_EQ(receiver.incomplete_packets(), 0);
    EXPECT_TRUE(accept(receiver, single(4, 2), 0));
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    EXPECT_TRUE(accept(receiver, single(5, 0), 0));
    EXPECT_FALSE(accept(receiver, single(5, 0), 0));
    EXPECT_TRUE(accept(receiver, single(6, 1), 0));
    EXPECT_TRUE(accept(receiver, single(2, 2), 0));
    EXPECT_TRUE(accept(receiver, single(3, 3), 0));
  }
}

TEST_F(MediaReceiverReassembly, NeverReallocatesAnEvictedPacket) {
  const auto original = pattern(2000, 3);
  MediaReceiver receiver{
      crypto, key.clone(), connection,
      MediaReceiverLimits{1, max_reassembly_bytes, max_reassembly_age_us}};
  EXPECT_FALSE(accept(
      receiver, fragment_datagram(MediaKind::video, 0, 1, original, 0, 0), 0));
  EXPECT_FALSE(accept(
      receiver, fragment_datagram(MediaKind::video, 1, 1, original, 0, 1), 0));
  EXPECT_FALSE(accept(
      receiver, fragment_datagram(MediaKind::video, 0, 1, original, 0, 2), 0));
  EXPECT_EQ(receiver.incomplete_packets(), 1);
  EXPECT_FALSE(accept(
      receiver, fragment_datagram(MediaKind::video, 0, 1, original, 1, 3), 0));
  EXPECT_EQ(receiver.incomplete_packets(), 1);
  const auto done = accept(
      receiver, fragment_datagram(MediaKind::video, 1, 1, original, 1, 4), 0);
  ASSERT_TRUE(done);
  EXPECT_EQ(done->bytes, original);
  EXPECT_FALSE(accept(
      receiver, fragment_datagram(MediaKind::video, 0, 1, original, 1, 5), 0));
  EXPECT_EQ(receiver.incomplete_packets(), 0);
}

TEST_F(MediaReceiverLifetime, WipesItsKeyWhenDestroyed) {
  static_assert(!std::is_copy_constructible_v<MediaReceiver>);
  alignas(MediaReceiver) std::array<std::byte, sizeof(MediaReceiver)> storage{};
  const auto holds_key = [&] {
    const std::span<const std::byte> secret{key.data(), key.size()};
    return !std::ranges::search(storage, secret).empty();
  };
  const auto original = pattern(3000, 1);
  auto* receiver =
      new (storage.data()) MediaReceiver{crypto, key.clone(), connection};
  EXPECT_TRUE(holds_key());
  EXPECT_FALSE(accept(
      *receiver, fragment_datagram(MediaKind::video, 0, 1, original, 0, 0), 0));
  EXPECT_FALSE(accept(*receiver, probe(1), 0));
  EXPECT_EQ(receiver->incomplete_packets(), 1);
  EXPECT_TRUE(receiver->next_deadline_us());
  std::destroy_at(receiver);
  EXPECT_FALSE(holds_key());
}

}  // namespace
}  // namespace net_kakusan_test
