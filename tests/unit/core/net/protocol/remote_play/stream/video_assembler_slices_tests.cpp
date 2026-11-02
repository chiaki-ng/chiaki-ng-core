#include "tests/unit/core/net/protocol/remote_play/stream/video_assembler_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

class VideoAssemblerSlices : public VideoAssemblerRig {};

TEST_F(VideoAssemblerSlices, AssembleSourcesWithTheParityCounts) {
  auto first = unit(3, 0, 0, 0xffff, bytes({0, 0, 1, 2}));
  first.video_word = 7;
  auto second = unit(3, 1, 0, 0xffff, bytes({0, 0, 3, 4}));
  second.video_word = 7;
  EXPECT_TRUE(pending(assembler.push(first)));
  EXPECT_TRUE(pending(assembler.push(second)));
  const auto frame = assembler.push(unit(3, 2, 3, 1, bytes({0, 0, 0x8e, 0})));
  ASSERT_TRUE(completes(frame));
  EXPECT_EQ(frame.value()->bytes, bytes({1, 2, 3, 4}));
  EXPECT_TRUE(frame.value()->counts_ignored);
  EXPECT_EQ(frame.value()->units_total, 3U);
  EXPECT_EQ(frame.value()->units_fec, 1U);
  EXPECT_EQ(frame.value()->units_received, 3U);
}

TEST_F(VideoAssemblerSlices, CompleteAFrameByCoverage) {
  auto first = unit(4, 0, 0, 0xffff, bytes({0, 0, 1}));
  first.video_word = 4;
  auto second = unit(4, 1, 0, 0xffff, bytes({0, 0, 2}));
  second.video_word = 6;
  EXPECT_TRUE(pending(assembler.push(first, 10)));
  const auto frame = assembler.push(second, 10);
  ASSERT_TRUE(completes(frame));
  EXPECT_EQ(frame.value()->bytes, bytes({1, 2}));
  EXPECT_EQ(frame.value()->units_total, 2U);
  EXPECT_EQ(frame.value()->units_fec, 0U);
  EXPECT_FALSE(assembler.push(first, 10));
}

TEST_F(VideoAssemblerSlices, JoinMultiUnitSlicesBeforeCountingCoverage) {
  auto first = unit(5, 0, 0, 0xffff, bytes({0, 0, 1}));
  first.video_word = 6;
  first.slice = takion::VideoSliceExtension{0, 2, true, false};
  auto second = unit(5, 1, 0, 0xffff, bytes({0, 0, 2}));
  second.video_word = 6;
  second.slice = takion::VideoSliceExtension{0, 2, false, true};
  auto third = unit(5, 2, 0, 0xffff, bytes({0, 0, 3}));
  third.video_word = 4;
  third.slice = takion::VideoSliceExtension{1, 1, true, true};
  EXPECT_TRUE(pending(assembler.push(second, 10)));
  EXPECT_TRUE(pending(assembler.push(third, 10)));
  const auto frame = assembler.push(first, 10);
  ASSERT_TRUE(completes(frame));
  EXPECT_EQ(frame.value()->bytes, bytes({1, 2, 3}));
}

TEST_F(VideoAssemblerSlices, RecoverASourceFromParity) {
  auto second = unit(6, 1, 0, 0xffff, bytes({0, 0, 3, 4}));
  second.video_word = 9;
  EXPECT_TRUE(pending(assembler.push(second)));
  const auto frame = assembler.push(unit(6, 2, 3, 1, bytes({0, 0, 0x8e, 0})));
  ASSERT_TRUE(completes(frame));
  EXPECT_EQ(frame.value()->bytes, bytes({1, 2, 3, 4}));
  EXPECT_EQ(frame.value()->restored_units, std::vector<std::uint16_t>{0});
  EXPECT_TRUE(frame.value()->counts_ignored);
}

TEST_F(VideoAssemblerSlices, RefuseParityWithoutUsableCounts) {
  EXPECT_EQ(error_of(assembler.push(unit(7, 2, 0, 0, bytes({0, 0, 1})))),
            remote_play::VideoAssemblerError::invalid_count);
  EXPECT_EQ(error_of(assembler.push(unit(7, 5, 3, 1, bytes({0, 0, 1})))),
            remote_play::VideoAssemblerError::invalid_unit);
  auto sliced = unit(7, 5, 3, 1, bytes({0, 0, 1}));
  sliced.video_word = 1;
  EXPECT_TRUE(pending(assembler.push(sliced)));
}

}  // namespace
}  // namespace remote_play_test
