#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/openssl/household.hpp"

#include <gtest/gtest.h>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::SignedInPlayer;
namespace {

namespace consoles = libchiaking::consoles;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
using play::SessionState;
using play::Where;
using Cause = play::EndReason::Cause;
using Generation = consoles::Generation;
using Reason = consoles::PairingFailure::Reason;

TEST(PlayInternetPairing, IsRefusedWhileASessionPlays) {
  Household home;
  const auto player = SignedInPlayer::fresh_for_remote_play();
  const auto which = Household::device(std::byte{9});
  play::testing::ScriptedLink link{LiveProviders::threading(), LiveProviders::steady()};
  const auto started = home.play.start_scripted(player, Where::home, link, {},
                                                home.media, home.events);
  ASSERT_TRUE(started);
  const auto connecting =
      home.pairer.pair(*player, which, Generation::ps5, nullptr, {});
  ASSERT_FALSE(connecting);
  EXPECT_EQ(connecting.error().reason, Reason::session_playing);
  link.connect();
  home.events.until_state(SessionState::playing);
  const auto playing =
      home.pairer.pair(*player, which, Generation::ps5, nullptr, {});
  ASSERT_FALSE(playing);
  EXPECT_EQ(playing.error().reason, Reason::session_playing);
  link.fail(play::EndReason{Cause::control_network_failure});
  Household::reconnecting(home.events, 2, play::ConnectStep::waking_console,
                          Cause::control_network_failure);
  const auto dropped =
      home.pairer.pair(*player, which, Generation::ps5, nullptr, {});
  ASSERT_FALSE(dropped);
  EXPECT_EQ(dropped.error().reason, Reason::session_playing);
  EXPECT_FALSE(home.pairer.core().is_pairing());
  EXPECT_EQ(home.ps.requests, 0U);
  EXPECT_EQ(home.websocket.connects.load(), 0);

  EXPECT_TRUE(started.value()->leave());
  static_cast<void>(started.value()->wait_until_ended());
  platform::StopSource stopped;
  EXPECT_TRUE(stopped.request_stop());
  const auto after = home.pairer.pair(*player, which, Generation::ps5, nullptr,
                                      stopped.token());
  ASSERT_FALSE(after);
  EXPECT_NE(after.error().reason, Reason::session_playing);
}

}  // namespace
}  // namespace play_test
