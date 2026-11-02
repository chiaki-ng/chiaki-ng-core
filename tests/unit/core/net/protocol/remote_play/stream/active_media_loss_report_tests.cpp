#include "tests/unit/core/net/protocol/remote_play/stream/active_media_pipeline_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace remote_play_test {
namespace {

using Rig = ActiveMediaRig;

class ActiveMediaHostLoss
    : public ActiveMediaHarness<::testing::TestWithParam<bool>> {};

TEST_P(ActiveMediaHostLoss, AnUnreportableLossRequestsAnIdrWithoutARange) {
  auto media = pipeline(false, Rig::header(false), GetParam());
  ASSERT_TRUE(media.consume(Rig::unit(1, Rig::slice(false, true)), 0));
  ASSERT_TRUE(media.consume(Rig::unit(2, Rig::slice(false, false)), 0));
  EXPECT_TRUE(Rig::take_video(media));
  EXPECT_TRUE(Rig::take_video(media));
  EXPECT_TRUE(media.report_loss(3, 2));
  EXPECT_TRUE(sink.corrupt.empty());
  EXPECT_EQ(sink.idr, 1U);
  EXPECT_TRUE(media.waiting_for_idr());
  EXPECT_EQ(media.frames_lost(), 0U);
  EXPECT_TRUE(media.report_loss(0, 0x8000));
  EXPECT_TRUE(sink.corrupt.empty());
  EXPECT_EQ(sink.idr, 1U);
  ASSERT_TRUE(media.consume(Rig::unit(3, Rig::slice(false, false)), 0));
  EXPECT_FALSE(Rig::take_video(media));
  ASSERT_TRUE(media.consume(Rig::unit(4, Rig::slice(false, true)), 0));
  const auto video = Rig::take_video(media);
  ASSERT_TRUE(video);
  EXPECT_EQ(video->frame_index, 4U);
  EXPECT_FALSE(media.waiting_for_idr());
  EXPECT_EQ(sink.corrupt, (Rig::Ranges{{3, 3}}));
  EXPECT_TRUE(media.report_loss(4, 3));
  EXPECT_EQ(sink.corrupt, (Rig::Ranges{{3, 3}}));
  EXPECT_EQ(sink.idr, 2U);
  EXPECT_TRUE(media.waiting_for_idr());
  ASSERT_TRUE(media.consume(Rig::unit(5, Rig::slice(false, false)), 0));
  EXPECT_FALSE(Rig::take_video(media));
}

INSTANTIATE_TEST_SUITE_P(
    IdrOnFecFailure, ActiveMediaHostLoss, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& tested) -> std::string {
      return tested.param ? "On" : "Off";
    });

class ActiveMediaRefusedIdr : public ActiveMediaHarness<::testing::Test> {};

TEST_F(ActiveMediaRefusedIdr, DropsKnownReferencesAfterAnUnreportableLoss) {
  sink.idr_accepted = false;
  auto media = pipeline(false, Rig::header(false));
  ASSERT_TRUE(media.consume(Rig::unit(1, Rig::slice(false, true)), 0));
  EXPECT_TRUE(Rig::take_video(media));
  EXPECT_TRUE(media.report_loss(2, 1));
  EXPECT_TRUE(sink.corrupt.empty());
  EXPECT_EQ(sink.idr, 1U);
  EXPECT_FALSE(media.waiting_for_idr());
  ASSERT_TRUE(media.consume(Rig::unit(2, Rig::slice(false, false)), 0));
  EXPECT_FALSE(Rig::take_video(media));
  EXPECT_TRUE(media.report_loss(2, 1));
  EXPECT_EQ(sink.idr, 2U);
}

class ActiveMediaReportedLoss
    : public ActiveMediaHarness<::testing::TestWithParam<bool>> {};

TEST_P(ActiveMediaReportedLoss, OfAMiddleFrameExcludesOnlyThatFrame) {
  const bool hevc = GetParam();
  auto media = pipeline(hevc, Rig::header(hevc));
  ASSERT_TRUE(Rig::delivered(media, hevc, 3));
  EXPECT_TRUE(media.report_loss(2, 2));
  EXPECT_EQ(sink.corrupt, (Rig::Ranges{{2, 2}}));
  ASSERT_TRUE(media.consume(Rig::unit(4, Rig::slice(hevc, false)), 0));
  auto video = Rig::take_video(media);
  ASSERT_TRUE(video);
  EXPECT_EQ(video->frame_index, 4U);
  EXPECT_FALSE(video->frame_recovered);
  EXPECT_EQ(sink.corrupt, (Rig::Ranges{{2, 2}}));
  ASSERT_TRUE(media.consume(Rig::unit(5, Rig::slice(hevc, false, 2)), 0));
  video = Rig::take_video(media);
  if (hevc) {
    ASSERT_TRUE(video);
    EXPECT_EQ(video->frame_index, 5U);
    EXPECT_TRUE(video->frame_recovered);
  } else {
    EXPECT_FALSE(video);
    EXPECT_EQ(media.frames_lost(), 1U);
  }
  EXPECT_EQ(sink.corrupt, (Rig::Ranges{{2, 2}}));
  EXPECT_EQ(sink.idr, 0U);
}

