#include "src/core/net/protocol/remote_play/stream/audio_layout.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/link_configuration_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <utility>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using link_configurations::Configurations;
using remote_play::AudioRequestTarget;
using remote_play::ControllerPreference;
using remote_play::RegistrationTarget;
using remote_play::SessionTuning;
using remote_play::StreamError;

static_assert(sizeof(SessionTuning::requested_audio_channels) ==
              sizeof(std::uint64_t));

TEST(LinkConfigurationTuning, AcceptsAPacketLossBetweenNoneAndAll) {
  SessionTuning tuning;
  EXPECT_TRUE(tuning.valid());
  for (const double loss : {0.0, 0.25, 1.0}) {
    SCOPED_TRACE(loss);
    tuning.packet_loss_max = loss;
    EXPECT_TRUE(tuning.valid());
  }
  for (const double loss :
       {-0.01, 1.5, std::numeric_limits<double>::quiet_NaN()}) {
    SCOPED_TRACE(loss);
    tuning.packet_loss_max = loss;
    EXPECT_FALSE(tuning.valid());
  }
}

TEST(LinkConfigurationTuning, AcceptsOnlyAudioLayoutsItKnows) {
  SessionTuning tuning;
  for (const int channels : {0, 2, 6, 8, 12}) {
    SCOPED_TRACE(channels);
    tuning.requested_audio_channels = static_cast<std::uint8_t>(channels);
    EXPECT_TRUE(tuning.valid());
  }
  for (const int channels : {1, 3, 7, 13}) {
    SCOPED_TRACE(channels);
    tuning.requested_audio_channels = static_cast<std::uint8_t>(channels);
    EXPECT_FALSE(tuning.valid());
  }
}

