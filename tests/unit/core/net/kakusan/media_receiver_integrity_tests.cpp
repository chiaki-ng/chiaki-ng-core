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

class MediaReceiverIntegrity : public MediaReceiverTest {};

TEST_F(MediaReceiverIntegrity, IgnoresUnauthenticatedDatagrams) {
  const auto original = pattern(2000, 1);
  const auto datagrams = fragments_of(MediaKind::video, 4, 77, original, 0);
  MediaReceiver receiver{crypto, key.clone(), connection};

  const auto wrong_key = sealed_fragment(
      make_key(9), connection, MediaKind::video, 4, 77, original, 0, 0);
  const auto wrong_connection = sealed_fragment(
      key, make_connection(0x11), MediaKind::video, 4, 77, original, 0, 0);
  auto bad_tag = datagrams[0];
  bad_tag.back() ^= std::byte{1};
  auto bad_body = datagrams[0];
  bad_body[70] ^= std::byte{1};
  auto bad_prefix = datagrams[0];
  bad_prefix[0] = std::byte{'X'};
  auto bad_version = datagrams[0];
  bad_version[4] = std::byte{2};
  const Bytes truncated(datagrams[0].begin(), datagrams[0].begin() + 75);
  const Bytes oversized(max_datagram_size + 1, std::byte{});
  const Bytes empty;
  const auto only_tag = Bytes(media_header_size + media_tag_size, std::byte{});

  const std::vector<Bytes> dropped{
      wrong_key,   wrong_connection, bad_tag,   bad_body, bad_prefix,
      bad_version, truncated,        oversized, empty,    only_tag};
  for (const auto& datagram : dropped) {
    EXPECT_FALSE(accept(receiver, datagram, 5));
    EXPECT_EQ(receiver.incomplete_packets(), 0);
    EXPECT_EQ(receiver.reassembly_bytes(), 0);
    expect_quiet(receiver, 10 * millisecond * 1000);
  }

  EXPECT_FALSE(accept(receiver, datagrams[0], 7));
  EXPECT_EQ(receiver.incomplete_packets(), 1);
  const auto packet = accept(receiver, datagrams[1], 8);
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->bytes, original);

  MediaReceiver other{crypto, key.clone(), connection};
  EXPECT_FALSE(accept(other, probe(0), 0));
  EXPECT_FALSE(accept(other, wrong_key, 0));
  EXPECT_FALSE(accept(other, wrong_connection, 0));
  const auto ack = decoded_ack(other.poll(25 * millisecond));
  EXPECT_EQ(ack.ranges, ranges({{0, 0}}));
}

TEST_F(MediaReceiverIntegrity, TreatsAuthenticatedGarbageAsFatal) {
  const auto original = pattern(3000, 2);
  const auto good = fragment_datagram(MediaKind::video, 1, 9, original, 0, 0);
  const auto zero_probe = probe(0);

  const std::vector<Bytes> invalid{
      reseal(good, 6, 1),        reseal(good, 7, 1),
      reseal(good, 5, 3),        reseal(good, 5, 255),
      reseal(good, 43, 2),       reseal(good, 40, 1),
      reseal(good, 58, 1),       reseal(good, 59, 1),
      reseal(good, 52, 0x20),    reseal(good, 54, 1),
      reseal(good, 55, 1),       reseal(good, 56, 0),
      reseal(good, 57, 0),       reseal(zero_probe, 5, 0),
      reseal(zero_probe, 33, 1), reseal(zero_probe, 44, 1),
      reseal(zero_probe, 53, 1),
  };
  for (const auto& datagram : invalid) {
    MediaReceiver receiver{crypto, key.clone(), connection};
    EXPECT_TRUE(fatal(receiver, datagram, 0));
    EXPECT_EQ(receiver.incomplete_packets(), 0);
    expect_quiet(receiver, 1000 * millisecond);
    EXPECT_FALSE(accept(receiver, good, 1));
    EXPECT_EQ(receiver.incomplete_packets(), 1);
  }

  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    Bytes longer =
        fragment_datagram(MediaKind::video, 1, 9, pattern(2000, 2), 1, 0);
    longer.insert(longer.end() - media_tag_size, std::byte{0});
    const auto mac = libchiaking::net::kakusan::Hash::hmac_sha256(
        crypto, key,
        std::span<const std::byte>{longer}.first(longer.size() -
                                                 media_tag_size));
    ASSERT_TRUE(mac);
    std::copy_n(mac.value().begin(), media_tag_size,
                longer.end() - media_tag_size);
    EXPECT_TRUE(fatal(receiver, longer, 0));
    expect_quiet(receiver, 0);
  }

  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    constexpr auto oversized = max_source_datagram_size + 1;
    auto header = reseal(good, 52, static_cast<std::uint8_t>(oversized >> 8U));
    header = reseal(header, 53, static_cast<std::uint8_t>(oversized & 0xffU));
    EXPECT_TRUE(fatal(receiver, header, 0));
  }
}

