#include "tests/unit/core/net/protocol/remote_play/stream/video_assembler_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

class VideoAssembly : public VideoAssemblerRig {};

TEST_F(VideoAssembly, JoinsCompleteSourceUnitsIntoAFrame) {
  EXPECT_TRUE(pending(assembler.push(unit(7, 1, 3, 1, bytes({0, 1, 3, 4})))));
  const auto frame = assembler.push(unit(7, 0, 3, 1, bytes({0, 0, 1, 2})));
  ASSERT_TRUE(completes(frame));
  EXPECT_EQ(frame.value()->frame_index, 7U);
  EXPECT_EQ(frame.value()->codec, 0x65U);
  EXPECT_EQ(frame.value()->bytes, bytes({1, 2, 3, 4}));
  EXPECT_FALSE(assembler.push(unit(7, 0, 3, 1, bytes({0, 0, 1}))));
}

TEST_F(VideoAssembly, PermitsAVideoWordPerPacket) {
  auto first = unit(1, 0, 3, 1, bytes({0, 0, 1, 2}));
  first.video_word = 0x1234;
  auto second = unit(1, 1, 3, 1, bytes({0, 0, 3, 4}));
  second.video_word = 0xabcd;
  EXPECT_TRUE(assembler.push(first));
  const auto result = assembler.push(second);
  ASSERT_TRUE(completes(result));
  EXPECT_EQ(result.value()->bytes, bytes({1, 2, 3, 4}));
}

TEST_F(VideoAssembly, RejectsInvalidUnits) {
  auto invalid = unit(1, 0, 2, 0, bytes({0, 0, 1}));
  invalid.type = takion::PacketType{0x03};
  EXPECT_EQ(error_of(assembler.push(invalid)),
            remote_play::VideoAssemblerError::invalid_family);
  EXPECT_EQ(error_of(assembler.push(unit(1, 0, 0, 0, bytes({0, 0, 1})))),
            remote_play::VideoAssemblerError::invalid_count);
  EXPECT_EQ(error_of(assembler.push(unit(1, 0, 2, 2, bytes({0, 0, 1})))),
            remote_play::VideoAssemblerError::invalid_count);
  EXPECT_EQ(error_of(assembler.push(unit(1, 0, 513, 0, bytes({0, 0, 1})))),
            remote_play::VideoAssemblerError::invalid_count);
  EXPECT_EQ(error_of(assembler.push(unit(1, 2, 2, 0, bytes({0, 0, 1})))),
            remote_play::VideoAssemblerError::invalid_unit);
  EXPECT_EQ(error_of(assembler.push(unit(1, 0, 2, 0, bytes({0})))),
            remote_play::VideoAssemblerError::short_source);
}

TEST_F(VideoAssembly, RejectsADuplicateUnitAndStillCompletesTheFrame) {
  EXPECT_TRUE(pending(assembler.push(unit(1, 0, 2, 0, bytes({0, 0, 1})))));
  EXPECT_EQ(error_of(assembler.push(unit(1, 0, 2, 0, bytes({0, 0, 2})))),
            remote_play::VideoAssemblerError::duplicate_unit);
  const auto completed = assembler.push(unit(1, 1, 2, 0, bytes({0, 0, 2})));
  ASSERT_TRUE(completes(completed));
  EXPECT_EQ(completed.value()->bytes, bytes({1, 2}));
}

TEST_F(VideoAssembly, KeepsTheFirstUnitGeometryThroughMidFrameMetadataChanges) {
  auto first = unit(1, 0, 3, 1, bytes({0, 0, 1}));
  first.adaptive_stream_index = 0;
  EXPECT_TRUE(assembler.push(first));
  EXPECT_FALSE(assembler.push(first).has_value());
  auto changed = unit(1, 1, 4, 0, bytes({0, 0, 2}));
  changed.codec = 0x66;
  changed.adaptive_stream_index = 1;
  const auto frame = assembler.push(changed);
  ASSERT_TRUE(completes(frame));
  EXPECT_EQ(frame.value()->bytes, bytes({1, 2}));
  EXPECT_EQ(frame.value()->codec, 0x65U);
  EXPECT_EQ(frame.value()->adaptive_stream_index, 1U);
  EXPECT_FALSE(assembler.take_loss());
}

