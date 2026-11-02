#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/start/link_request_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <string>

namespace play_test {

using test_support::play::SignedInPlayer;
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::consoles::Generation;
using remote_play::RegistrationTarget;

static_assert(play::InternetLinkConfiguration::default_start_timeout ==
              std::chrono::seconds{60});

class InternetLinkConfiguration : public LinkRequestRig {
 protected:
  void SetUp() override {
    player = SignedInPlayer::fresh_for_remote_play(
        "SYNTHETIC_BEARER_TOKEN", libchiaking::AccountId{0x0202020202020202});
    console = consoles.paired(*player, ps5_console, Generation::ps5, {},
                              device(std::byte{1}));
    ASSERT_TRUE(console);
  }

  std::shared_ptr<libchiaking::users::PsUser> player;
  std::shared_ptr<libchiaking::consoles::Console> console;
};

TEST_F(InternetLinkConfiguration, CarriesTheSignedInPlayerAndItsDevice) {
  const auto built = internet(*console, player, VideoRequest{});
  ASSERT_TRUE(built);
  const auto& remote = built.value();
  ASSERT_TRUE(remote.user);
  EXPECT_EQ(remote.user->account_id(),
            libchiaking::AccountId{0x0202020202020202});
  EXPECT_EQ(remote.device_uid[31], std::byte{1});
  EXPECT_EQ(remote.user->account_id().bytes()[7], std::byte{2});
  EXPECT_EQ(remote.did, (std::array<std::byte, libchiaking::rp_did_size>{}));
  EXPECT_TRUE(remote.enable_ipv6);
  EXPECT_EQ(remote.start_timeout_us, 60'000'000u);
  EXPECT_FALSE(remote.force_port_guessing);
  EXPECT_EQ(remote.port_guess_count, 75u);
  EXPECT_EQ(remote.feedback, remote_play::FeedbackMode::switch_compatibility);
  EXPECT_TRUE(remote.enable_idr_on_fec_failure);
  EXPECT_EQ(remote.video.width, 1280u);
  EXPECT_EQ(remote.video.height, 720u);
  EXPECT_TRUE(remote.enable_dualsense_ctrl);
  EXPECT_EQ(remote.console.target, RegistrationTarget::ps5_1);
}

TEST_F(InternetLinkConfiguration, ClampsForcedPortGuessingOnAPs4) {
  const auto old = consoles.paired(*player, ps4_console, Generation::ps4);
  const auto forced = internet(
      *old, player,
      VideoRequest::create(1080, 60, 30000, LocalVideoCodec::h265).value(), {},
      play::PortGuessing{true, 400});
  ASSERT_TRUE(forced);
  EXPECT_TRUE(forced.value().force_port_guessing);
  EXPECT_EQ(forced.value().port_guess_count, 80u);
  EXPECT_EQ(forced.value().codec, LocalVideoCodec::h264);
  EXPECT_EQ(forced.value().console.target, RegistrationTarget::ps4_10);
  EXPECT_EQ(forced.value().device_uid,
            libchiaking::consoles::DeviceUid::main_ps4().bytes());
}

TEST_F(InternetLinkConfiguration, NeedsASignedInPlayer) {
  const auto reachable = internet_target(*console, *player);
  const auto anonymous = play::InternetLinks::configured(
      reachable, std::shared_ptr<libchiaking::users::PsUser>{}, VideoRequest{},
      {}, {}, play::InternetLinkConfiguration::default_start_timeout);
  ASSERT_FALSE(anonymous);
  EXPECT_EQ(anonymous.error(), ConfigurationError::invalid_credential);
  const auto offline = play::InternetLinks::configured(
      reachable, home_user(), VideoRequest{}, {}, {},
      play::InternetLinkConfiguration::default_start_timeout);
  ASSERT_FALSE(offline);
  EXPECT_EQ(offline.error(), ConfigurationError::invalid_credential);
  EXPECT_FALSE(
      SignedInPlayer::admissible(SignedInPlayer::saved_for_remote_play("")));
  EXPECT_FALSE(SignedInPlayer::admissible(
      SignedInPlayer::saved_for_remote_play(std::string(8193, 'a'))));
}

TEST_F(InternetLinkConfiguration, NeedsAConsolePairedForTheInternet) {
  const auto unlinked =
      consoles.paired(*player, "aabbccddee03", Generation::ps5);
  const libchiaking::consoles::PairedPlay unpaired{*unlinked, *player};
  EXPECT_FALSE(unpaired.over_the_internet());
}

TEST_F(InternetLinkConfiguration, RefusesAZeroStartTimeout) {
  const auto hurried = internet(*console, player, VideoRequest{}, {}, {},
                                std::chrono::seconds{0});
  ASSERT_FALSE(hurried);
  EXPECT_EQ(hurried.error(), ConfigurationError::rejected);
}

}  // namespace
}  // namespace play_test