TEST(LinkConfigurationTuning, IsCheckedForEveryKind) {
  auto console = Configurations::local();
  console.tuning.packet_loss_max = 1.5;
  EXPECT_EQ(Configurations::refusal(std::move(console)),
            StreamError::invalid_configuration);
  auto psn = Configurations::remote();
  psn.tuning.requested_audio_channels = 3;
  EXPECT_EQ(Configurations::refusal(std::move(psn)),
            StreamError::invalid_configuration);
  auto streamed = Configurations::cloud();
  streamed.tuning.packet_loss_max = -0.01;
  EXPECT_EQ(Configurations::refusal(std::move(streamed)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationTuning, RefusesAWideAudioRequestThatAliasesAValidOne) {
  SessionTuning tuning;
  for (const std::uint64_t channels :
       {std::uint64_t{0}, std::uint64_t{2}, std::uint64_t{6}, std::uint64_t{8},
        std::uint64_t{12}}) {
    SCOPED_TRACE(channels);
    tuning.requested_audio_channels = channels;
    EXPECT_TRUE(tuning.valid());
  }
  for (const std::uint64_t channels :
       {std::uint64_t{1}, std::uint64_t{3}, std::uint64_t{4}, std::uint64_t{5},
        std::uint64_t{7}, std::uint64_t{9}, std::uint64_t{10},
        std::uint64_t{11}, std::uint64_t{13}, std::uint64_t{16},
        std::uint64_t{255}, std::uint64_t{256}, std::uint64_t{262},
        std::uint64_t{0x10000000c},
        std::numeric_limits<std::uint64_t>::max()}) {
    SCOPED_TRACE(channels);
    tuning.requested_audio_channels = channels;
    EXPECT_FALSE(tuning.valid());
    EXPECT_FALSE(remote_play::valid_requested_audio_channels(channels));
    for (const auto target : {AudioRequestTarget::ps4, AudioRequestTarget::ps5,
                              AudioRequestTarget::legacy_cloud_stream,
                              AudioRequestTarget::cloud_stream}) {
      SCOPED_TRACE(testing::PrintToString(target));
      EXPECT_FALSE(
          remote_play::requested_audio_channels_supported(target, channels));
    }
    auto console = Configurations::local();
    console.tuning.requested_audio_channels = channels;
    EXPECT_EQ(Configurations::refusal(std::move(console)),
              StreamError::invalid_configuration);
    auto psn = Configurations::remote();
    psn.tuning.requested_audio_channels = channels;
    EXPECT_EQ(Configurations::refusal(std::move(psn)),
              StreamError::invalid_configuration);
    auto streamed = Configurations::cloud();
    streamed.tuning.requested_audio_channels = channels;
    EXPECT_EQ(Configurations::refusal(std::move(streamed)),
              StreamError::invalid_configuration);
  }
}

TEST(LinkConfigurationTuning, AcceptsAKnownAudioRequestOnEveryKind) {
  for (const std::uint64_t channels :
       {std::uint64_t{0}, std::uint64_t{2}, std::uint64_t{6}, std::uint64_t{8},
        std::uint64_t{12}}) {
    SCOPED_TRACE(channels);
    auto console = Configurations::local();
    console.tuning.requested_audio_channels = channels;
    auto checked = std::move(console).validated();
    ASSERT_TRUE(checked);
    EXPECT_EQ(checked.value().tuning.requested_audio_channels, channels);
    auto legacy = Configurations::local();
    legacy.console.target = RegistrationTarget::ps4_10;
    legacy.tuning.requested_audio_channels = channels;
    EXPECT_TRUE(Configurations::accepts(std::move(legacy)));
    auto psn = Configurations::remote();
    psn.tuning.requested_audio_channels = channels;
    EXPECT_TRUE(Configurations::accepts(std::move(psn)));
    auto streamed = Configurations::cloud();
    streamed.tuning.requested_audio_channels = channels;
    EXPECT_TRUE(Configurations::accepts(std::move(streamed)));
  }
}

TEST(LinkConfigurationTuning, RefusesAnInfinitePacketLoss) {
  SessionTuning tuning;
  tuning.packet_loss_max = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(tuning.valid());
  tuning.packet_loss_max = -std::numeric_limits<double>::infinity();
  EXPECT_FALSE(tuning.valid());
  auto unbounded = Configurations::cloud();
  unbounded.tuning.packet_loss_max = std::numeric_limits<double>::infinity();
  EXPECT_EQ(Configurations::refusal(std::move(unbounded)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationTuning, KeepsItsSwitchesOnEveryKind) {
  const auto tuned = [](auto configuration, bool audio, bool video) {
    configuration.tuning.disable_audio = audio;
    configuration.tuning.disable_video = video;
    configuration.tuning.packet_loss_max = 0.25;
    configuration.tuning.takion_version_override = 13;
    auto checked = std::move(configuration).validated();
    ASSERT_TRUE(checked);
    EXPECT_EQ(checked.value().tuning.disable_audio, audio);
    EXPECT_EQ(checked.value().tuning.disable_video, video);
    EXPECT_EQ(checked.value().tuning.packet_loss_max, 0.25);
    EXPECT_EQ(checked.value().tuning.takion_version_override, 13U);
  };
  for (const bool audio : {false, true}) {
    for (const bool video : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << "audio " << audio << " video " << video);
      auto console = Configurations::local();
      console.video.auto_downgrade = false;
      EXPECT_TRUE(console.prefer_controller(ControllerPreference::dualshock4));
      tuned(std::move(console), audio, video);
      auto psn = Configurations::remote();
      psn.video.auto_downgrade = false;
      EXPECT_TRUE(psn.prefer_controller(ControllerPreference::dualshock4));
      tuned(std::move(psn), audio, video);
      tuned(Configurations::cloud(), audio, video);
    }
  }
}

TEST(LinkConfigurationTuning, KeepsAutoDowngradeOff) {
  auto downgraded = Configurations::local();
  downgraded.video.auto_downgrade = false;
  auto checked = std::move(downgraded).validated();
  ASSERT_TRUE(checked);
  EXPECT_FALSE(checked.value().video.auto_downgrade);
}

TEST(LinkConfigurationTuning, AcceptsVideoOffWithLoss) {
  auto lossy = Configurations::cloud();
  lossy.tuning.disable_video = true;
  lossy.tuning.packet_loss_max = 0.5;
  EXPECT_TRUE(Configurations::accepts(std::move(lossy)));
}

TEST(LinkConfigurationTuning, AcceptsADualSenseWithTotalLoss) {
  auto dualsense = Configurations::local();
  EXPECT_TRUE(dualsense.prefer_controller(ControllerPreference::dualsense));
  dualsense.tuning.packet_loss_max = 1.0;
  EXPECT_TRUE(Configurations::accepts(std::move(dualsense)));
}

TEST(LinkConfigurationTuning, AcceptsAnUnlimitedFrameRate) {
  auto unlimited = Configurations::local();
  unlimited.video.max_fps = 0;
  auto local = std::move(unlimited).validated();
  ASSERT_TRUE(local);
  EXPECT_EQ(local.value().video.max_fps, 0U);
  auto remote_unlimited = Configurations::remote();
  remote_unlimited.video.max_fps = 0;
  auto remote = std::move(remote_unlimited).validated();
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote.value().video.max_fps, 0U);
}

}  // namespace
}  // namespace play_test
