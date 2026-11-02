#include "tests/support/enum_printing.hpp"
#include "tests/support/play/public_household.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::PublicEvents;
using test_support::play::PublicHousehold;
using test_support::play::PublicMedia;
using test_support::play::SignedInPlayer;
namespace {

namespace play = libchiaking::play;
using play::LaunchActivity;
using play::LaunchProgress;
using play::SessionState;
using play::Where;
using play::testing::ScriptedLink;

class ScriptedCloudStart : public ::testing::Test {
 protected:
  static constexpr auto service =
      libchiaking::catalog::ServiceGeneration::ps5_cloud;

  static auto at(LaunchActivity activity) -> LaunchProgress {
    return *LaunchProgress::at(service, activity);
  }

  static auto queued_at(std::uint32_t position) -> LaunchProgress {
    return *LaunchProgress::at(service, LaunchActivity::allocating_server,
                               {.queue_position = position});
  }

  PublicHousehold home;
  PublicMedia media;
  PublicEvents events;
};

TEST_F(ScriptedCloudStart, ReportsProgressScriptedBeforeAndAfterTheStart) {
  ScriptedLink link{LiveProviders::threading(), LiveProviders::steady()};
  link.launch_progress(at(LaunchActivity::reserving_server));
  const auto started = home.play.start_scripted(
      SignedInPlayer::fresh(), Where::cloud, link, {}, media, events);
  ASSERT_TRUE(started);
  link.launch_progress(at(LaunchActivity::measuring_datacenters));
  link.launch_progress(queued_at(3));
  link.connect();
  link.launch_progress(at(LaunchActivity::connecting_to_server));
  link.finish();
  static_cast<void>(started.value()->wait_until_ended());
  const std::vector expected{at(LaunchActivity::reserving_server),
                             at(LaunchActivity::measuring_datacenters),
                             queued_at(3),
                             at(LaunchActivity::connecting_to_server)};
  EXPECT_EQ(events.launches, expected);
  EXPECT_TRUE(events.reached(SessionState::playing));
  ASSERT_FALSE(events.states.empty());
  EXPECT_EQ(events.states.back(), SessionState::ended);
}

TEST_F(ScriptedCloudStart, KeepsProgressAfterTheScriptingLinkIsGone) {
  auto link = std::make_unique<ScriptedLink>(LiveProviders::threading(), LiveProviders::steady());
  link->launch_progress(at(LaunchActivity::reserving_server));
  const auto started = home.play.start_scripted(
      SignedInPlayer::fresh(), Where::cloud, *link, {}, media, events);
  ASSERT_TRUE(started);
  link->launch_progress(at(LaunchActivity::connecting_to_server));
  link->connect();
  link->finish();
  link.reset();
  static_cast<void>(started.value()->wait_until_ended());
  const std::vector expected{at(LaunchActivity::reserving_server),
                             at(LaunchActivity::connecting_to_server)};
  EXPECT_EQ(events.launches, expected);
  ASSERT_FALSE(events.states.empty());
  EXPECT_EQ(events.states.back(), SessionState::ended);
}

TEST_F(ScriptedCloudStart, RefusesAHomeUserTheCloudButStartsItAtHome) {
  ScriptedLink link{LiveProviders::threading(), LiveProviders::steady()};
  link.launch_progress(at(LaunchActivity::reserving_server));
  const auto refused = home.play.start_scripted(home.home_user(), Where::cloud,
                                                link, {}, media, events);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind(),
            play::StartRefusal::Kind::needs_psn_sign_in);
  EXPECT_TRUE(events.launches.empty());
  EXPECT_TRUE(events.states.empty());
  EXPECT_TRUE(link.calls().empty());
  EXPECT_EQ(home.play.live(), nullptr);

  const auto at_home = home.play.start_scripted(home.home_user(41), Where::home,
                                                link, {}, media, events);
  ASSERT_TRUE(at_home);
  link.connect();
  link.finish();
  static_cast<void>(at_home.value()->wait_until_ended());
  EXPECT_TRUE(events.launches.empty());
  EXPECT_TRUE(events.reached(SessionState::playing));
}

}  // namespace
}  // namespace play_test
