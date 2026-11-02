#include "src/core/net/protocol/remote_play/stream/audio_layout.hpp"
#include "src/core/net/protocol/remote_play/video_presets.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/play/adapters/session/command_route.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;
using libchiaking::play::CommandRoute;
using remote_play::AudioRequestTarget;
using remote_play::StreamError;
using takion::BuildTarget;

static_assert(libchiaking::video_preset_360p.width == 640 &&
              libchiaking::video_preset_360p.height == 360 &&
              libchiaking::video_preset_360p.bitrate_kbps == 2000);
static_assert(libchiaking::video_preset_540p.width == 960 &&
              libchiaking::video_preset_540p.height == 540 &&
              libchiaking::video_preset_540p.bitrate_kbps == 6000);
static_assert(libchiaking::video_preset_720p.width == 1280 &&
              libchiaking::video_preset_720p.height == 720 &&
              libchiaking::video_preset_720p.bitrate_kbps == 10000);
static_assert(libchiaking::video_preset_1080p.width == 1920 &&
              libchiaking::video_preset_1080p.height == 1080 &&
              libchiaking::video_preset_1080p.bitrate_kbps == 15000);
static_assert(libchiaking::video_fps_30 == 30 &&
              libchiaking::video_fps_60 == 60);
static_assert(libchiaking::default_video_fps == 60 &&
              libchiaking::default_video_preset.height == 720);
static_assert(static_cast<int>(remote_play::StreamState::idle) == 0);
static_assert(
    static_cast<int>(remote_play::EncodedMediaKind::video_fec_failure) == 11);
static_assert(static_cast<int>(remote_play::EncodedMediaKind::audio_profile) ==
              12);
static_assert(static_cast<int>(remote_play::FailureStage::cloud_launch_check) ==
              33);
static_assert(takion::takion_build_version(static_cast<BuildTarget>(4)) == 0);
static_assert(takion::takion_build_version(static_cast<BuildTarget>(255)) == 0);

TEST(LinkResultNames, NameAKnownResultAndNothingElse) {
  EXPECT_EQ(CommandRoute::result_name(StreamError::ctrl_timeout),
            "ctrl_timeout");
  EXPECT_TRUE(CommandRoute::result_name(static_cast<StreamError>(200)).empty());
}

TEST(LinkAudioRequestTargets, SupportNothingForAnUnknownTarget) {
  for (const int target : {4, 9, 255}) {
    for (std::uint64_t channels = 0; channels <= 16; ++channels) {
      SCOPED_TRACE(testing::Message()
                   << "target " << target << " channels " << channels);
      EXPECT_FALSE(remote_play::requested_audio_channels_supported(
          static_cast<AudioRequestTarget>(target), channels));
    }
  }
}

TEST(LinkAudioRequestTargets, SupportStereoInTheCloudAndWideAudioOnAPs5) {
  EXPECT_TRUE(remote_play::requested_audio_channels_supported(
      AudioRequestTarget::cloud_stream, 2));
  EXPECT_TRUE(remote_play::requested_audio_channels_supported(
      AudioRequestTarget::ps5, 12));
}

TEST(LinkTakionBuildVersions, FollowTheTargetAndAnyPin) {
  const auto pinned = takion::takion_build_version_override;
  EXPECT_EQ(takion::takion_build_version(BuildTarget::cloud_stream),
            pinned ? pinned : takion::takion_version_cloud_stream);
  const bool cloud_only = pinned == 21 || pinned == 23 || pinned == 25;
  const bool console_pin = pinned != 0 && !cloud_only;
  EXPECT_EQ(takion::takion_build_version(BuildTarget::ps4),
            console_pin ? pinned : takion::takion_version_ps4);
  EXPECT_EQ(takion::takion_build_version(BuildTarget::ps5),
            console_pin ? pinned : takion::takion_version_ps5);
  EXPECT_EQ(takion::takion_build_version(BuildTarget::legacy_cloud_stream),
            console_pin ? pinned : takion::takion_version_legacy_cloud_stream);
}

TEST(LinkTakionBuildVersions, AgreeWithTheVersionSelectedForASession) {
  EXPECT_EQ(takion::takion_build_version(BuildTarget::ps4),
            takion::select_takion_version(takion::takion_version_ps4, 0,
                                          takion::SessionScope::console));
  EXPECT_EQ(takion::takion_build_version(BuildTarget::ps5),
            takion::select_takion_version(takion::takion_version_ps5, 0,
                                          takion::SessionScope::console));
  EXPECT_EQ(takion::takion_build_version(BuildTarget::legacy_cloud_stream),
            takion::select_takion_version(
                takion::takion_version_legacy_cloud_stream, 0,
                takion::SessionScope::legacy_cloud_stream));
}

}  // namespace
}  // namespace play_test
