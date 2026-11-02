#include "tests/unit/api/chiaki_ng/rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace chiaki_ng_test {
namespace {

using ChiakiNgSession = ChiakiNgTest;

TEST_F(ChiakiNgSession, SeatsPlayersReconnectsAndEndsAtSignOut) {
  Household home;
  const auto added = home.chiaki.users().add_home_user(AccountId{home_account},
                                                       "Living Room Player");
  ASSERT_TRUE(added);
  const auto player = added.value();
  play::testing::ScriptedLink link{home.threading, home.steady};
  link.connect();
  const auto started = home.chiaki.play().start_scripted(
      player, Where::home, link, {}, home.media, home.events);
  ASSERT_TRUE(started);
  const auto session = started.value();
  home.events.until_state(SessionState::playing);
  EXPECT_EQ(home.chiaki.play().live(), session);

  EXPECT_TRUE(session->seats().add_player(
      seat(1), libchiaking::controllers::ControllerKind::dualsense));
  test_support::play::SessionRig::eventually(
      [&] { return link.count("seat_join") == 1; });
  link.join(seat(1), {1, 2, 3});
  home.events.until("seat_joined");
  EXPECT_EQ(session->seats().seat(seat(1)).state(), play::SeatState::playing);

  link.fail(play::EndReason{play::EndReason::Cause::control_network_failure});
  test_support::play::SessionRig::eventually([&] {
    const auto progress = home.events.progress();
    return !progress.empty() && progress.back().reconnecting;
  });
  EXPECT_EQ(session->state(), SessionState::connecting);
  EXPECT_FALSE(session->outcome());
  EXPECT_TRUE(session->seats().seat(seat(1)).is_empty());
  home.clock.skip(std::chrono::seconds{3});
  link.connect();
  test_support::play::SessionRig::eventually([&] {
    return std::ranges::count(home.events.states(), SessionState::playing) == 2;
  });

  EXPECT_TRUE(home.chiaki.sign_out(player->account_id()));
  EXPECT_TRUE(session->wait_until_ended().was_left());
  EXPECT_EQ(home.chiaki.play().live(), nullptr);
  EXPECT_TRUE(home.chiaki.users().everyone().empty());
}

}  // namespace
}  // namespace chiaki_ng_test
