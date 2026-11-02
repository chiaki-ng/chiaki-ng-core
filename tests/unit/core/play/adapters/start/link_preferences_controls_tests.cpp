#include "src/core/play/adapters/start/link_preferences.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace play_test {
namespace {

using namespace std::chrono_literals;
using libchiaking::catalog::ServiceGeneration;
using libchiaking::consoles::Generation;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using play::LinkPreferences;
using remote_play::ControllerPreference;
using remote_play::FeedbackMode;

class LinkControls : public ::testing::Test {
 protected:
  static auto controlled() -> play::Preferences {
    play::Preferences preferences;
    preferences.controls =
        play::ControlsRequest{play::ControllerChoice::dualshock4,
                              play::FeedbackStyle::switch_compatibility, true};
    return preferences;
  }

  static auto tuned() -> play::Preferences {
    auto preferences = controlled();
    preferences.sound = play::SoundRequest::create(6).value();
    preferences.internet =
        play::InternetOptions::create(true, 500, 30s).value();
    preferences.developer =
        play::DeveloperTuning::create({.disable_audio = true,
                                       .disable_video = true,
                                       .protocol_version = 12,
                                       .negotiate_protocol_version = true,
                                       .packet_loss_max = 0.25,
                                       .keyframe_on_lost_frame = false})
            .value();
    return preferences;
  }
};

TEST_F(LinkControls, DefaultOnAConsole) {
  const play::Preferences preferences;
  const auto defaults =
      LinkPreferences::on_console(preferences, Generation::ps5).value();
  EXPECT_EQ(defaults.console.feedback, FeedbackMode::standard);
  EXPECT_EQ(defaults.console.controller, ControllerPreference::console_default);
  EXPECT_FALSE(defaults.console.keyboard);
  EXPECT_TRUE(defaults.console.idr_on_fec_failure);
  EXPECT_TRUE(defaults.console.tuning.valid());
  EXPECT_EQ(defaults.console.tuning.requested_audio_channels, 0u);
  EXPECT_EQ(defaults.console.tuning.takion_version_override, 0u);
  EXPECT_FALSE(defaults.console.tuning.negotiate_takion_version);
  EXPECT_EQ(defaults.console.tuning.packet_loss_max, 0.05);
  EXPECT_EQ(defaults.console.tuning.source_tap, nullptr);
  EXPECT_FALSE(defaults.port_guessing.forced());
  EXPECT_EQ(defaults.port_guessing.count(), 75u);
  EXPECT_EQ(defaults.start_timeout, 60s);
}

TEST_F(LinkControls, DefaultToADualSenseOnTheCloud) {
  const play::Preferences preferences;
  const auto cloud_defaults =
      LinkPreferences::on_cloud(preferences, ServiceGeneration::ps5_cloud)
          .value();
  EXPECT_EQ(cloud_defaults.controller, ControllerPreference::dualsense);
  EXPECT_EQ(cloud_defaults.feedback, FeedbackMode::standard);
  EXPECT_FALSE(cloud_defaults.idr_on_fec_failure);
}

TEST_F(LinkControls, AskTheCloudForSurroundOnlyOnThePs5Cloud) {
  auto preferences = controlled();
  for (const auto channels : play::SoundRequest::channel_counts) {
    SCOPED_TRACE(+channels);
    preferences.sound = play::SoundRequest::create(channels).value();
    const auto ps5 =
        LinkPreferences::on_cloud(preferences, ServiceGeneration::ps5_cloud);
    ASSERT_TRUE(ps5);
    EXPECT_EQ(ps5->tuning.requested_audio_channels, channels);
    const auto old =
        LinkPreferences::on_cloud(preferences, ServiceGeneration::old_service);
    ASSERT_TRUE(old);
    EXPECT_EQ(old->tuning.requested_audio_channels,
              channels > 2 ? 0 : channels);
  }
}

TEST_F(LinkControls, CapTheCloudAudioAtTheSoundCeiling) {
  auto preferences = controlled();
  preferences.sound = play::SoundRequest::create(2).value();
  const auto uncapped =
      LinkPreferences::on_cloud(preferences, ServiceGeneration::ps5_cloud);
  ASSERT_TRUE(uncapped);
  EXPECT_EQ(uncapped->tuning.max_audio_channels, 0u);
  EXPECT_FALSE(play::SoundRequest::create(2)->capped_at(7));
  for (const auto channels : play::SoundRequest::channel_counts) {
    SCOPED_TRACE(+channels);
    preferences.sound =
        play::SoundRequest::create(2)->capped_at(channels).value();
    EXPECT_EQ(preferences.sound.ceiling(), channels);
    const auto capped =
        LinkPreferences::on_cloud(preferences, ServiceGeneration::ps5_cloud);
    ASSERT_TRUE(capped);
    EXPECT_EQ(capped->tuning.max_audio_channels, channels);
  }
}

TEST_F(LinkControls, ResolveEveryPreferenceForAPs5) {
  const auto ps5 =
      LinkPreferences::on_console(tuned(), Generation::ps5).value();
  EXPECT_EQ(ps5.console.feedback, FeedbackMode::switch_compatibility);
  EXPECT_TRUE(ps5.console.keyboard);
  EXPECT_EQ(ps5.console.controller, ControllerPreference::dualshock4);
  EXPECT_FALSE(ps5.console.idr_on_fec_failure);
  EXPECT_TRUE(ps5.console.tuning.disable_audio);
  EXPECT_TRUE(ps5.console.tuning.disable_video);
  EXPECT_EQ(ps5.console.tuning.takion_version_override, 12u);
  EXPECT_TRUE(ps5.console.tuning.negotiate_takion_version);
  EXPECT_EQ(ps5.console.tuning.packet_loss_max, 0.25);
  EXPECT_EQ(ps5.console.tuning.requested_audio_channels, 6u);
  EXPECT_TRUE(ps5.port_guessing.forced());
  EXPECT_EQ(ps5.port_guessing.count(), 80u);
  EXPECT_EQ(ps5.start_timeout, 30s);
}

TEST_F(LinkControls, DropSurroundForAPs4) {
  const auto ps4 =
      LinkPreferences::on_console(tuned(), Generation::ps4).value();
  EXPECT_EQ(ps4.console.tuning.requested_audio_channels, 0u);
}

TEST_F(LinkControls, ResolveEveryPreferenceForTheOldCloud) {
  const auto cloud =
      LinkPreferences::on_cloud(tuned(), ServiceGeneration::old_service)
          .value();
  EXPECT_EQ(cloud.controller, ControllerPreference::dualshock4);
  EXPECT_EQ(cloud.feedback, FeedbackMode::switch_compatibility);
  EXPECT_FALSE(cloud.idr_on_fec_failure);
  EXPECT_EQ(cloud.tuning.requested_audio_channels, 0u);
  EXPECT_EQ(cloud.tuning.takion_version_override, 12u);
}

TEST_F(LinkControls, AskTheCloudForAKeyframeOnALostFrameWhenTuned) {
  auto preferences = tuned();
  preferences.developer =
      play::DeveloperTuning::create({.keyframe_on_lost_frame = true}).value();
  EXPECT_TRUE(
      LinkPreferences::on_cloud(preferences, ServiceGeneration::ps5_cloud)
          .value()
          .idr_on_fec_failure);
}

}  // namespace
}  // namespace play_test