TEST_F(VideoAssembly, EndsAFrameOnTheLastUnitOfThePacketGeometry) {
  EXPECT_TRUE(completes(assembler.push(unit(1, 0, 1, 0, bytes({0, 0, 9})))));
  assembler.mark_complete(1);
  EXPECT_TRUE(pending(assembler.push(unit(2, 0, 3, 0, bytes({0, 0, 1})))));
  EXPECT_TRUE(pending(assembler.push(unit(2, 2, 4, 0, bytes({0, 0, 3})))));
  EXPECT_FALSE(assembler.take_loss());
  const auto frame = assembler.push(unit(2, 1, 3, 1, bytes({0, 0, 2})));
  ASSERT_TRUE(completes(frame));
  EXPECT_EQ(frame.value()->bytes, bytes({1, 2, 3}));
}

TEST_F(VideoAssembly, FailsTheFrameWhenARejectedUnitIsLastInItsGeometry) {
  EXPECT_TRUE(completes(assembler.push(unit(1, 0, 1, 0, bytes({0, 0, 9})))));
  assembler.mark_complete(1);
  EXPECT_TRUE(pending(assembler.push(unit(2, 1, 4, 0, bytes({0, 0, 2})))));
  EXPECT_EQ(error_of(assembler.push(unit(2, 1, 2, 0, bytes({0, 0, 2})))),
            remote_play::VideoAssemblerError::duplicate_unit);
  EXPECT_TRUE(loss_is(assembler.take_loss(), 2, 2, true));
  EXPECT_EQ(error_of(assembler.push(unit(2, 0, 4, 0, bytes({0, 0, 1})))),
            remote_play::VideoAssemblerError::stale_frame);
}

TEST_F(VideoAssembly, DiscardsAnIncompleteFrameForANewerOneAcrossTheWrap) {
  EXPECT_TRUE(assembler.push(unit(65535, 0, 2, 0, bytes({0, 0, 1}))));
  const auto wrapped = assembler.push(unit(0, 0, 1, 0, bytes({0, 0, 2})));
  ASSERT_TRUE(completes(wrapped));
  EXPECT_EQ(wrapped.value()->bytes, bytes({2}));
  EXPECT_EQ(error_of(assembler.push(unit(65535, 1, 2, 0, bytes({0, 0, 3})))),
            remote_play::VideoAssemblerError::stale_frame);
  EXPECT_EQ(error_of(assembler.push(unit(0, 0, 1, 0, bytes({0, 0, 2})))),
            remote_play::VideoAssemblerError::stale_frame);
}

TEST_F(VideoAssembly, CallsFramesStaleExactlyWhenPushRefusesThem) {
  EXPECT_FALSE(assembler.stale(5));
  EXPECT_TRUE(assembler.push(unit(5, 0, 2, 0, bytes({0, 0, 1}))));
  EXPECT_FALSE(assembler.stale(5));
  EXPECT_FALSE(assembler.stale(6));
  EXPECT_TRUE(assembler.stale(4));
  const auto frame = assembler.push(unit(5, 1, 2, 0, bytes({0, 0, 2})));
  EXPECT_TRUE(completes(frame));
  EXPECT_TRUE(assembler.stale(5));
  EXPECT_TRUE(assembler.stale(4));
  EXPECT_FALSE(assembler.stale(6));
  EXPECT_EQ(error_of(assembler.push(unit(5, 1, 2, 0, bytes({0, 0, 2})))),
            remote_play::VideoAssemblerError::stale_frame);
  EXPECT_FALSE(assembler.stale(static_cast<std::uint16_t>(5 + 0x7fff)));
}

