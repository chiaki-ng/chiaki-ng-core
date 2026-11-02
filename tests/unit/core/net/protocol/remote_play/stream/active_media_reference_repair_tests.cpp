#include "tests/unit/core/net/protocol/remote_play/stream/active_media_pipeline_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace remote_play_test {
namespace {

using Rig = ActiveMediaRig;

class ActiveMediaReferences : public ActiveMediaHarness<::testing::Test> {};

TEST_F(ActiveMediaReferences, DependencyRejectionsCountAsLostFrames) {
  auto media = pipeline(false, Rig::header(false));
  ASSERT_TRUE(media.consume(Rig::unit(1, Rig::slice(false, true)), 0));
  ASSERT_TRUE(media.consume(Rig::unit(2, Rig::slice(false, false)), 0));
  EXPECT_TRUE(Rig::take_video(media));
  EXPECT_TRUE(Rig::take_video(media));
  ASSERT_TRUE(media.consume(Rig::unit(4, Rig::slice(false, false)), 0));
  ASSERT_TRUE(media.consume(Rig::unit(5, Rig::slice(false, false)), 0));
  EXPECT_FALSE(Rig::take_video(media));
  EXPECT_EQ(media.frames_lost(), 2U);
  ASSERT_TRUE(media.consume(Rig::unit(6, Rig::slice(false, true)), 0));
  const auto video = Rig::take_video(media);
  ASSERT_TRUE(video);
  EXPECT_EQ(video->frame_index, 6U);
  EXPECT_EQ(video->frames_lost, 2U);
}

TEST_F(ActiveMediaReferences, AnUnansweredKeyframeRequestIsSentAgain) {
  auto media = pipeline(false, Rig::header(false));
  ASSERT_TRUE(media.consume(Rig::unit(1, Rig::slice(false, true)), 0));
  EXPECT_TRUE(Rig::take_video(media));
  std::uint16_t frame = 3;
  for (; frame < 3 + 2 * 10; frame += 2)
    ASSERT_TRUE(media.consume(Rig::unit(frame, Rig::slice(false, false)),
                              1'000'000));
  ASSERT_TRUE(media.waiting_for_idr());
  const auto asked = sink.idr;
  ASSERT_TRUE(media.consume(Rig::unit(frame, Rig::slice(false, false)),
                            1'900'000));
  EXPECT_EQ(sink.idr, asked);
  ASSERT_TRUE(media.consume(Rig::unit(frame + 1, Rig::slice(false, false)),
                            2'000'000));
  EXPECT_EQ(sink.idr, asked + 1U);
  EXPECT_TRUE(media.waiting_for_idr());
  ASSERT_TRUE(media.consume(Rig::unit(frame + 2, Rig::slice(false, true)),
                            2'100'000));
  EXPECT_TRUE(Rig::take_video(media));
  EXPECT_FALSE(media.waiting_for_idr());
  ASSERT_TRUE(media.consume(Rig::unit(frame + 3, Rig::slice(false, false)),
                            9'000'000));
  EXPECT_EQ(sink.idr, asked + 1U);
}

TEST_F(ActiveMediaReferences, ARunOfRefusedFramesAsksForAKeyframe) {
  auto media = pipeline(false, Rig::header(false));
  ASSERT_TRUE(media.consume(Rig::unit(1, Rig::slice(false, true)), 0));
  EXPECT_TRUE(Rig::take_video(media));
  for (std::uint16_t frame = 3; frame < 3 + 9; frame += 2)
    ASSERT_TRUE(media.consume(Rig::unit(frame, Rig::slice(false, false)), 0));
  const auto before = sink.idr;
  for (std::uint16_t frame = 13; frame < 13 + 20; frame += 2)
    ASSERT_TRUE(media.consume(Rig::unit(frame, Rig::slice(false, false)), 0));
  EXPECT_EQ(sink.idr, before + 1U);
  EXPECT_TRUE(media.waiting_for_idr());
}

TEST_F(ActiveMediaReferences, ARepairedReferenceMarksTheFrameRecovered) {
  auto media = pipeline(true, Rig::header(true));
  ASSERT_TRUE(media.consume(Rig::unit(1, Rig::slice(true, true)), 0));
  ASSERT_TRUE(media.consume(Rig::unit(2, Rig::slice(true, false)), 0));
  EXPECT_TRUE(Rig::take_video(media));
  EXPECT_TRUE(Rig::take_video(media));
  ASSERT_TRUE(media.consume(Rig::unit(4, Rig::slice(true, false)), 0));
  const auto video = Rig::take_video(media);
  ASSERT_TRUE(video);
  EXPECT_EQ(video->frame_index, 4U);
  EXPECT_TRUE(video->frame_recovered);
  EXPECT_EQ(video->frames_lost, 0U);
}

TEST_F(ActiveMediaReferences, ARequestedDropDiscardsTheNextFrameAndRepairs) {
  auto media = pipeline(true, Rig::header(true));
  ASSERT_TRUE(media.consume(Rig::unit(1, Rig::slice(true, true)), 0));
  ASSERT_TRUE(media.consume(Rig::unit(2, Rig::slice(true, false)), 0));
  EXPECT_TRUE(Rig::take_video(media));
  EXPECT_TRUE(Rig::take_video(media));
  media.drop_next_video_frame();
  ASSERT_TRUE(media.consume(Rig::unit(3, Rig::slice(true, false)), 0));
  EXPECT_EQ(diagnostics.dropped_on_request, 1U);
  ASSERT_TRUE(media.consume(Rig::unit(4, Rig::slice(true, false)), 0));
  const auto video = Rig::take_video(media);
  ASSERT_TRUE(video);
  EXPECT_EQ(video->frame_index, 4U);
  EXPECT_TRUE(video->frame_recovered);
  EXPECT_EQ(diagnostics.repaired_frames, 1U);
  ASSERT_FALSE(sink.corrupt.empty());
  EXPECT_EQ(sink.corrupt.back().first, 3U);
  EXPECT_EQ(sink.corrupt.back().second, 3U);
}

}  // namespace
}  // namespace remote_play_test
