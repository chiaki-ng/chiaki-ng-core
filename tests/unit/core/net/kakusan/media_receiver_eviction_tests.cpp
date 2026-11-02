#include "src/core/net/kakusan/media_receiver.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/kakusan/media_receiver_rig.hpp"

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

class MediaReceiverEviction : public MediaReceiverTest {};

TEST_F(MediaReceiverEviction, DropsTheOldestBeyondSixtyFourPackets) {
  const auto original = pattern(2000, 1);
  MediaReceiver receiver{crypto, key.clone(), connection};
  std::uint64_t datagram = 0;
  for (std::uint64_t packet = 0; packet < max_incomplete_packets; ++packet)
    EXPECT_FALSE(accept(receiver,
                        fragment_datagram(MediaKind::video, packet, packet,
                                          original, 0, datagram++),
                        packet));
  EXPECT_EQ(receiver.incomplete_packets(), 64);
  EXPECT_EQ(receiver.reassembly_bytes(), 64 * 2000);

  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 64, 64, original, 0, datagram++),
      64));
  EXPECT_EQ(receiver.incomplete_packets(), 64);
  EXPECT_EQ(receiver.reassembly_bytes(), 64 * 2000);

  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 0, 0, original, 1, datagram++), 65));
  EXPECT_EQ(receiver.incomplete_packets(), 64);

  const auto delivered = accept(
      receiver,
      fragment_datagram(MediaKind::video, 1, 1, original, 1, datagram++), 66);
  ASSERT_TRUE(delivered);
  EXPECT_EQ(delivered->bytes, original);
  EXPECT_EQ(delivered->key_position, 1);
  EXPECT_EQ(receiver.incomplete_packets(), 63);

  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 65, 65, original, 0, datagram++),
      67));
  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 66, 66, original, 0, datagram++),
      67));
  EXPECT_EQ(receiver.incomplete_packets(), 64);
  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 2, 2, original, 1, datagram++), 68));
  EXPECT_EQ(receiver.incomplete_packets(), 64);
  EXPECT_TRUE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 4, 4, original, 1, datagram++), 69));

  static_assert(max_incomplete_packets * max_source_datagram_size <=
                max_reassembly_bytes);
}

TEST_F(MediaReceiverEviction, DropsTheOldestBeyondTheByteBound) {
  static_assert(64 * max_source_datagram_size < max_reassembly_bytes);
  const auto original = pattern(max_source_datagram_size, 4);
  MediaReceiver full{crypto, key.clone(), connection};
  std::uint64_t datagram = 0;
  for (std::uint64_t packet = 0; packet < 80; ++packet)
    EXPECT_FALSE(accept(
        full,
        fragment_datagram(MediaKind::video, packet, 0, original, 0, datagram++),
        packet));
  EXPECT_EQ(full.incomplete_packets(), 64);
  EXPECT_EQ(full.reassembly_bytes(), 64 * max_source_datagram_size);
  EXPECT_LE(full.reassembly_bytes(), max_reassembly_bytes);

  const auto small = pattern(2000, 5);
  MediaReceiver receiver{
      crypto, key.clone(), connection,
      MediaReceiverLimits{64, 3 * 2000, max_reassembly_age_us}};
  datagram = 0;
  for (std::uint64_t packet = 0; packet < 3; ++packet)
    EXPECT_FALSE(accept(
        receiver,
        fragment_datagram(MediaKind::video, packet, 0, small, 0, datagram++),
        0));
  EXPECT_EQ(receiver.incomplete_packets(), 3);
  EXPECT_EQ(receiver.reassembly_bytes(), 6000);
  EXPECT_FALSE(accept(
      receiver, fragment_datagram(MediaKind::video, 3, 0, small, 0, datagram++),
      0));
  EXPECT_EQ(receiver.incomplete_packets(), 3);
  EXPECT_EQ(receiver.reassembly_bytes(), 6000);
  EXPECT_FALSE(accept(
      receiver, fragment_datagram(MediaKind::video, 0, 0, small, 1, datagram++),
      0));
  EXPECT_EQ(receiver.incomplete_packets(), 3);
  EXPECT_TRUE(accept(
      receiver, fragment_datagram(MediaKind::video, 1, 0, small, 1, datagram++),
      0));
  EXPECT_EQ(receiver.incomplete_packets(), 2);
  EXPECT_EQ(receiver.reassembly_bytes(), 4000);

  const auto big = pattern(5000, 8);
  EXPECT_FALSE(accept(
      receiver, fragment_datagram(MediaKind::video, 10, 0, big, 0, datagram++),
      0));
  EXPECT_EQ(receiver.incomplete_packets(), 1);
  EXPECT_EQ(receiver.reassembly_bytes(), 5000);

  const auto huge = pattern(7000, 8);
  EXPECT_FALSE(accept(
      receiver, fragment_datagram(MediaKind::video, 11, 0, huge, 0, datagram++),
      0));
  EXPECT_EQ(receiver.incomplete_packets(), 0);
  EXPECT_EQ(receiver.reassembly_bytes(), 0);
  EXPECT_FALSE(accept(
      receiver, fragment_datagram(MediaKind::video, 11, 0, huge, 1, datagram++),
      0));
  EXPECT_EQ(receiver.incomplete_packets(), 0);

  MediaReceiver clamped{crypto, key.clone(), connection,
                        MediaReceiverLimits{1000, 100 * max_reassembly_bytes,
                                            10 * max_reassembly_age_us}};
  datagram = 0;
  for (std::uint64_t packet = 0; packet < 100; ++packet)
    EXPECT_FALSE(accept(
        clamped,
        fragment_datagram(MediaKind::video, packet, 0, small, 0, datagram++),
        0));
  EXPECT_EQ(clamped.incomplete_packets(), 64);
  EXPECT_FALSE(accept(
      clamped,
      fragment_datagram(MediaKind::video, 200, 0, small, 0, datagram++), 0));
}

