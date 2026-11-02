#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/openssl/household.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::SignedInPlayer;
using test_support::play::Title;
namespace {

namespace consoles = libchiaking::consoles;
namespace play = libchiaking::play;
using play::StartRefusal;
using play::Where;
using Generation = consoles::Generation;

class PlayStartRefusal : public ::testing::Test {
 protected:
  void nothing_was_sent() {
    EXPECT_EQ(home_.play.live(), nullptr);
    EXPECT_TRUE(home_.events.names().empty());
    EXPECT_EQ(home_.ps.requests, 0U);
    EXPECT_EQ(home_.websocket.connects.load(), 0);
    EXPECT_EQ(home_.clearance.clears, 0U);
  }

  Household home_;
  std::shared_ptr<libchiaking::users::PsUser> signed_in_ =
      SignedInPlayer::fresh_for_remote_play();
  std::shared_ptr<libchiaking::users::PsUser> home_user_ = home_.home_user();
  std::shared_ptr<libchiaking::users::PsUser> stranger_ = home_.home_user(41);
  std::shared_ptr<consoles::Console> console_ = home_.paired(
      *home_user_, Household::living_room, Generation::ps5, "127.0.0.1");
  std::shared_ptr<consoles::Console> unaddressed_ =
      home_.paired(*home_user_, "aabbccddee02", Generation::ps5);
  std::shared_ptr<consoles::Console> unlinked_ =
      home_.paired(*signed_in_, "aabbccddee03", Generation::ps5);
  libchiaking::catalog::ProductId product_ = Title::product_id(Household::cloud_title);
};

TEST_F(PlayStartRefusal, RefusesAMissingPlayerOrConsole) {
  EXPECT_EQ(Household::refusal(home_.play.start_on_console(
                nullptr, console_, {}, home_.media, home_.events)),
            StartRefusal::Kind::invalid_preferences);
  EXPECT_EQ(Household::refusal(home_.play.start_on_console(
                home_user_, nullptr, {}, home_.media, home_.events)),
            StartRefusal::Kind::invalid_preferences);
  nothing_was_sent();
}

TEST_F(PlayStartRefusal, RefusesAPlayerTheConsoleIsNotPairedWith) {
  EXPECT_EQ(Household::refusal(home_.play.start_on_console(
                stranger_, console_, {}, home_.media, home_.events)),
            StartRefusal::Kind::not_paired);
  nothing_was_sent();
}

TEST_F(PlayStartRefusal, RefusesAConsoleWithNoAddress) {
  const auto nowhere = home_.play.start_on_console(home_user_, unaddressed_, {},
                                                   home_.media, home_.events);
  EXPECT_EQ(Household::refusal(nowhere),
            StartRefusal::Kind::console_unreachable);
  ASSERT_FALSE(nowhere);
  EXPECT_EQ(nowhere.error().unreachable(), consoles::Unreachable::no_address);
  nothing_was_sent();
}

TEST_F(PlayStartRefusal, AsksAHomeUserToSignInBeforeAnInternetOrCloudStart) {
  EXPECT_EQ(Household::refusal(home_.play.start_over_internet(
                home_user_, console_, {}, home_.media, home_.events)),
            StartRefusal::Kind::needs_psn_sign_in);
  EXPECT_EQ(Household::refusal(home_.play.start_on_cloud(
                home_user_, product_, {}, {}, home_.media, home_.events)),
            StartRefusal::Kind::needs_psn_sign_in);
  play::testing::ScriptedLink link{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(Household::refusal(home_.play.start_scripted(
                home_user_, Where::cloud, link, {}, home_.media, home_.events)),
            StartRefusal::Kind::needs_psn_sign_in);
  nothing_was_sent();
}

TEST_F(PlayStartRefusal, RefusesAnInternetConsoleTheAccountCannotFind) {
  const auto unknown = home_.play.start_over_internet(
      signed_in_, unlinked_, {}, home_.media, home_.events);
  EXPECT_EQ(Household::refusal(unknown),
            StartRefusal::Kind::console_unreachable);
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().unreachable(), consoles::Unreachable::network);
  EXPECT_GE(home_.offline.requests, 1);
  nothing_was_sent();
}

TEST_F(PlayStartRefusal, RefusesAForgottenConsole) {
  EXPECT_TRUE(home_.known.forget(*unaddressed_));
  const auto forgotten = home_.play.start_on_console(
      home_user_, unaddressed_, {}, home_.media, home_.events);
  EXPECT_EQ(Household::refusal(forgotten),
            StartRefusal::Kind::console_unreachable);
  ASSERT_FALSE(forgotten);
  EXPECT_EQ(forgotten.error().unreachable(), consoles::Unreachable::forgotten);
  nothing_was_sent();
}

TEST_F(PlayStartRefusal, RefusesAnInternetLinkToAPs4OnFirmwareNine) {
  using Target = consoles::PairedPlay::InternetTarget;
  const auto legacy = play::InternetLinks::configured(
      Target{Household::device(std::byte{1}),
             consoles::Firmware{consoles::Firmware::Family::ps4_9}},
      signed_in_, {}, {}, {},
      play::InternetLinkConfiguration::default_start_timeout);
  ASSERT_FALSE(legacy);
  EXPECT_EQ(legacy.error(), play::ConfigurationError::invalid_console);
}

}  // namespace
}  // namespace play_test