TEST_F(MediaReceiverIntegrity, FailsClosedOnExhaustedCounters) {
  MediaReceiver receiver{crypto, key.clone(), connection};
  EXPECT_TRUE(fatal(receiver, probe(max_u64), 0));
  expect_quiet(receiver, 0);
  const auto original = pattern(10, 1);
  EXPECT_TRUE(fatal(
      receiver, fragment_datagram(MediaKind::video, max_u64, 1, original, 0, 0),
      0));
  EXPECT_TRUE(fatal(
      receiver, fragment_datagram(MediaKind::video, 1, 1, original, 0, max_u64),
      0));
  expect_quiet(receiver, 0);
  EXPECT_TRUE(accept(
      receiver,
      fragment_datagram(MediaKind::video, max_u64 - 1, 1, original, 0, 0), 0));
  EXPECT_FALSE(accept(receiver, probe(max_u64 - 1), 0));
}

TEST_F(MediaReceiverIntegrity, TreatsConflictingMetadataAsFatal) {
  const auto original = pattern(2200, 1);
  const auto longer = pattern(3300, 1);
  const auto first = [&](MediaReceiver& receiver) {
    EXPECT_FALSE(
        accept(receiver,
               fragment_datagram(MediaKind::video, 5, 100, original, 0, 0), 0));
  };
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    first(receiver);
    EXPECT_TRUE(
        fatal(receiver,
              fragment_datagram(MediaKind::video, 5, 101, original, 1, 1), 0));
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    first(receiver);
    EXPECT_TRUE(
        fatal(receiver,
              fragment_datagram(MediaKind::audio, 5, 100, original, 1, 1), 0));
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    first(receiver);
    EXPECT_TRUE(fatal(receiver,
                      fragment_datagram(MediaKind::video, 5, 100, longer, 1, 1),
                      0));
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    first(receiver);
    EXPECT_TRUE(fatal(receiver,
                      fragment_datagram(MediaKind::video, 5, 100, longer, 2, 1),
                      0));
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    first(receiver);
    const auto packet =
        accept(receiver,
               fragment_datagram(MediaKind::video, 5, 100, original, 1, 1), 0);
    ASSERT_TRUE(packet);
    EXPECT_EQ(packet->bytes, original);
  }
}

TEST_F(MediaReceiverIntegrity,
       TreatsARepeatedFragmentUnderANewSequenceAsFatal) {
  const auto original = pattern(4000, 1);
  MediaReceiver receiver{crypto, key.clone(), connection};
  EXPECT_FALSE(
      accept(receiver,
             fragment_datagram(MediaKind::video, 5, 100, original, 0, 0), 0));
  EXPECT_FALSE(
      accept(receiver,
             fragment_datagram(MediaKind::video, 5, 100, original, 0, 0), 0));
  EXPECT_EQ(receiver.incomplete_packets(), 1);
  EXPECT_TRUE(fatal(receiver,
                    fragment_datagram(MediaKind::video, 5, 100, original, 0, 1),
                    0));

  MediaReceiver overlapping{crypto, key.clone(), connection};
  EXPECT_FALSE(
      accept(overlapping,
             fragment_datagram(MediaKind::video, 5, 100, original, 1, 0), 0));
  auto changed = original;
  changed[1124] ^= std::byte{1};
  EXPECT_TRUE(fatal(overlapping,
                    fragment_datagram(MediaKind::video, 5, 100, changed, 1, 1),
                    0));
}

}  // namespace
}  // namespace net_kakusan_test
