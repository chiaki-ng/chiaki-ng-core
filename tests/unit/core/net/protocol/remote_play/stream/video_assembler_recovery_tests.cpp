#include "tests/unit/core/net/protocol/remote_play/stream/video_assembler_rig.hpp"

#include <gtest/gtest.h>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;

class VideoAssemblerRecovery : public VideoAssemblerRig {};

TEST_F(VideoAssemblerRecovery, RecoversWithTheFecCountOfTheFirstUnit) {
  EXPECT_TRUE(assembler.push(unit(1, 0, 3, 1, bytes({0, 0, 1, 2}))));
  auto changed = unit(1, 2, 3, 0, bytes({0, 0, 0x8e, 0}));
  changed.adaptive_stream_index = 2;
  const auto frame = assembler.push(changed);
  ASSERT_TRUE(completes(frame));
  EXPECT_EQ(frame.value()->bytes, bytes({1, 2, 3, 4}));
  EXPECT_EQ(frame.value()->adaptive_stream_index, 2U);
}

TEST_F(VideoAssemblerRecovery, RecoversASourceUnitFromParity) {
  EXPECT_TRUE(pending(assembler.push(unit(9, 0, 3, 1, bytes({0, 0, 1, 2})))));
  const auto parity = assembler.push(unit(9, 2, 3, 1, bytes({0, 0, 0x8e, 0})));
  ASSERT_TRUE(completes(parity));
  EXPECT_EQ(parity.value()->frame_index, 9U);
  EXPECT_EQ(parity.value()->codec, 0x65U);
  EXPECT_EQ(parity.value()->bytes, bytes({1, 2, 3, 4}));
  remote_play::VideoAssembler no_loss;
  EXPECT_TRUE(no_loss.push(unit(9, 0, 3, 1, bytes({0, 0, 1, 2}))));
  const auto complete = no_loss.push(unit(9, 1, 3, 1, bytes({0, 0, 3, 4})));
  ASSERT_TRUE(completes(complete));
  EXPECT_EQ(parity.value()->bytes, complete.value()->bytes);
  EXPECT_FALSE(assembler.push(unit(9, 1, 3, 1, bytes({0, 0, 3, 4}))));
  const auto following = assembler.push(unit(10, 0, 1, 0, bytes({0, 0, 5})));
  ASSERT_TRUE(completes(following));
  EXPECT_EQ(following.value()->bytes, bytes({5}));
}

TEST_F(VideoAssemblerRecovery, RecoversAFrameFromParityAlone) {
  EXPECT_TRUE(
      pending(assembler.push(unit(11, 2, 4, 2, bytes({0, 0, 0x8f, 0xf6})))));
  const auto second =
      assembler.push(unit(11, 3, 4, 2, bytes({0, 0, 0x7b, 0xf7})));
  ASSERT_TRUE(completes(second));
  EXPECT_EQ(second.value()->bytes, bytes({1, 2, 3, 4}));
}

TEST_F(VideoAssemblerRecovery, StripsThePaddingOfARecoveredUnit) {
  EXPECT_TRUE(assembler.push(unit(12, 0, 3, 1, bytes({0, 1, 1}))));
  const auto recovered =
      assembler.push(unit(12, 2, 3, 1, bytes({0, 1, 0x8e, 2})));
  ASSERT_TRUE(completes(recovered));
  EXPECT_EQ(recovered.value()->bytes, bytes({1, 3, 4}));
}

TEST_F(VideoAssemblerRecovery, GivesUpAFrameWhosePaddingExceedsItsUnit) {
  EXPECT_TRUE(assembler.push(unit(20, 0, 3, 1, bytes({0, 3, 1}))));
  EXPECT_EQ(error_of(assembler.push(unit(20, 2, 3, 1, bytes({0, 0, 0x8e, 0})))),
            remote_play::VideoAssemblerError::invalid_unit);
  EXPECT_EQ(error_of(assembler.push(unit(20, 1, 3, 1, bytes({0, 0, 3, 4})))),
            remote_play::VideoAssemblerError::stale_frame);
  const auto next = assembler.push(unit(21, 0, 1, 0, bytes({0, 0, 9})));
  ASSERT_TRUE(completes(next));
  EXPECT_EQ(next.value()->bytes, bytes({9}));
}

TEST_F(VideoAssemblerRecovery, RefusesParityUnitsOfDifferentLengths) {
  EXPECT_TRUE(assembler.push(unit(22, 2, 4, 2, bytes({0, 0, 0x8f, 0xf6}))));
  EXPECT_EQ(error_of(assembler.push(unit(22, 3, 4, 2, bytes({0x7b})))),
            remote_play::VideoAssemblerError::invalid_unit);
}

TEST_F(VideoAssemblerRecovery, RefusesARecoveredUnitWithInvalidPadding) {
  EXPECT_TRUE(assembler.push(unit(23, 0, 3, 1, bytes({0, 0, 1, 2}))));
  EXPECT_EQ(error_of(assembler.push(unit(23, 2, 3, 1, bytes({0, 2, 0x8e, 0})))),
            remote_play::VideoAssemblerError::invalid_unit);
}

}  // namespace
}  // namespace remote_play_test
