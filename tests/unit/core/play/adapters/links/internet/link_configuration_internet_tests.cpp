#include "src/core/play/adapters/links/internet/internet_link_configuration.hpp"
#include "src/core/psn/account/client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/link_configuration_rig.hpp"
#include "tests/support/play/signed_in_player.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace play_test {

using test_support::play::SignedInPlayer;
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::play::InternetLinkConfiguration;
using libchiaking::psn::account::Client;
using link_configurations::Configurations;
using remote_play::ControllerPreference;
using remote_play::RegistrationTarget;
using remote_play::StreamError;

static_assert(Client::token_limit == 8192);

TEST(LinkConfigurationOverInternet, AcceptsARemotePlayUser) {
  EXPECT_TRUE(Configurations::accepts(Configurations::remote()));
}

TEST(LinkConfigurationOverInternet, RefusesAMissingUser) {
  auto configuration = Configurations::remote();
  configuration.user.reset();
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationOverInternet, RefusesAHomeUser) {
  auto configuration = Configurations::remote();
  configuration.user = Configurations::home_user();
  ASSERT_NE(configuration.user, nullptr);
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationOverInternet, AdmitsABearerTokenUpToTheLimit) {
  EXPECT_TRUE(SignedInPlayer::admissible(SignedInPlayer::saved_for_remote_play(
      std::string(Client::token_limit, 't'))));
  EXPECT_FALSE(SignedInPlayer::admissible(SignedInPlayer::saved_for_remote_play(
      std::string(Client::token_limit + 1, 't'))));
  EXPECT_FALSE(
      SignedInPlayer::admissible(SignedInPlayer::saved_for_remote_play("")));
}

TEST(LinkConfigurationOverInternet, RefusesAPs4OlderThanVersion10) {
  auto configuration = Configurations::remote();
  configuration.console.target = RegistrationTarget::ps4_8;
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
  configuration = Configurations::remote();
  configuration.console.target = RegistrationTarget::ps4_10;
  EXPECT_TRUE(Configurations::accepts(std::move(configuration)));
}

TEST(LinkConfigurationOverInternet, RefusesAnUnknownCodec) {
  auto configuration = Configurations::remote();
  configuration.codec = static_cast<remote_play::VideoCodec>(99);
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationOverInternet, RefusesAnUnknownFeedbackMode) {
  auto configuration = Configurations::remote();
  configuration.feedback = static_cast<remote_play::FeedbackMode>(99);
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationOverInternet, AcceptsAnEmptyVideoProfile) {
  auto configuration = Configurations::remote();
  configuration.video = {
      .width = 0, .height = 0, .max_fps = 0, .bitrate_kbps = 0};
  EXPECT_TRUE(Configurations::accepts(std::move(configuration)));
}

TEST(LinkConfigurationOverInternet, RefusesAZeroStartTimeout) {
  auto configuration = Configurations::remote();
  configuration.start_timeout_us = 0;
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationOverInternet, AcceptsForcedPortGuessing) {
  auto configuration = Configurations::remote();
  configuration.force_port_guessing = true;
  configuration.port_guess_count = 40;
  configuration.nat_probe_socket_count = 100;
  EXPECT_TRUE(Configurations::accepts(std::move(configuration)));
}

TEST(LinkConfigurationOverInternet, BoundsThePortGuessCount) {
  auto configuration = Configurations::remote();
  configuration.port_guess_count = InternetLinkConfiguration::port_guess_limit;
  EXPECT_TRUE(Configurations::accepts(std::move(configuration)));
  configuration = Configurations::remote();
  configuration.port_guess_count =
      InternetLinkConfiguration::port_guess_limit + 1;
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationOverInternet, BoundsTheNatProbeSocketCount) {
  auto configuration = Configurations::remote();
  configuration.nat_probe_socket_count =
      InternetLinkConfiguration::nat_probe_socket_limit + 1;
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationOverInternet, PicksTheControllerTheTargetExpects) {
  auto psn = Configurations::remote();
  EXPECT_TRUE(psn.prefer_controller(ControllerPreference::console_default));
  EXPECT_TRUE(psn.enable_dualsense_ctrl);
  psn.console.target = RegistrationTarget::ps4_10;
  EXPECT_TRUE(psn.prefer_controller(ControllerPreference::console_default));
  EXPECT_FALSE(psn.enable_dualsense_ctrl);
}

}  // namespace
}  // namespace play_test
