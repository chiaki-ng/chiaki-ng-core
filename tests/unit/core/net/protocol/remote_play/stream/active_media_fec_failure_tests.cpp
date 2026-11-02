#include "tests/unit/core/net/protocol/remote_play/stream/active_media_pipeline_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace remote_play_test {
namespace {

using Rig = ActiveMediaRig;
using FecFailures = ActiveMediaRecordingSink::FecFailures;

class ActiveMediaFecLoss : public ActiveMediaHarness<::testing::Test> {};

TEST_F(ActiveMediaFecLoss, ReportsBothLossRangesAndCountsTheLostFrames) {
  auto media = pipeline(false, {std::byte{1}});
  ASSERT_TRUE(media.consume(Rig::unit(1, {std::byte{9}}), 0));
  EXPECT_TRUE(Rig::take_video(media));
  ASSERT_TRUE(media.consume(Rig::parity(4), 0));
  EXPECT_EQ(sink.corrupt, (Rig::Ranges{{2, 3}, {2, 4}}));
  EXPECT_EQ(sink.idr, 1U);
  EXPECT_EQ(media.frames_lost(), 3U);
  ASSERT_TRUE(media.consume(Rig::unit(5, {std::byte{9}}), 0));
  auto video = Rig::take_video(media);
  ASSERT_TRUE(video);
  EXPECT_EQ(video->frame_index, 5U);
  EXPECT_EQ(video->frames_lost, 3U);
  EXPECT_FALSE(video->frame_recovered);
  ASSERT_TRUE(media.consume(Rig::unit(6, {std::byte{9}}), 0));
  video = Rig::take_video(media);
  ASSERT_TRUE(video);
  EXPECT_EQ(video->frames_lost, 0U);
}

TEST_F(ActiveMediaFecLoss, IsNotReportedWhenIdrOnFecFailureIsOff) {
  auto media = pipeline(false, {std::byte{1}}, false);
  ASSERT_TRUE(media.consume(Rig::unit(1, {std::byte{9}}), 0));
  EXPECT_TRUE(Rig::take_video(media));
  ASSERT_TRUE(media.consume(Rig::parity(4), 0));
  EXPECT_TRUE(sink.fec_failures.empty());
  EXPECT_EQ(sink.idr, 0U);
}

TEST_F(ActiveMediaFecLoss, AnUnreportableFailureIsOnlyCountedWhenIdrIsOff) {
  auto media = pipeline(false, Rig::header(false), false);
  ASSERT_TRUE(media.consume(Rig::parity(0x9000), 0));
  EXPECT_TRUE(sink.corrupt.empty());
  EXPECT_EQ(media.frames_lost(), 1U);
  EXPECT_TRUE(sink.fec_failures.empty());
  EXPECT_EQ(sink.idr, 0U);
  EXPECT_FALSE(media.waiting_for_idr());
  ASSERT_TRUE(media.consume(Rig::parity(0x9001), 0));
  EXPECT_TRUE(sink.corrupt.empty());
  EXPECT_TRUE(sink.fec_failures.empty());
  EXPECT_EQ(sink.idr, 0U);
}

class ActiveMediaFecFailure
    : public ActiveMediaHarness<::testing::TestWithParam<bool>> {};

TEST_P(ActiveMediaFecFailure, ReportsTheFrameAndWhetherAnIdrWasSent) {
  const bool idr_accepted = GetParam();
  sink.idr_accepted = idr_accepted;
  auto media = pipeline(false, {std::byte{1}});
  ASSERT_TRUE(media.consume(Rig::unit(1, {std::byte{9}}), 0));
  EXPECT_TRUE(Rig::take_video(media));
  ASSERT_TRUE(media.consume(Rig::parity(4), 0));
  EXPECT_EQ(sink.fec_failures, (FecFailures{{4, idr_accepted}}));
  ASSERT_TRUE(media.consume(Rig::parity(6), 0));
  ASSERT_EQ(sink.fec_failures.size(), 2U);
  EXPECT_EQ(sink.fec_failures[1].first, 6U);
  EXPECT_EQ(sink.fec_failures[1].second, idr_accepted);
  EXPECT_EQ(sink.idr, idr_accepted ? 1U : 2U);
}

TEST_P(ActiveMediaFecFailure, AnUnreportableFrameRequestsAnIdrWithoutARange) {
  const bool idr_accepted = GetParam();
  sink.idr_accepted = idr_accepted;
  auto media = pipeline(false, Rig::header(false));
  ASSERT_TRUE(media.consume(Rig::parity(0x9000), 0));
  EXPECT_TRUE(sink.corrupt.empty());
  EXPECT_EQ(media.frames_lost(), 1U);
  EXPECT_EQ(sink.fec_failures, (FecFailures{{0x9000, idr_accepted}}));
  EXPECT_EQ(sink.idr, 1U);
  EXPECT_EQ(media.waiting_for_idr(), idr_accepted);
  ASSERT_TRUE(media.consume(Rig::parity(0x9001), 0));
  EXPECT_TRUE(sink.corrupt.empty());
  ASSERT_EQ(sink.fec_failures.size(), 2U);
  EXPECT_EQ(sink.fec_failures[1].first, 0x9001U);
  EXPECT_EQ(sink.fec_failures[1].second, idr_accepted);
  EXPECT_EQ(sink.idr, idr_accepted ? 1U : 2U);
  if (!idr_accepted) return;
  ASSERT_TRUE(media.consume(Rig::unit(0x9002, Rig::slice(false, false)), 0));
  EXPECT_FALSE(Rig::take_video(media));
  ASSERT_TRUE(media.consume(Rig::unit(0x9003, Rig::slice(false, true)), 0));
  const auto video = Rig::take_video(media);
  ASSERT_TRUE(video);
  EXPECT_EQ(video->frame_index, 0x9003U);
  EXPECT_EQ(video->frames_lost, 2U);
  EXPECT_FALSE(media.waiting_for_idr());
  EXPECT_TRUE(sink.corrupt.empty());
  EXPECT_EQ(sink.idr, 1U);
}

INSTANTIATE_TEST_SUITE_P(IdrAnswers, ActiveMediaFecFailure, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& tested)
                             -> std::string {
                           return tested.param ? "IdrAccepted" : "IdrRefused";
                         });

}  // namespace
}  // namespace remote_play_test
