#include "tests/unit/core/net/protocol/remote_play/stream/active_media_pipeline_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace remote_play_test {
namespace {

using Rig = ActiveMediaRig;

class ActiveMediaStreams : public ActiveMediaHarness<::testing::Test> {};

TEST_F(ActiveMediaStreams, DropsVideoWithAnInvalidAdaptiveStreamIndex) {
  auto media = pipeline(false, {std::byte{1}});
  ASSERT_TRUE(media.consume(Rig::unit(1, {std::byte{9}}), 0));
  EXPECT_TRUE(Rig::take_video(media));
  ASSERT_TRUE(media.consume(Rig::unit(2, {std::byte{9}}, 1), 0));
  EXPECT_FALSE(Rig::take_video(media));
  EXPECT_EQ(media.rejected_units(), 1U);
  EXPECT_TRUE(sink.corrupt.empty());
  ASSERT_TRUE(media.consume(Rig::unit(2, {std::byte{9}}), 0));
  const auto video = Rig::take_video(media);
  ASSERT_TRUE(video);
  EXPECT_EQ(video->frame_index, 2U);
  EXPECT_TRUE(sink.corrupt.empty());
}

TEST_F(ActiveMediaStreams, VideoOverflowKeepsTheProfileHeader) {
  auto media = pipeline(false, {std::byte{1}});
  for (std::uint16_t frame = 1; frame <= 70; ++frame)
    ASSERT_TRUE(media.consume(Rig::unit(frame, {std::byte{9}}), 0))
        << "frame " << frame;
  const auto first = Rig::take_any_video(media);
  ASSERT_TRUE(first);
  EXPECT_TRUE(first->profile_header);
  EXPECT_EQ(first->bytes, std::vector<std::byte>{std::byte{1}});
  const auto next = Rig::take_any_video(media);
  ASSERT_TRUE(next);
  EXPECT_FALSE(next->profile_header);
  EXPECT_EQ(next->frame_index, 65U);
  EXPECT_EQ(sink.corrupt, (Rig::Ranges{{1, 64}}));
}

TEST_F(ActiveMediaStreams, IgnoresARepeatedStreamInfo) {
  auto media = pipeline(false, {std::byte{1}});
  remote_play::ValidatedStreamInfo repeated;
  repeated.resolutions.push_back(
      takion::StreamInfoResolution{1280, 720, {std::byte{2}}});
  repeated.resolutions.push_back(
      takion::StreamInfoResolution{1920, 1080, {std::byte{3}}});
  media.on_stream_info(repeated);
  ASSERT_TRUE(media.consume(Rig::unit(1, {std::byte{9}}), 0));
  const auto header = Rig::take_any_video(media);
  ASSERT_TRUE(header);
  EXPECT_EQ(header->bytes, std::vector<std::byte>{std::byte{1}});
  EXPECT_TRUE(Rig::take_any_video(media));
  ASSERT_TRUE(media.consume(Rig::unit(2, {std::byte{9}}, 1), 0));
  EXPECT_FALSE(Rig::take_any_video(media));
  EXPECT_EQ(media.rejected_units(), 1U);
}

class ActiveMediaProfileHeader
    : public ActiveMediaHarness<::testing::TestWithParam<bool>> {};

TEST_P(ActiveMediaProfileHeader, PrecedesTheFirstFrameAndEverySwitch) {
  const bool hevc = GetParam();
  auto media = two_profiles(hevc);
  ASSERT_TRUE(media.consume(Rig::unit(1, Rig::slice(hevc, true)), 0));
  const auto first = Rig::take_any_video(media);
  ASSERT_TRUE(first);
  EXPECT_TRUE(first->profile_header);
  EXPECT_EQ(first->bytes, Rig::header(hevc));
  EXPECT_EQ(first->adaptive_stream_index, 0U);
  EXPECT_EQ(first->frames_lost, 0U);
  EXPECT_FALSE(first->frame_recovered);
  EXPECT_GE(first->bytes.capacity(), first->bytes.size() + 64);
  auto frame = Rig::take_any_video(media);
  ASSERT_TRUE(frame);
  EXPECT_FALSE(frame->profile_header);
  EXPECT_EQ(frame->frame_index, 1U);
  EXPECT_GE(frame->bytes.capacity(), frame->bytes.size() + 64);
  ASSERT_TRUE(media.consume(Rig::unit(2, Rig::slice(hevc, false)), 0));
  frame = Rig::take_any_video(media);
  ASSERT_TRUE(frame);
  EXPECT_FALSE(frame->profile_header);
  EXPECT_EQ(frame->frame_index, 2U);
  ASSERT_TRUE(media.consume(Rig::unit(3, Rig::slice(hevc, true), 1), 0));
  auto switched = Rig::take_any_video(media);
  ASSERT_TRUE(switched);
  EXPECT_TRUE(switched->profile_header);
  EXPECT_EQ(switched->adaptive_stream_index, 1U);
  EXPECT_EQ(switched->bytes.size(), Rig::header(hevc).size() + 1);
  ASSERT_FALSE(switched->bytes.empty());
  EXPECT_EQ(switched->bytes.back(), std::byte{0x5a});
  frame = Rig::take_any_video(media);
  ASSERT_TRUE(frame);
  EXPECT_FALSE(frame->profile_header);
  EXPECT_EQ(frame->frame_index, 3U);
  ASSERT_TRUE(media.consume(Rig::unit(4, Rig::slice(hevc, false), 1), 0));
  frame = Rig::take_any_video(media);
  ASSERT_TRUE(frame);
  EXPECT_FALSE(frame->profile_header);
  EXPECT_EQ(frame->frame_index, 4U);
  ASSERT_TRUE(media.consume(Rig::unit(5, Rig::slice(hevc, true)), 0));
  switched = Rig::take_any_video(media);
  ASSERT_TRUE(switched);
  EXPECT_TRUE(switched->profile_header);
  EXPECT_EQ(switched->adaptive_stream_index, 0U);
  EXPECT_TRUE(sink.corrupt.empty());
}

TEST_P(ActiveMediaProfileHeader, IsPushedEvenWhenTheFrameNeverCompletes) {
  const bool hevc = GetParam();
  auto media = two_profiles(hevc);
  ASSERT_TRUE(media.consume(Rig::split_unit(1, Rig::slice(hevc, true), 0), 0));
  const auto first = Rig::take_any_video(media);
  ASSERT_TRUE(first);
  EXPECT_TRUE(first->profile_header);
  EXPECT_EQ(first->frame_index, 1U);
  EXPECT_FALSE(Rig::take_any_video(media));
}

INSTANTIATE_TEST_SUITE_P(Codecs, ActiveMediaProfileHeader, ::testing::Bool(),
                         ActiveMediaRig::codec_name);

}  // namespace
}  // namespace remote_play_test
