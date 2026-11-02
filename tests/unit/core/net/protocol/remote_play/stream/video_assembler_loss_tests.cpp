#include "tests/unit/core/net/protocol/remote_play/stream/video_assembler_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

class VideoAssemblerLoss : public VideoAssemblerRig {
 protected:
  auto accept(takion::AvPacketValues value) -> testing::AssertionResult {
    const auto pushed = assembler.push(std::move(value));
    if (!pushed)
      return testing::AssertionFailure() << "the unit was refused with error "
                                         << static_cast<int>(pushed.error());
    if (!pushed.value())
      return testing::AssertionFailure() << "the unit completed no frame";
    assembler.mark_complete(pushed.value()->frame_index);
    return testing::AssertionSuccess();
  }

  auto held(takion::AvPacketValues value) -> testing::AssertionResult {
    const auto pushed = assembler.push(std::move(value));
    if (!pushed)
      return testing::AssertionFailure() << "the unit was refused with error "
                                         << static_cast<int>(pushed.error());
    if (pushed.value()) {
      assembler.mark_complete(pushed.value()->frame_index);
      return testing::AssertionFailure() << "the unit completed a frame";
    }
    return testing::AssertionSuccess();
  }

  auto only_loss_is(std::uint16_t first, std::uint16_t last,
                    bool fec_failure = false) -> testing::AssertionResult {
    auto result = loss_is(assembler.take_loss(), first, last, fec_failure);
    if (!result) return result;
    if (assembler.take_loss())
      return testing::AssertionFailure() << "a second loss was reported";
    return testing::AssertionSuccess();
  }

  void play_through_frame_seven() {
    ASSERT_TRUE(accept(unit(1, 0, 1, 0, bytes({0, 0, 1}))));
    EXPECT_FALSE(assembler.take_loss());
    ASSERT_TRUE(held(unit(2, 0, 3, 1, bytes({0, 0, 1, 2}))));
    EXPECT_FALSE(assembler.take_loss());
    ASSERT_TRUE(accept(unit(4, 0, 1, 0, bytes({0, 0, 3}))));
    EXPECT_TRUE(loss_is(assembler.take_loss(), 2, 2, true));
    EXPECT_TRUE(only_loss_is(2, 3));
    EXPECT_FALSE(assembler.push(unit(2, 1, 3, 1, bytes({0, 0, 3, 4}))));
    EXPECT_FALSE(assembler.take_loss());
    ASSERT_TRUE(held(unit(5, 2, 3, 1, bytes({0, 0, 0x8e, 0}))));
    EXPECT_TRUE(only_loss_is(5, 5, true));
    EXPECT_FALSE(assembler.push(unit(5, 0, 3, 1, bytes({0, 0, 1, 2}))));
    ASSERT_TRUE(held(unit(6, 0, 3, 1, bytes({0, 0, 1, 2}))));
    EXPECT_TRUE(only_loss_is(5, 5));
    ASSERT_TRUE(accept(unit(6, 2, 3, 1, bytes({0, 0, 0x8e, 0}))));
    EXPECT_FALSE(assembler.take_loss());
    ASSERT_TRUE(accept(unit(7, 0, 1, 0, bytes({0, 0, 1}))));
    EXPECT_FALSE(assembler.take_loss());
  }
};

TEST_F(VideoAssemblerLoss, ReportsAbandonedFramesGapsAndFecFailuresInOrder) {
  ASSERT_NO_FATAL_FAILURE(play_through_frame_seven());
}

TEST_F(VideoAssemblerLoss, ReportsAGapAcrossTheWrapAfterAReset) {
  ASSERT_NO_FATAL_FAILURE(play_through_frame_seven());
  assembler.reset();
  EXPECT_FALSE(assembler.take_loss());
  EXPECT_TRUE(accept(unit(65534, 0, 1, 0, bytes({0, 0, 1}))));
  EXPECT_FALSE(assembler.take_loss());
  EXPECT_TRUE(accept(unit(1, 0, 1, 0, bytes({0, 0, 1}))));
  EXPECT_TRUE(only_loss_is(65535, 0));
  assembler.reset();
  EXPECT_TRUE(accept(unit(3, 0, 1, 0, bytes({0, 0, 1}))));
  EXPECT_TRUE(only_loss_is(1, 2));
}

