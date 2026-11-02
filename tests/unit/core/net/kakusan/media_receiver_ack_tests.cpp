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

class MediaReceiverDatagrams : public MediaReceiverTest {};
class MediaReceiverAcks : public MediaReceiverTest {};

TEST_F(MediaReceiverDatagrams, DropDuplicatesAndOldSequences) {
  MediaReceiver receiver{crypto, key.clone(), connection};
  const auto first = single(0, 0);
  EXPECT_TRUE(accept(receiver, first, 0));
  EXPECT_FALSE(accept(receiver, first, 1));
  EXPECT_FALSE(accept(receiver, first, 2));
  EXPECT_EQ(decoded_ack(receiver.poll(25 * millisecond)).ranges,
            ranges({{0, 0}}));

  EXPECT_TRUE(accept(receiver, single(1, 1), 30 * millisecond));
  EXPECT_TRUE(accept(receiver, single(2, 2), 30 * millisecond));
  EXPECT_FALSE(accept(receiver, first, 31 * millisecond));
  EXPECT_FALSE(accept(receiver, single(1, 1), 31 * millisecond));
  const auto ack = decoded_ack(receiver.poll(32 * millisecond));
  EXPECT_EQ(ack.ranges, ranges({{0, 2}}));
  expect_quiet(receiver, 10000 * millisecond);
  EXPECT_FALSE(accept(receiver, single(1, 1), 40 * millisecond));
  EXPECT_FALSE(accept(receiver, single(0, 0), 40 * millisecond));
  expect_quiet(receiver, 10000 * millisecond);
}

TEST_F(MediaReceiverDatagrams, StayWithinA2048SequenceWindow) {
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    EXPECT_FALSE(accept(receiver, probe(5000), 0));
    EXPECT_EQ(decoded_ack(receiver.poll(0)).ranges, ranges({{5000, 5000}}));

    EXPECT_FALSE(accept(receiver, probe(5000 - 2048), 1));
    EXPECT_FALSE(accept(receiver, probe(5000 - 2049), 1));
    EXPECT_FALSE(accept(receiver, probe(0), 1));
    expect_quiet(receiver, 1000 * millisecond);

    EXPECT_FALSE(accept(receiver, probe(5000 - 2047), 2));
    const auto ack = decoded_ack(receiver.poll(2));
    EXPECT_EQ(ack.largest, 5000);
    EXPECT_EQ(ack.ranges, ranges({{5000, 5000}, {2953, 2953}}));

    EXPECT_FALSE(accept(receiver, probe(5000 - 2047), 3));
    expect_quiet(receiver, 1000 * millisecond);

    EXPECT_FALSE(accept(receiver, probe(5001), 4));
    EXPECT_EQ(decoded_ack(receiver.poll(4 + 25 * millisecond)).ranges,
              ranges({{5000, 5001}}));
    EXPECT_FALSE(accept(receiver, probe(2953), 5 + 25 * millisecond));
    expect_quiet(receiver, 1000 * millisecond);
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    EXPECT_FALSE(accept(receiver, probe(9), 0));
    EXPECT_FALSE(accept(receiver, probe(9 + 2048), 0));
    EXPECT_FALSE(accept(receiver, probe(9 + 2048), 0));
    EXPECT_FALSE(accept(receiver, probe(9 + 2047), 0));
    EXPECT_EQ(decoded_ack(receiver.poll(0)).ranges, ranges({{2056, 2057}}));
    EXPECT_FALSE(accept(receiver, probe(9), 1));
    expect_quiet(receiver, 1000 * millisecond);
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    EXPECT_FALSE(accept(receiver, probe(100), 0));
    EXPECT_FALSE(accept(receiver, probe(100 + 4096), 0));
    EXPECT_EQ(decoded_ack(receiver.poll(0)).ranges, ranges({{4196, 4196}}));
    EXPECT_FALSE(accept(receiver, probe(100), 1));
    EXPECT_FALSE(accept(receiver, probe(100 + 2048), 1));
    expect_quiet(receiver, 1000 * millisecond);
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    EXPECT_FALSE(accept(receiver, probe(3), 0));
    EXPECT_FALSE(accept(receiver, probe(1), 0));
    EXPECT_FALSE(accept(receiver, probe(0), 0));
    EXPECT_EQ(decoded_ack(receiver.poll(0)).ranges, ranges({{3, 3}, {0, 1}}));
  }
}

