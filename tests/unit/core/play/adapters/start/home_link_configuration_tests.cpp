#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/start/link_request_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <initializer_list>
#include <string>

namespace play_test {

using test_support::play::SignedInPlayer;
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::consoles::Generation;
using libchiaking::consoles::Host;
using remote_play::RegistrationTarget;

class HomeLinkConfiguration : public LinkRequestRig {
 protected:
  void SetUp() override {
    player = home_user();
    console =
        consoles.paired(*player, ps5_console, Generation::ps5, "192.168.1.10");
    ASSERT_TRUE(console);
  }

  std::shared_ptr<libchiaking::users::PsUser> player;
  std::shared_ptr<libchiaking::consoles::Console> console;
};

TEST_F(HomeLinkConfiguration, CarriesAPs5ConsoleAndTheRequest) {
  const auto built =
      play::HomeLinkConfiguration::create(*console, *player, hd_video(), {});
  ASSERT_TRUE(built);
  const auto& ps5 = built.value();
  EXPECT_EQ(ps5.host, "192.168.1.10");
  EXPECT_EQ(ps5.console.target, RegistrationTarget::ps5_1);
  EXPECT_EQ(ps5.console.registration_key[0], std::byte{0x31});
  EXPECT_EQ(ps5.console.key[15], std::byte{0xa0});
  EXPECT_EQ(ps5.did, (std::array<std::byte, libchiaking::rp_did_size>{}));
  EXPECT_EQ(ps5.ctrl_endpoint.port, 9295);
  EXPECT_EQ(ps5.active_stream_port, 9296);
  EXPECT_EQ(ps5.video.width, 1920u);
  EXPECT_EQ(ps5.video.height, 1080u);
  EXPECT_EQ(ps5.video.max_fps, 60u);
  EXPECT_EQ(ps5.video.bitrate_kbps, 30000u);
  EXPECT_EQ(ps5.codec, LocalVideoCodec::h265_hdr);
  EXPECT_EQ(ps5.feedback, remote_play::FeedbackMode::switch_compatibility);
  EXPECT_TRUE(ps5.enable_idr_on_fec_failure);
  EXPECT_FALSE(ps5.enable_keyboard);
  EXPECT_TRUE(ps5.enable_dualsense_ctrl);
  EXPECT_EQ(ps5.tuning.packet_loss_max, 0.05);
}

TEST_F(HomeLinkConfiguration, AsksAPs4ForH264WithoutADualSense) {
  const auto old =
      consoles.paired(*player, ps4_console, Generation::ps4, "ps4.lan");
  const auto ps4 =
      play::HomeLinkConfiguration::create(*old, *player, hd_video(), {});
  ASSERT_TRUE(ps4);
  EXPECT_EQ(ps4.value().codec, LocalVideoCodec::h264);
  EXPECT_FALSE(ps4.value().enable_dualsense_ctrl);
  EXPECT_EQ(ps4.value().host, "ps4.lan");
  EXPECT_EQ(ps4.value().console.target, RegistrationTarget::ps4_10);
}

TEST_F(HomeLinkConfiguration, FollowsTheConsolePreferences) {
  play::ConsolePreferences keyboard;
  keyboard.keyboard = true;
  keyboard.feedback = remote_play::FeedbackMode::standard;
  keyboard.controller = remote_play::ControllerPreference::dualshock4;
  const auto tuned = play::HomeLinkConfiguration::create(*console, *player,
                                                         hd_video(), keyboard);
  ASSERT_TRUE(tuned);
  EXPECT_TRUE(tuned.value().enable_keyboard);
  EXPECT_FALSE(tuned.value().enable_dualsense_ctrl);
  EXPECT_EQ(tuned.value().feedback, remote_play::FeedbackMode::standard);
}

TEST_F(HomeLinkConfiguration, TakesAHostOfUpTo255Characters) {
  for (const auto& host : {std::string{}, std::string{"bad host"},
                           std::string{"a\r\nb"}, std::string(256, 'a')}) {
    SCOPED_TRACE(host);
    EXPECT_FALSE(Host::create(host, Generation::ps5));
  }
  EXPECT_TRUE(Host::create(std::string(255, 'a'), Generation::ps5));
  EXPECT_TRUE(console->moved_to(
      Host::create(std::string(255, 'a'), Generation::ps5).value()));
  EXPECT_EQ(
      play::HomeLinkConfiguration::create(*console, *player, hd_video(), {})
          .value()
          .host.size(),
      255u);
  EXPECT_FALSE(
      libchiaking::consoles::Firmware::from(RegistrationTarget::ps4_unknown));
}

TEST_F(HomeLinkConfiguration, RefusesAConsoleWithoutAnAddress) {
  const auto nowhere =
      consoles.paired(*player, "aabbccddee03", Generation::ps5);
  const auto unaddressed =
      play::HomeLinkConfiguration::create(*nowhere, *player, hd_video(), {});
  ASSERT_FALSE(unaddressed);
  EXPECT_EQ(unaddressed.error(), ConfigurationError::invalid_host);
}

TEST_F(HomeLinkConfiguration, RefusesAPlayerTheConsoleIsNotPairedWith) {
  const auto stranger = SignedInPlayer::fresh();
  const auto unpaired =
      play::HomeLinkConfiguration::create(*console, *stranger, hd_video(), {});
  ASSERT_FALSE(unpaired);
  EXPECT_EQ(unpaired.error(), ConfigurationError::not_paired);
}

TEST_F(HomeLinkConfiguration, RefusesInvalidTuning) {
  play::ConsolePreferences lossy;
  lossy.tuning.packet_loss_max = 2.0;
  const auto rejected =
      play::HomeLinkConfiguration::create(*console, *player, hd_video(), lossy);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), ConfigurationError::rejected);
}

TEST_F(HomeLinkConfiguration, RefusesAForgottenConsole) {
  const auto old =
      consoles.paired(*player, ps4_console, Generation::ps4, "ps4.lan");
  EXPECT_TRUE(consoles.known.forget(*old));
  const auto forgotten =
      play::HomeLinkConfiguration::create(*old, *player, hd_video(), {});
  ASSERT_FALSE(forgotten);
  EXPECT_EQ(forgotten.error(), ConfigurationError::invalid_console);
}

}  // namespace
}  // namespace play_test
