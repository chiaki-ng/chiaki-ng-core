#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/video_intake_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>

namespace play_test {

namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using api_test::bytes_of;
using api_test::stream_info;
using api_test::video_media;
using libchiaking::media::MediaFailure;
using libchiaking::media::VideoLimits;
using libchiaking::media::VideoVerdict;

class VideoIntakeProfiles : public VideoIntakeTest {};

TEST_F(VideoIntakeProfiles, HaveSensibleDefaultLimits) {
  const VideoLimits limits;
  EXPECT_EQ(limits.profiles, 8U);
  EXPECT_EQ(limits.header_bytes, 65536U);
  EXPECT_EQ(limits.frame_bytes, 4U * 1024U * 1024U);
  EXPECT_FALSE(limits.dimension);
  EXPECT_EQ(limits.idr_rejections, 2U);
  EXPECT_EQ(limits.idr_interval, std::chrono::milliseconds{1000});
}

TEST_F(VideoIntakeProfiles, RefuseFramesBeforeAnyAreConfigured) {
  Intake intake{clock()};
  const auto early = intake.admit(frame(0));
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(), MediaFailure::frame_validation);
}

TEST_F(VideoIntakeProfiles, AreReplacedByEachConfiguration) {
  Intake intake{clock()};
  auto info = stream_info(2);
  info.profiles[0].width = 1920;
  EXPECT_TRUE(intake.configure(info));
  EXPECT_EQ(intake.profile_count(), 2U);
  info = stream_info(1);
  info.profiles[0].video_header = bytes_of({9, 9});
  EXPECT_TRUE(intake.configure(info));
  EXPECT_EQ(intake.profile_count(), 1U);
}

TEST_F(VideoIntakeProfiles, AreBoundedInCountHeaderAndDimension) {
  Intake intake{clock()};
  const auto too_many = intake.configure(stream_info(9));
  ASSERT_FALSE(too_many);
  EXPECT_EQ(too_many.error(), MediaFailure::profile_validation);
  auto broken = stream_info(1);
  broken.profiles[0].video_header.clear();
  EXPECT_FALSE(intake.configure(broken));
  broken.profiles[0].video_header.assign(65537, std::byte{1});
  EXPECT_FALSE(intake.configure(broken));
  broken = stream_info(1);
  broken.profiles[0].width = 0;
  EXPECT_FALSE(intake.configure(broken));
  EXPECT_FALSE(intake.configure(remote_play::event::StreamInfoReady{}));
  broken.profiles[0].width = 5000;
  EXPECT_TRUE(intake.configure(broken));

  const auto kept = intake.admit(frame(1));
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept.value().width(), 5000U);
  EXPECT_EQ(kept.value().profile_header().size(), 3U);
}

TEST_F(VideoIntakeProfiles, FollowStricterLimits) {
  auto wide = stream_info(1);
  wide.profiles[0].width = 5000;
  VideoLimits capped;
  capped.dimension = 4096;
  capped.frame_bytes = 2;
  Intake strict{clock(), capped};
  EXPECT_FALSE(strict.configure(wide));
  EXPECT_TRUE(strict.configure(stream_info(1)));
  const auto oversized = strict.admit(frame(1));
  ASSERT_FALSE(oversized);
  EXPECT_EQ(oversized.error(), MediaFailure::frame_validation);
}

TEST_F(VideoIntakeProfiles, RefuseAFrameForAnUnknownProfileOrWithNoBytes) {
  Intake intake{clock()};
  EXPECT_TRUE(intake.configure(stream_info(2)));
  const auto out_of_range = intake.admit(frame(1, 2));
  ASSERT_FALSE(out_of_range);
  EXPECT_EQ(out_of_range.error(), MediaFailure::frame_validation);
  EXPECT_FALSE(
      intake.admit(frame(Media{remote_play::EncodedMediaKind::video, {}})));
}

TEST_F(VideoIntakeProfiles, HandOverTheirHeaderOncePerSwitch) {
  Intake intake{clock()};
  EXPECT_TRUE(intake.configure(stream_info(2)));
  const auto first = intake.admit(frame(1));
  ASSERT_TRUE(first);
  EXPECT_TRUE(first.value().profile_changed());
  EXPECT_EQ(first.value().profile_header().size(), 3U);
  const auto failed =
      intake.settle(first.value(), {VideoVerdict::format_failed, -777});
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), MediaFailure::decoder_submission);
  EXPECT_FALSE(intake.settle(first.value(), {VideoVerdict::submit_failed, 1}));

  const auto again = intake.admit(frame(2));
  ASSERT_TRUE(again);
  EXPECT_TRUE(again.value().profile_changed());
  EXPECT_TRUE(intake.settle(again.value(), {VideoVerdict::accepted, 0}));
  const auto steady = intake.admit(frame(3));
  ASSERT_TRUE(steady);
  EXPECT_FALSE(steady.value().profile_changed());
  EXPECT_TRUE(steady.value().profile_header().empty());
  const auto switched = intake.admit(frame(4, 1));
  ASSERT_TRUE(switched);
  EXPECT_TRUE(switched.value().profile_changed());
  EXPECT_EQ(switched.value().profile_index(), 1U);
}

TEST_F(VideoIntakeProfiles, ReadTheCodecFromTheHeaderAndFallBackToTheFrame) {
  Intake intake{clock()};
  auto info = stream_info(3);
  info.profiles[0].video_header =
      bytes_of({0, 0, 0, 1, 0x67, 0x64, 0, 0, 0, 1, 0x68});
  info.profiles[1].video_header =
      bytes_of({0, 0, 1, 0x40, 0x01, 0x0c, 0, 0, 1, 0x42, 0x01});
  info.profiles[2].video_header = bytes_of({1, 2, 3});
  EXPECT_TRUE(intake.configure(info));

  auto hinted = video_media(1, 0);
  hinted.codec = 1;
  const auto avc = intake.admit(frame(std::move(hinted)));
  ASSERT_TRUE(avc);
  EXPECT_FALSE(avc.value().hevc());
  const auto hevc = intake.admit(frame(2, 1));
  ASSERT_TRUE(hevc);
  EXPECT_TRUE(hevc.value().hevc());
  const auto unknown = intake.admit(frame(3, 2));
  ASSERT_TRUE(unknown);
  EXPECT_FALSE(unknown.value().hevc());
  auto opaque = video_media(4, 2);
  opaque.codec = 1;
  const auto hinted_hevc = intake.admit(frame(std::move(opaque)));
  ASSERT_TRUE(hinted_hevc);
  EXPECT_TRUE(hinted_hevc.value().hevc());
}

}  // namespace
}  // namespace play_test