TEST_F(MediaReceiverAcks, CoverProbesWithoutReassemblyState) {
  MediaReceiver receiver{crypto, key.clone(), connection};
  EXPECT_FALSE(accept(receiver, probe(0), 100));
  EXPECT_EQ(receiver.incomplete_packets(), 0);
  EXPECT_EQ(receiver.reassembly_bytes(), 0);
  EXPECT_EQ(*receiver.next_deadline_us(), 100 + 25 * millisecond);
  EXPECT_FALSE(receiver.poll(100 + 25 * millisecond - 1));
  const auto ack = decoded_ack(receiver.poll(100 + 25 * millisecond));
  EXPECT_EQ(ack.largest, 0);
  EXPECT_EQ(ack.delay_us, 25 * millisecond);
  EXPECT_EQ(ack.ranges, ranges({{0, 0}}));
  EXPECT_FALSE(accept(receiver, probe(0), 100000));
  expect_quiet(receiver, 100000);

  EXPECT_FALSE(accept(receiver, probe(1), 200000));
  EXPECT_FALSE(accept(receiver, probe(2), 200001));
  EXPECT_EQ(decoded_ack(receiver.poll(200001)).ranges, ranges({{0, 2}}));
}

TEST_F(MediaReceiverAcks, AreSentAfterTwoNewDatagrams) {
  MediaReceiver receiver{crypto, key.clone(), connection};
  EXPECT_TRUE(accept(receiver, single(0, 0), 100));
  EXPECT_FALSE(receiver.poll(100));
  EXPECT_FALSE(receiver.poll(100 + 25 * millisecond - 1));
  EXPECT_TRUE(accept(receiver, single(1, 1), 200));
  EXPECT_EQ(*receiver.next_deadline_us(), 200);
  EXPECT_FALSE(accept(receiver, single(1, 1), 210));
  const auto ack = decoded_ack(receiver.poll(250));
  EXPECT_EQ(ack.largest, 1);
  EXPECT_EQ(ack.delay_us, 50);
  EXPECT_EQ(ack.ranges, ranges({{0, 1}}));
  expect_quiet(receiver, 500 * millisecond);
}

TEST_F(MediaReceiverAcks, AreSentTwentyFiveMillisecondsAfterALoneDatagram) {
  MediaReceiver receiver{crypto, key.clone(), connection};
  EXPECT_TRUE(accept(receiver, single(0, 0), 1000));
  EXPECT_EQ(*receiver.next_deadline_us(), 1000 + 25 * millisecond);
  EXPECT_FALSE(receiver.poll(1000 + 25 * millisecond - 1));
  const auto ack = decoded_ack(receiver.poll(1000 + 25 * millisecond));
  EXPECT_EQ(ack.delay_us, 25 * millisecond);
  EXPECT_EQ(ack.ranges, ranges({{0, 0}}));
  expect_quiet(receiver, 1000 * millisecond);

  EXPECT_TRUE(accept(receiver, single(1, 1), 2000 * millisecond));
  EXPECT_EQ(*receiver.next_deadline_us(),
            2000 * millisecond + 25 * millisecond);
  const auto late = decoded_ack(receiver.poll(2100 * millisecond));
  EXPECT_EQ(late.delay_us, 100 * millisecond);
  EXPECT_EQ(late.ranges, ranges({{0, 1}}));
}

