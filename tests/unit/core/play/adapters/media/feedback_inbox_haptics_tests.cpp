#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/media/counted_allocations.hpp"
#include "tests/unit/core/play/adapters/media/feedback_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
namespace {

using namespace std::chrono_literals;
namespace controllers = libchiaking::controllers;
using controllers::FeedbackIntensity;
using Block = controllers::HapticBlock;

class FeedbackInboxHaptics : public FeedbackRig {};

TEST_F(FeedbackInboxHaptics, ReblocksAudioInto256ByteBlocks) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(Block::size, 256);
  EXPECT_EQ(Block::frames, 64);
  EXPECT_EQ(haptics(inbox, 120, 0x12), Admission::held);
  EXPECT_EQ(haptics(inbox, 120, 0x12), Admission::held);
  EXPECT_FALSE(inbox.take_haptic_block(seat(0), start));
  EXPECT_EQ(haptics(inbox, 16, 0x34), Admission::held);
  const auto block = inbox.take_haptic_block(seat(0), start);
  ASSERT_TRUE(block);
  EXPECT_EQ(block->intensity(), FeedbackIntensity::strong);
  EXPECT_EQ(block->gain(), 1.0F);
  EXPECT_TRUE(filled(block->pcm().first(240), 0x12));
  EXPECT_TRUE(filled(block->pcm().last(16), 0x34));
  EXPECT_FALSE(inbox.take_haptic_block(seat(0), start));
  EXPECT_FALSE(inbox.take_haptic_block(seat(1), start));
  EXPECT_EQ(inbox.statistics().haptic_blocks_taken, 1);
  EXPECT_EQ(inbox.statistics().haptic_packets, 3);
  EXPECT_EQ(inbox.statistics().haptic_bytes, 256);
}

TEST_F(FeedbackInboxHaptics, TakesBlocksWithoutAllocating) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady(),
              Inbox::Configuration{.buffered_blocks = 4}};
  const std::vector<std::byte> pcm(HapticAudio::max_bytes, std::byte{0x21});
  const std::vector<std::byte> oversize(HapticAudio::max_bytes + 400,
                                        std::byte{0x21});
  const EncodedMedia unit{MediaKind::haptics,
                          std::vector<std::byte>(480, std::byte{0x33})};
  const auto before = CountedAllocations::everywhere();
  std::size_t blocks = 0;
  for (int round = 0; round < 8; ++round) {
    const auto typed = libchiaking::play::MediaUnits::feedback(unit);
    ASSERT_TRUE(typed);
    EXPECT_EQ(inbox.accept(*typed, start), Admission::held);
    EXPECT_EQ(
        inbox.accept({seat(1), HapticAudio{std::span{pcm}.first(1024)}}, start),
        Admission::held);
    EXPECT_EQ(inbox.accept({seat(2), HapticAudio{pcm}}, start),
              Admission::held);
    EXPECT_EQ(inbox.accept({seat(3), HapticAudio{oversize}}, start),
              Admission::malformed);
    for (std::size_t index = 0; index < SeatNumber::count; ++index)
      while (const auto block = inbox.take_haptic_block(seat(index), start)) {
        blocks += block->silent() ? 0U : 1U;
        const auto first = block->scaled_samples()[0];
        EXPECT_TRUE(first == 0x2121 || first == 0x3333) << first;
      }
    EXPECT_EQ(inbox.accept({seat(1), controllers::Rumble{1, 2}}, start),
              Admission::held);
    EXPECT_EQ(inbox.accept({seat(1), triggers()}, start), Admission::held);
    EXPECT_TRUE(inbox.take_rumble(seat(1)));
    EXPECT_TRUE(inbox.take_triggers(seat(1)));
  }
  EXPECT_EQ(CountedAllocations::everywhere(), before);
  EXPECT_EQ(blocks, 15 + 32 + 32);
  EXPECT_EQ(inbox.statistics().haptic_malformed, 8);
  EXPECT_EQ(inbox.statistics().haptic_bytes, 8 * (480 + 1024 + 4096 + 4096));
}