TEST_F(MediaReceiverEviction, ExpiresPartialsAfterOneHundredMilliseconds) {
  const auto original = pattern(3000, 2);
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    EXPECT_FALSE(accept(
        receiver, fragment_datagram(MediaKind::video, 0, 1, original, 0, 0),
        1000));
    EXPECT_EQ(*receiver.next_deadline_us(), 1000 + 25 * millisecond);
    EXPECT_TRUE(receiver.poll(1000 + 25 * millisecond));
    EXPECT_EQ(*receiver.next_deadline_us(), 1000 + 100 * millisecond);
    const auto packet = accept(
        receiver, fragment_datagram(MediaKind::video, 0, 1, original, 1, 1),
        1000 + 100 * millisecond - 1);
    EXPECT_FALSE(packet);
    const auto done = accept(
        receiver, fragment_datagram(MediaKind::video, 0, 1, original, 2, 2),
        1000 + 100 * millisecond - 1);
    ASSERT_TRUE(done);
    EXPECT_EQ(done->bytes, original);
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    EXPECT_FALSE(accept(
        receiver, fragment_datagram(MediaKind::video, 0, 1, original, 0, 0),
        1000));
    EXPECT_FALSE(accept(
        receiver, fragment_datagram(MediaKind::video, 0, 1, original, 1, 1),
        1000 + 50 * millisecond));
    EXPECT_EQ(receiver.incomplete_packets(), 1);
    EXPECT_FALSE(accept(
        receiver, fragment_datagram(MediaKind::video, 0, 1, original, 2, 2),
        1000 + 100 * millisecond));
    EXPECT_EQ(receiver.incomplete_packets(), 0);
    EXPECT_EQ(receiver.reassembly_bytes(), 0);
    EXPECT_FALSE(accept(
        receiver, fragment_datagram(MediaKind::video, 0, 1, original, 2, 3),
        1000 + 200 * millisecond));
    EXPECT_EQ(receiver.incomplete_packets(), 0);
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    EXPECT_FALSE(
        accept(receiver,
               fragment_datagram(MediaKind::video, 0, 1, original, 0, 0), 0));
    EXPECT_FALSE(accept(
        receiver, fragment_datagram(MediaKind::video, 1, 1, original, 0, 1),
        60 * millisecond));
    EXPECT_EQ(receiver.incomplete_packets(), 2);
    EXPECT_TRUE(receiver.poll(60 * millisecond));
    EXPECT_FALSE(receiver.poll(99'999));
    EXPECT_EQ(receiver.incomplete_packets(), 2);
    EXPECT_FALSE(receiver.poll(100 * millisecond));
    EXPECT_EQ(receiver.incomplete_packets(), 1);
    EXPECT_EQ(receiver.reassembly_bytes(), 3000);
    EXPECT_FALSE(receiver.poll(160 * millisecond));
    EXPECT_EQ(receiver.incomplete_packets(), 0);
    EXPECT_EQ(receiver.reassembly_bytes(), 0);
    const auto ack = receiver.poll(160 * millisecond);
    EXPECT_FALSE(ack);
    EXPECT_FALSE(receiver.next_deadline_us());
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    EXPECT_FALSE(
        accept(receiver,
               fragment_datagram(MediaKind::video, 0, 1, original, 0, 0), 0));
    EXPECT_FALSE(accept(
        receiver, fragment_datagram(MediaKind::video, 1, 1, original, 0, 1),
        10 * millisecond));
    EXPECT_FALSE(accept(
        receiver, fragment_datagram(MediaKind::video, 0, 1, original, 1, 2),
        100 * millisecond));
    EXPECT_EQ(receiver.incomplete_packets(), 1);
    EXPECT_FALSE(accept(
        receiver, fragment_datagram(MediaKind::video, 0, 1, original, 2, 3),
        101 * millisecond));
    EXPECT_EQ(receiver.incomplete_packets(), 1);
  }
}

TEST_F(MediaReceiverEviction, DropsPartialsThatLeaveThePacketWindow) {
  const auto original = pattern(2000, 3);
  MediaReceiver receiver{crypto, key.clone(), connection};
  std::uint64_t datagram = 0;
  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 0, 1, original, 0, datagram++), 0));
  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 1, 1, original, 0, datagram++), 0));
  EXPECT_EQ(receiver.incomplete_packets(), 2);
  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 2048, 1, original, 0, datagram++),
      0));
  EXPECT_EQ(receiver.incomplete_packets(), 2);
  EXPECT_EQ(receiver.reassembly_bytes(), 4000);

  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 0, 1, original, 1, datagram++), 0));
  EXPECT_EQ(receiver.incomplete_packets(), 2);

  const auto done = accept(
      receiver,
      fragment_datagram(MediaKind::video, 1, 1, original, 1, datagram++), 0);
  ASSERT_TRUE(done);
  EXPECT_EQ(done->bytes, original);

  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 2049, 1, original, 0, datagram++),
      0));
  EXPECT_EQ(receiver.incomplete_packets(), 2);
  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 5000, 1, original, 0, datagram++),
      0));
  EXPECT_EQ(receiver.incomplete_packets(), 1);
  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 2048, 1, original, 1, datagram++),
      0));
  EXPECT_EQ(receiver.incomplete_packets(), 1);
  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 2952, 1, original, 0, datagram++),
      0));
  EXPECT_EQ(receiver.incomplete_packets(), 1);
  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 2951, 1, original, 0, datagram++),
      0));
  EXPECT_EQ(receiver.incomplete_packets(), 1);
}