TEST_F(MediaReceiverAcks, AreSentAtOnceForGapsAndReordering) {
  MediaReceiver receiver{crypto, key.clone(), connection};
  EXPECT_TRUE(accept(receiver, single(0, 0), 0));
  EXPECT_EQ(decoded_ack(receiver.poll(25 * millisecond)).ranges,
            ranges({{0, 0}}));

  EXPECT_TRUE(accept(receiver, single(2, 2), 30 * millisecond));
  EXPECT_EQ(*receiver.next_deadline_us(), 30 * millisecond);
  const auto gap = decoded_ack(receiver.poll(30 * millisecond));
  EXPECT_EQ(gap.largest, 2);
  EXPECT_EQ(gap.delay_us, 0);
  EXPECT_EQ(gap.ranges, ranges({{2, 2}, {0, 0}}));
  expect_quiet(receiver, 1000 * millisecond);

  EXPECT_TRUE(accept(receiver, single(1, 1), 40 * millisecond));
  const auto reordered = decoded_ack(receiver.poll(40 * millisecond));
  EXPECT_EQ(reordered.largest, 2);
  EXPECT_EQ(reordered.delay_us, 10 * millisecond);
  EXPECT_EQ(reordered.ranges, ranges({{0, 2}}));
  expect_quiet(receiver, 1000 * millisecond);

  MediaReceiver late_start{crypto, key.clone(), connection};
  EXPECT_TRUE(accept(late_start, single(3, 5), 7));
  const auto first = decoded_ack(late_start.poll(7));
  EXPECT_EQ(first.ranges, ranges({{5, 5}}));

  MediaReceiver in_order{crypto, key.clone(), connection};
  EXPECT_TRUE(accept(in_order, single(0, 0), 0));
  EXPECT_FALSE(in_order.poll(0));
}

TEST_F(MediaReceiverAcks, CoverDatagramsOfEvictedPackets) {
  const auto original = pattern(2000, 6);
  MediaReceiver evicting{
      crypto, key.clone(), connection,
      MediaReceiverLimits{1, max_reassembly_bytes, max_reassembly_age_us}};
  EXPECT_FALSE(accept(
      evicting, fragment_datagram(MediaKind::video, 0, 1, original, 0, 0), 0));
  EXPECT_FALSE(accept(
      evicting, fragment_datagram(MediaKind::video, 1, 1, original, 0, 1), 1));
  EXPECT_EQ(evicting.incomplete_packets(), 1);
  EXPECT_FALSE(accept(
      evicting, fragment_datagram(MediaKind::video, 0, 1, original, 1, 2), 2));
  EXPECT_EQ(evicting.incomplete_packets(), 1);
  EXPECT_EQ(decoded_ack(evicting.poll(2)).ranges, ranges({{0, 2}}));
}

TEST_F(MediaReceiverAcks, AreRateLimited) {
  MediaReceiver receiver{crypto, key.clone(), connection};
  std::uint64_t sequence = 2;
  for (int index = 0; index < 20; ++index) {
    EXPECT_FALSE(accept(receiver, probe(sequence), 0));
    const auto ack = receiver.poll(0);
    ASSERT_TRUE(ack);
    EXPECT_EQ(decoded_ack(ack).largest, sequence);
    sequence += 2;
  }
  EXPECT_FALSE(accept(receiver, probe(sequence), 0));
  EXPECT_FALSE(receiver.poll(0));
  EXPECT_EQ(*receiver.next_deadline_us(), 5000);
  sequence += 2;
  EXPECT_FALSE(accept(receiver, probe(sequence), 100));
  EXPECT_FALSE(receiver.poll(4999));
  EXPECT_EQ(*receiver.next_deadline_us(), 5000);
  const auto limited = decoded_ack(receiver.poll(5000));
  EXPECT_EQ(limited.largest, sequence);
  EXPECT_EQ(limited.ranges.size(), 22);
  EXPECT_EQ(limited.ranges.front(), (AckRange{sequence, sequence}));
  EXPECT_EQ(limited.delay_us, 4900);
  EXPECT_FALSE(receiver.poll(5000));
  expect_quiet(receiver, 5000);

  EXPECT_FALSE(accept(receiver, probe(sequence + 2), 5001));
  EXPECT_FALSE(receiver.poll(5001));
  EXPECT_EQ(*receiver.next_deadline_us(), 10000);
  EXPECT_FALSE(receiver.poll(9999));
  EXPECT_TRUE(receiver.poll(10000));

  MediaReceiver rested{crypto, key.clone(), connection};
  sequence = 2;
  for (int index = 0; index < 20; ++index) {
    EXPECT_FALSE(accept(rested, probe(sequence), 0));
    EXPECT_TRUE(rested.poll(0));
    sequence += 2;
  }
  const auto later = 60'000 * millisecond;
  for (int index = 0; index < 20; ++index) {
    EXPECT_FALSE(accept(rested, probe(sequence), later));
    EXPECT_TRUE(rested.poll(later));
    sequence += 2;
  }
  EXPECT_FALSE(accept(rested, probe(sequence), later));
  EXPECT_FALSE(rested.poll(later));
  EXPECT_EQ(*rested.next_deadline_us(), later + 5000);
}