TEST_F(FeedbackInboxHaptics, DiscardsEverythingBufferedOnAnOverrunByDefault) {
  EXPECT_EQ(Inbox::Configuration{}.overrun, Inbox::Overrun::discard_buffered);
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(haptics(inbox, 512, 0x00), Admission::held);
  EXPECT_EQ(inbox.statistics().haptic_bytes_buffered, 512);
  EXPECT_EQ(haptics(inbox, 16, 0x34), Admission::held);
  EXPECT_FALSE(inbox.take_haptic_block(seat(0), start));
  EXPECT_EQ(inbox.statistics().haptic_bytes_overrun, 512);
  EXPECT_EQ(inbox.statistics().haptic_bytes_buffered, 16);
  EXPECT_EQ(haptics(inbox, 240, 0x56), Admission::held);
  const auto block = inbox.take_haptic_block(seat(0), start);
  ASSERT_TRUE(block);
  EXPECT_TRUE(filled(block->pcm().first(16), 0x34));
  EXPECT_TRUE(filled(block->pcm().last(240), 0x56));
}

TEST_F(FeedbackInboxHaptics, OverrunsOnAPacketLargerThanTheBuffer) {
  Inbox large{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(haptics(large, 516, 0x21), Admission::held);
  EXPECT_FALSE(large.take_haptic_block(seat(0), start));
  EXPECT_EQ(large.statistics().haptic_bytes_overrun, 512);
}

TEST_F(FeedbackInboxHaptics, BuffersAsManyBlocksAsConfigured) {
  Inbox deep{LiveProviders::threading(), LiveProviders::steady(),
             Inbox::Configuration{.buffered_blocks = 4}};
  EXPECT_EQ(haptics(deep, 1024, 0x21), Admission::held);
  for (int index = 0; index < 4; ++index) {
    SCOPED_TRACE(index);
    EXPECT_TRUE(deep.take_haptic_block(seat(0), start));
  }
  EXPECT_FALSE(deep.take_haptic_block(seat(0), start));
}

TEST_F(FeedbackInboxHaptics, CanDropOnlyTheOldestBlockOnAnOverrun) {
  Inbox inbox{
      LiveProviders::threading(), LiveProviders::steady(),
      Inbox::Configuration{.overrun = Inbox::Overrun::drop_oldest_block}};
  EXPECT_EQ(haptics(inbox, 256, 0x0a), Admission::held);
  EXPECT_EQ(haptics(inbox, 256, 0x0b), Admission::held);
  EXPECT_EQ(haptics(inbox, 256, 0x0c), Admission::held);
  EXPECT_EQ(inbox.statistics().haptic_bytes_overrun, 256);
  EXPECT_EQ(inbox.statistics().haptic_bytes_buffered, 512);
  const auto kept = inbox.take_haptic_block(seat(0), start);
  const auto newest = inbox.take_haptic_block(seat(0), start);
  ASSERT_TRUE(kept);
  EXPECT_TRUE(filled(kept->pcm(), 0x0b));
  ASSERT_TRUE(newest);
  EXPECT_TRUE(filled(newest->pcm(), 0x0c));
  EXPECT_FALSE(inbox.take_haptic_block(seat(0), start));
  EXPECT_EQ(inbox.statistics().haptic_bytes_buffered, 0);
}

TEST_F(FeedbackInboxHaptics, DiscardsBufferedAudioOlderThan40Ms) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(haptics(inbox, 256, 0x11), Admission::held);
  EXPECT_FALSE(inbox.take_haptic_block(seat(0), start + 41ms));
  EXPECT_EQ(inbox.statistics().haptic_bytes_stale, 256);
  EXPECT_FALSE(inbox.take_haptic_block(seat(0), start + 41ms));

  EXPECT_EQ(haptics(inbox, 256, 0x22, 0, start + 50ms), Admission::held);
  EXPECT_TRUE(inbox.take_haptic_block(seat(0), start + 90ms));
}

TEST_F(FeedbackInboxHaptics, KeepsOnlyTheFreshPartOfAPartialBlock) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(haptics(inbox, 128, 0x33, 0, start + 100ms), Admission::held);
  EXPECT_EQ(haptics(inbox, 128, 0x44, 0, start + 141ms), Admission::held);
  EXPECT_FALSE(inbox.take_haptic_block(seat(0), start + 141ms));
  EXPECT_EQ(haptics(inbox, 128, 0x55, 0, start + 150ms), Admission::held);
  const auto block = inbox.take_haptic_block(seat(0), start + 150ms);
  ASSERT_TRUE(block);
  EXPECT_TRUE(filled(block->pcm().first(128), 0x44));
  EXPECT_TRUE(filled(block->pcm().last(128), 0x55));
}

TEST_F(FeedbackInboxHaptics, AgesEachBlockFromWhenItArrived) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(haptics(inbox, 512, 0x66, 0, start + 200ms), Admission::held);
  EXPECT_TRUE(inbox.take_haptic_block(seat(0), start + 230ms));
  EXPECT_TRUE(inbox.take_haptic_block(seat(0), start + 260ms));
}