TEST_P(ActiveMediaReportedLoss, OfTheNewestFrameIsReportedWithTheNextFrame) {
  const bool hevc = GetParam();
  auto media = pipeline(hevc, Rig::header(hevc));
  ASSERT_TRUE(Rig::delivered(media, hevc, 3));
  EXPECT_TRUE(media.report_loss(3, 3));
  EXPECT_TRUE(sink.corrupt.empty());
  ASSERT_TRUE(media.consume(Rig::unit(4, Rig::slice(hevc, false)), 0));
  EXPECT_EQ(sink.corrupt, (Rig::Ranges{{3, 3}}));
  auto video = Rig::take_video(media);
  if (hevc) {
    ASSERT_TRUE(video);
    EXPECT_EQ(video->frame_index, 4U);
    EXPECT_TRUE(video->frame_recovered);
  } else {
    EXPECT_FALSE(video);
    EXPECT_EQ(media.frames_lost(), 1U);
  }
  ASSERT_TRUE(
      media.consume(Rig::unit(5, Rig::slice(hevc, false, hevc ? 0 : 2)), 0));
  video = Rig::take_video(media);
  ASSERT_TRUE(video);
  EXPECT_EQ(video->frame_index, 5U);
  EXPECT_FALSE(video->frame_recovered);
  EXPECT_EQ(sink.corrupt,
            (hevc ? Rig::Ranges{{3, 3}} : Rig::Ranges{{3, 3}, {3, 4}}));
  EXPECT_EQ(sink.idr, 0U);
}

TEST_P(ActiveMediaReportedLoss, OutOfOrderReportsMatchPerFrameFailures) {
  const bool hevc = GetParam();
  auto media = pipeline(hevc, Rig::header(hevc));
  ASSERT_TRUE(Rig::delivered(media, hevc, 4));
  EXPECT_TRUE(media.report_loss(4, 4));
  EXPECT_TRUE(media.report_loss(2, 2));
  EXPECT_EQ(sink.corrupt, (Rig::Ranges{{2, 2}}));
  ASSERT_TRUE(media.consume(Rig::unit(5, Rig::slice(hevc, false, 1)), 0));
  EXPECT_EQ(sink.corrupt, (Rig::Ranges{{2, 2}, {4, 4}}));
  const auto video = Rig::take_video(media);
  ASSERT_TRUE(video);
  EXPECT_EQ(video->frame_index, 5U);
  EXPECT_FALSE(video->frame_recovered);
  EXPECT_TRUE(media.report_loss(3, 3));
  EXPECT_EQ(sink.corrupt, (Rig::Ranges{{2, 2}, {4, 4}, {2, 4}}));
}

TEST_P(ActiveMediaReportedLoss, OfTheNewestFrameWhileTheNextIsAssembling) {
  const bool hevc = GetParam();
  auto media = pipeline(hevc, Rig::header(hevc));
  ASSERT_TRUE(Rig::delivered(media, hevc, 2));
  const auto next = Rig::slice(hevc, false, 1);
  ASSERT_TRUE(media.consume(Rig::split_unit(3, next, 0), 0));
  EXPECT_TRUE(media.report_loss(2, 2));
  EXPECT_EQ(sink.corrupt, (Rig::Ranges{{2, 2}}));
  ASSERT_TRUE(media.consume(Rig::split_unit(3, next, 1), 0));
  const auto video = Rig::take_video(media);
  ASSERT_TRUE(video);
  EXPECT_EQ(video->frame_index, 3U);
  EXPECT_FALSE(video->frame_recovered);
  ASSERT_TRUE(media.consume(Rig::unit(4, Rig::slice(hevc, false)), 0));
  EXPECT_EQ(sink.corrupt, (Rig::Ranges{{2, 2}}));
}

INSTANTIATE_TEST_SUITE_P(Codecs, ActiveMediaReportedLoss, ::testing::Bool(),
                         ActiveMediaRig::codec_name);

}  // namespace
}  // namespace remote_play_test