TEST_F(MediaReceiverEviction, AllocatesOnlyInsideThePacketWindow) {
  const auto original = pattern(2000, 3);
  MediaReceiver receiver{crypto, key.clone(), connection};
  std::uint64_t datagram = 0;
  EXPECT_FALSE(accept(
      receiver,
      fragment_datagram(MediaKind::video, 5000, 1, original, 0, datagram++),
      0));
  EXPECT_FALSE(accept(receiver,
                      fragment_datagram(MediaKind::video, 5000 - 2047, 1,
                                        original, 0, datagram++),
                      0));
  EXPECT_EQ(receiver.incomplete_packets(), 2);
  EXPECT_FALSE(accept(receiver,
                      fragment_datagram(MediaKind::video, 5000 - 2048, 1,
                                        original, 0, datagram++),
                      0));
  EXPECT_FALSE(accept(receiver,
                      fragment_datagram(MediaKind::video, 5000 - 2049, 1,
                                        original, 0, datagram++),
                      0));
  EXPECT_EQ(receiver.incomplete_packets(), 2);
  EXPECT_TRUE(accept(receiver,
                     fragment_datagram(MediaKind::video, 5000 - 2047, 1,
                                       original, 1, datagram++),
                     0));
  EXPECT_EQ(receiver.incomplete_packets(), 1);
}

}  // namespace
}  // namespace net_kakusan_test