TEST_F(FeedbackInboxHaptics, WaitsAsLongAsConfiguredBeforeAudioGoesStale) {
  Inbox patient{LiveProviders::threading(), LiveProviders::steady(),
                Inbox::Configuration{.stale_after = 100ms}};
  EXPECT_EQ(haptics(patient, 256, 0x11), Admission::held);
  EXPECT_TRUE(patient.take_haptic_block(seat(0), start + 100ms));
}

TEST_F(FeedbackInboxHaptics, CountsAndDropsMalformedAudio) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(haptics(inbox, 0, 0), Admission::malformed);
  EXPECT_EQ(haptics(inbox, 3, 0), Admission::malformed);
  EXPECT_EQ(haptics(inbox, 4097, 0), Admission::malformed);
  EXPECT_EQ(haptics(inbox, 8192, 0), Admission::malformed);
  EXPECT_EQ(haptics(inbox, 119, 0), Admission::malformed);
  EXPECT_EQ(inbox.statistics().haptic_malformed, 5);
  EXPECT_EQ(inbox.statistics().haptic_bytes, 0 + 3 + 4096 + 4096 + 119);
  EXPECT_EQ(haptics(inbox, 4096, 0), Admission::held);
  EXPECT_EQ(inbox.statistics().haptic_packets, 6);
}

TEST_F(FeedbackInboxHaptics, KeepsAudioWithItsSeat) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(haptics(inbox, 256, 0x0a, 0), Admission::held);
  EXPECT_EQ(haptics(inbox, 256, 0x0b, 2), Admission::held);
  const auto second = inbox.take_haptic_block(seat(2), start);
  const auto first = inbox.take_haptic_block(seat(0), start);
  ASSERT_TRUE(second);
  EXPECT_TRUE(filled(second->pcm(), 0x0b));
  ASSERT_TRUE(first);
  EXPECT_TRUE(filled(first->pcm(), 0x0a));
}

TEST_F(FeedbackInboxHaptics, HandsBlocksAcrossThreadsWithoutLossOrReordering) {
  constexpr std::size_t buffered = 8;
  constexpr std::uint16_t total = 4000;
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady(),
              Inbox::Configuration{.stale_after = std::chrono::hours{1},
                                   .buffered_blocks = buffered}};
  const auto deadline = LiveProviders::steady_us() + 30'000'000U;
  auto producer = LiveProviders::spawn("haptics", [&] {
    std::vector<std::byte> pcm(Block::size);
    for (std::uint16_t index = 0; index < total; ++index) {
      while (inbox.pending(seat(0)).haptic_blocks >= buffered &&
             LiveProviders::steady_us() < deadline)
        LiveProviders::spin_pause();
      for (std::size_t offset = 0; offset < pcm.size(); offset += 2) {
        pcm[offset] = std::byte{static_cast<std::uint8_t>(index & 0xffU)};
        pcm[offset + 1] = std::byte{static_cast<std::uint8_t>(index >> 8U)};
      }
      EXPECT_EQ(inbox.accept({seat(0), HapticAudio{pcm}}, start),
                Admission::held);
    }
  });
  std::uint16_t expected = 0;
  bool ordered = true;
  while (expected < total && LiveProviders::steady_us() < deadline) {
    const auto block = inbox.take_haptic_block(seat(0), start);
    if (!block) {
      LiveProviders::spin_pause();
      continue;
    }
    const auto samples = block->scaled_samples();
    ordered = ordered && std::ranges::all_of(samples, [&](std::int16_t value) {
                return value == static_cast<std::int16_t>(expected);
              });
    ++expected;
  }
  static_cast<void>(producer.join());
  EXPECT_EQ(expected, total);
  EXPECT_TRUE(ordered);
  EXPECT_FALSE(inbox.take_haptic_block(seat(0), start));
  const auto statistics = inbox.statistics();
  EXPECT_EQ(statistics.haptic_blocks_taken, total);
  EXPECT_EQ(statistics.haptic_bytes_overrun, 0U);
  EXPECT_EQ(statistics.haptic_bytes_stale, 0U);
  EXPECT_EQ(statistics.haptic_bytes_buffered, 0U);
}

TEST_F(FeedbackInboxHaptics, DropsTheAudioOfAForgottenSeat) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(haptics(inbox, 256, 0x0c, 3), Admission::held);
  inbox.forget(seat(3));
  EXPECT_FALSE(inbox.take_haptic_block(seat(3), start));
}

}  // namespace
}  // namespace play_test
