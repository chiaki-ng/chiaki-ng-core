#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/start/link_request_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>

namespace play_test {

using test_support::play::SignedInPlayer;
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::consoles::Generation;
using remote_play::ControllerPreference;

class LinkRequestPreferences : public LinkRequestRig {
 protected:
  void SetUp() override {
    player = SignedInPlayer::fresh_for_remote_play("SYNTHETIC_BEARER_TOKEN");
    console = consoles.paired(*player, ps5_console, Generation::ps5,
                              "192.168.1.10", device(std::byte{1}));
    ASSERT_TRUE(console);
  }

  static auto streamed() -> play::CloudLinkRequest {
    auto launch = cloud_launch();
    launch.time_zone = "UTC+00:00";
    return launch;
  }

  std::shared_ptr<libchiaking::users::PsUser> player;
  std::shared_ptr<libchiaking::consoles::Console> console;
};

TEST_F(LinkRequestPreferences, RejectAnUnknownControllerEverywhere) {
  for (const int raw : {3, 4, 255}) {
    SCOPED_TRACE(raw);
    const auto unknown = static_cast<ControllerPreference>(raw);
    play::ConsolePreferences preferences;
    preferences.controller = unknown;
    const auto local_built = play::HomeLinkConfiguration::create(
        *console, *player, VideoRequest{}, preferences);
    ASSERT_FALSE(local_built);
    EXPECT_EQ(local_built.error(), ConfigurationError::rejected);
    const auto remote_built =
        internet(*console, player, VideoRequest{}, preferences);
    ASSERT_FALSE(remote_built);
    EXPECT_EQ(remote_built.error(), ConfigurationError::rejected);
    auto launch = streamed();
    launch.controller = unknown;
    const auto cloud_built =
        play::CloudLinkConfiguration::create(std::move(launch));
    ASSERT_FALSE(cloud_built);
    EXPECT_EQ(cloud_built.error(), ConfigurationError::rejected);
  }
}

TEST_F(LinkRequestPreferences,
       UseADualSenseOnTheCloudUnlessADualShock4IsAsked) {
  for (const auto known :
       {ControllerPreference::console_default, ControllerPreference::dualshock4,
        ControllerPreference::dualsense}) {
    SCOPED_TRACE(testing::PrintToString(known));
    auto launch = streamed();
    launch.controller = known;
    const auto built = play::CloudLinkConfiguration::create(std::move(launch));
    ASSERT_TRUE(built);
    EXPECT_EQ(built.value().enable_dualsense_ctrl,
              known != ControllerPreference::dualshock4);
  }
}

TEST_F(LinkRequestPreferences, RejectAnUnknownChannelCountEverywhere) {
  for (const std::uint64_t channels :
       {std::uint64_t{3}, std::uint64_t{256}, std::uint64_t{262},
        std::uint64_t{0x10000000c}}) {
    SCOPED_TRACE(channels);
    play::ConsolePreferences preferences;
    preferences.tuning.requested_audio_channels = channels;
    const auto local_built = play::HomeLinkConfiguration::create(
        *console, *player, VideoRequest{}, preferences);
    ASSERT_FALSE(local_built);
    EXPECT_EQ(local_built.error(), ConfigurationError::rejected);
    EXPECT_FALSE(internet(*console, player, VideoRequest{}, preferences));
    auto launch = streamed();
    launch.tuning.requested_audio_channels = channels;
    EXPECT_FALSE(play::CloudLinkConfiguration::create(std::move(launch)));
  }
}

TEST_F(LinkRequestPreferences, KeepASurroundChannelCount) {
  play::ConsolePreferences surround;
  surround.tuning.requested_audio_channels = 6;
  const auto built = play::HomeLinkConfiguration::create(
      *console, *player, VideoRequest{}, surround);
  ASSERT_TRUE(built);
  EXPECT_EQ(built.value().tuning.requested_audio_channels, 6u);
}

}  // namespace
}  // namespace play_test