TEST_F(MediaReceiverAcks, DescribeTheReceivedRanges) {
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    for (const std::uint64_t sequence :
         {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2},
          std::uint64_t{4}, std::uint64_t{5}, std::uint64_t{9},
          std::uint64_t{12}, std::uint64_t{13}, std::uint64_t{14},
          std::uint64_t{15}})
      EXPECT_FALSE(accept(receiver, probe(sequence), 0));
    const auto ack = decoded_ack(receiver.poll(0));
    EXPECT_EQ(ack.largest, 15);
    EXPECT_EQ(ack.ranges, ranges({{12, 15}, {9, 9}, {4, 5}, {0, 2}}));
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    for (std::uint64_t sequence = 0; sequence <= 80; sequence += 2)
      EXPECT_FALSE(accept(receiver, probe(sequence), 0));
    const auto ack = decoded_ack(receiver.poll(0));
    EXPECT_EQ(ack.largest, 80);
    EXPECT_EQ(ack.ranges.size(), max_ack_ranges);
    EXPECT_EQ(ack.ranges.front(), (AckRange{80, 80}));
    EXPECT_EQ(ack.ranges.back(), (AckRange{18, 18}));
    for (std::size_t index = 0; index < ack.ranges.size(); ++index)
      EXPECT_EQ(ack.ranges[index].high, 80 - 2 * index);
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    for (std::uint64_t sequence = 0; sequence <= 62; sequence += 2)
      EXPECT_FALSE(accept(receiver, probe(sequence), 0));
    const auto ack = decoded_ack(receiver.poll(0));
    EXPECT_EQ(ack.ranges.size(), max_ack_ranges);
    EXPECT_EQ(ack.ranges.back(), (AckRange{0, 0}));
  }
  {
    MediaReceiver receiver{crypto, key.clone(), connection};
    for (std::uint64_t sequence = 0; sequence < 3000; ++sequence)
      EXPECT_FALSE(accept(receiver, probe(sequence), 0));
    const auto ack = decoded_ack(receiver.poll(0));
    EXPECT_EQ(ack.largest, 2999);
    EXPECT_EQ(ack.ranges, ranges({{2999 - 2047, 2999}}));
  }
}

TEST_F(MediaReceiverAcks, ReportACappedDelay) {
  const auto delay_after = [this](std::uint64_t arrival,
                                  std::uint64_t emission) {
    MediaReceiver receiver{crypto, key.clone(), connection};
    EXPECT_FALSE(accept(receiver, probe(0), arrival));
    return decoded_ack(receiver.poll(emission)).delay_us;
  };
  EXPECT_EQ(delay_after(0, 25 * millisecond), 25 * millisecond);
  EXPECT_EQ(delay_after(1'000'000, 1'030'000), 30'000);
  EXPECT_EQ(delay_after(0, 0xfffffffeULL), 0xfffffffeU);
  EXPECT_EQ(delay_after(0, 0xffffffffULL), 0xffffffffU);
  EXPECT_EQ(delay_after(0, 0x100000000ULL), 0xffffffffU);
  EXPECT_EQ(delay_after(5, 0xfffffffeULL + 5), 0xfffffffeU);
  EXPECT_EQ(delay_after(5, 0x100000000ULL + 5), 0xffffffffU);
  EXPECT_EQ(delay_after(0, max_u64 - 1), 0xffffffffU);

  MediaReceiver receiver{crypto, key.clone(), connection};
  EXPECT_FALSE(accept(receiver, probe(0), 10 * millisecond));
  EXPECT_FALSE(accept(receiver, probe(1), 12 * millisecond));
  EXPECT_EQ(decoded_ack(receiver.poll(20 * millisecond)).delay_us,
            8 * millisecond);
}

TEST_F(MediaReceiverAcks, AreNotRepeatedPeriodically) {
  MediaReceiver receiver{crypto, key.clone(), connection};
  EXPECT_FALSE(accept(receiver, probe(0), 0));
  EXPECT_TRUE(receiver.poll(25 * millisecond));
  for (std::uint64_t now = 26 * millisecond; now < 10'000 * millisecond;
       now += 777 * millisecond)
    expect_quiet(receiver, now);
}

}  // namespace
}  // namespace net_kakusan_test