TEST_F(VideoAssembly, RefusesUnitsWhoseDataIsStillEncrypted) {
  auto sealed = unit(5, 0, 2, 0, bytes({0, 0, 9}));
  sealed.encrypted = true;
  EXPECT_EQ(error_of(assembler.push(sealed)),
            remote_play::VideoAssemblerError::stale_frame);
  EXPECT_FALSE(assembler.last_frame_index());
  EXPECT_TRUE(assembler.push(unit(5, 0, 2, 0, bytes({0, 0, 1}))));
  sealed.unit_index = 1;
  EXPECT_EQ(error_of(assembler.push(sealed)),
            remote_play::VideoAssemblerError::stale_frame);
  EXPECT_TRUE(completes(assembler.push(unit(5, 1, 2, 0, bytes({0, 0, 2})))));
}

TEST_F(VideoAssembly, BoundsTheStorageOfOneFrame) {
  const std::vector<std::byte> data(2 + 3 * 1024 * 1024, std::byte{0x11});
  EXPECT_TRUE(assembler.push(unit(1, 0, 2, 0, data)));
  EXPECT_EQ(error_of(assembler.push(unit(1, 1, 2, 0, data))),
            remote_play::VideoAssemblerError::frame_too_large);
}

TEST_F(VideoAssembly, AssemblesMaskedZeroTotalsAndLatchesFlagsPerFrame) {
  auto first = unit(8, 0, 0, 3, bytes({0, 0, 1}));
  first.video_word = 4;
  first.suppress_presentation = true;
  auto second = unit(8, 1, 0, 3, bytes({0, 0, 2}));
  second.video_word = 6;
  second.dynamic_range_flag = true;
  EXPECT_TRUE(pending(assembler.push(first, 10)));
  const auto frame = assembler.push(second, 10);
  ASSERT_TRUE(completes(frame));
  EXPECT_EQ(frame.value()->bytes, bytes({1, 2}));
  EXPECT_TRUE(frame.value()->counts_ignored);
  EXPECT_TRUE(frame.value()->suppress_presentation);
  EXPECT_TRUE(frame.value()->dynamic_range_flag);

  auto next = unit(9, 0, 0, 0, bytes({0, 0, 5}));
  next.video_word = 10;
  const auto following = assembler.push(next, 10);
  ASSERT_TRUE(completes(following));
  EXPECT_FALSE(following.value()->suppress_presentation);
  EXPECT_FALSE(following.value()->dynamic_range_flag);
}

TEST_F(VideoAssembly, ForgetsTheFlagsOfAnAbandonedFrame) {
  ASSERT_TRUE(completes(assembler.push(unit(9, 0, 1, 0, bytes({0, 0, 5})))));
  auto stale_flagged = unit(10, 0, 2, 0, bytes({0, 0, 7}));
  stale_flagged.suppress_presentation = true;
  EXPECT_TRUE(pending(assembler.push(stale_flagged)));
  const auto frame = assembler.push(unit(11, 0, 1, 0, bytes({0, 0, 9})));
  ASSERT_TRUE(completes(frame));
  EXPECT_EQ(frame.value()->frame_index, 11U);
  EXPECT_FALSE(frame.value()->suppress_presentation);
}

TEST_F(VideoAssembly, LatchesAFlagCarriedOnlyByTheLastUnit) {
  auto last_flagged = unit(12, 1, 3, 1, bytes({0, 0, 3, 4}));
  last_flagged.suppress_presentation = true;
  EXPECT_TRUE(pending(assembler.push(unit(12, 0, 3, 1, bytes({0, 0, 1, 2})))));
  const auto frame = assembler.push(last_flagged);
  ASSERT_TRUE(completes(frame));
  EXPECT_TRUE(frame.value()->suppress_presentation);
}

}  // namespace
}  // namespace remote_play_test