TEST_F(VideoAssemblerLoss, ReportsAGapAndAFecFailureFromTheSameStart) {
  EXPECT_TRUE(completes(assembler.push(unit(1, 0, 1, 0, bytes({0, 0, 1})))));
  assembler.mark_complete(1);
  EXPECT_TRUE(
      pending(assembler.push(unit(4, 2, 3, 1, bytes({0, 0, 0x8e, 0})))));
  EXPECT_TRUE(loss_is(assembler.take_loss(), 2, 3, false));
  EXPECT_TRUE(loss_is(assembler.take_loss(), 2, 4, true));
  EXPECT_FALSE(assembler.take_loss());
}

TEST_F(VideoAssemblerLoss, SurfacesAFecFailureWhoseSpanCannotBeReported) {
  EXPECT_TRUE(
      pending(assembler.push(unit(0x9000, 2, 3, 1, bytes({0, 0, 0x8e, 0})))));
  EXPECT_TRUE(loss_is(assembler.take_loss(), 1, 0x9000, true));
  EXPECT_FALSE(assembler.take_loss());
  EXPECT_TRUE(
      pending(assembler.push(unit(0x9001, 2, 3, 1, bytes({0, 0, 0x8e, 0})))));
  EXPECT_TRUE(loss_is(assembler.take_loss(), 1, 0x9001, true));
  EXPECT_FALSE(assembler.take_loss());
}

TEST_F(VideoAssemblerLoss, FailsFecBeyond256UnitsOnceEnoughUnitsArrive) {
  EXPECT_TRUE(completes(assembler.push(unit(1, 0, 1, 0, bytes({0, 0, 1})))));
  assembler.mark_complete(1);
  constexpr std::uint16_t total = 300;
  constexpr std::uint16_t fec = 10;
  for (std::uint16_t index = 1; index < total - fec; ++index)
    EXPECT_TRUE(
        pending(assembler.push(unit(2, index, total, fec, bytes({0, 0, 1})))))
        << "unit " << index;
  EXPECT_FALSE(assembler.take_loss());
  EXPECT_TRUE(pending(
      assembler.push(unit(2, total - fec, total, fec, bytes({0, 0, 1})))));
  EXPECT_TRUE(loss_is(assembler.take_loss(), 2, 2, true));
  EXPECT_EQ(error_of(assembler.push(
                unit(2, total - 1, total, fec, bytes({0, 0, 1})))),
            remote_play::VideoAssemblerError::stale_frame);
  EXPECT_FALSE(assembler.take_loss());
}

TEST_F(VideoAssemblerLoss, ReReportsUntilAFrameIsAcceptedForDecode) {
  EXPECT_TRUE(completes(assembler.push(unit(1, 0, 1, 0, bytes({0, 0, 1})))));
  assembler.mark_complete(1);
  EXPECT_TRUE(completes(assembler.push(unit(2, 0, 1, 0, bytes({0, 0, 2})))));
  EXPECT_FALSE(assembler.take_loss());
  EXPECT_EQ(assembler.last_complete(), 1U);
  EXPECT_TRUE(assembler.push(unit(3, 0, 1, 0, bytes({0, 0, 3}))));
  EXPECT_TRUE(loss_is(assembler.take_loss(), 2, 2, false));
  EXPECT_TRUE(assembler.push(unit(4, 0, 1, 0, bytes({0, 0, 4}))));
  const auto lost = assembler.take_loss();
  ASSERT_TRUE(lost);
  EXPECT_EQ(lost->first, 2U);
  EXPECT_EQ(lost->last, 3U);
  assembler.mark_complete(4);
  EXPECT_TRUE(assembler.push(unit(5, 0, 1, 0, bytes({0, 0, 5}))));
  EXPECT_FALSE(assembler.take_loss());
  assembler.mark_complete(5);
  assembler.rewind_complete(7);
  EXPECT_EQ(assembler.last_complete(), 5U);
  assembler.rewind_complete(4);
  EXPECT_EQ(assembler.last_complete(), 3U);
  EXPECT_TRUE(assembler.push(unit(6, 0, 1, 0, bytes({0, 0, 6}))));
  const auto rewound = assembler.take_loss();
  ASSERT_TRUE(rewound);
  EXPECT_EQ(rewound->first, 4U);
  EXPECT_EQ(rewound->last, 5U);
}

}  // namespace
}  // namespace remote_play_test
