#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/openssl/household.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::SessionRig;
using test_support::play::SignedInPlayer;
namespace {

namespace controllers = libchiaking::controllers;
namespace play = libchiaking::play;
using play::SessionState;
using play::Where;
using Cause = play::EndReason::Cause;
using Quit = play::ConsoleQuit::Reason;

class PlayReconnection : public ::testing::Test {
 protected:
  void start(Where where) {
    link_.connect();
    const auto started = home_.play.start_scripted(player_, where, link_, {},
                                                   home_.media, home_.events);
    ASSERT_TRUE(started);
    session_ = started.value();
    home_.events.until_state(SessionState::playing);
  }

  Household home_;
  std::shared_ptr<libchiaking::users::PsUser> player_ = SignedInPlayer::fresh();
  play::testing::ScriptedLink link_{LiveProviders::threading(),
                                    LiveProviders::steady()};
  std::shared_ptr<play::Session> session_;
};

class PlayHomeReconnection : public PlayReconnection {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(start(Where::home));
    link_.describe(Household::description(1280, 720));
    link_.show(Household::frame(1));
    SessionRig::eventually([&] { return home_.media.shown().size() == 1; });
    EXPECT_TRUE(session_->seats().add_player(
        Household::seat(1), controllers::ControllerKind::dualsense));
    SessionRig::eventually([&] { return link_.count("seat_join") == 1; });
    link_.join(Household::seat(1), {1, 2, 3});
    home_.events.until("seat_joined");
    EXPECT_EQ(session_->seats().seat(Household::seat(1)).state(),
              play::SeatState::playing);
    EXPECT_TRUE(session_->seats().add_player(
        Household::seat(2), controllers::ControllerKind::dualshock4));
    SessionRig::eventually([&] { return link_.count("seat_join") == 2; });
    EXPECT_EQ(session_->seats().seat(Household::seat(2)).state(),
              play::SeatState::joining);

    link_.fail(play::EndReason{Cause::control_network_failure});
    Household::reconnecting(home_.events, 2, play::ConnectStep::waking_console,
                            Cause::control_network_failure);
  }

  void TearDown() override {
    if (!session_) return;
    static_cast<void>(session_->leave());
    static_cast<void>(session_->wait_until_ended());
  }

  void reconnect() {
    home_.clock.skip(std::chrono::seconds{3});
    link_.connect();
    Household::playing_again(home_.events, 2);
  }
};

TEST_F(PlayHomeReconnection, WakesTheConsoleOnceTheSessionDrops) {
  EXPECT_EQ(home_.events.states(),
            (std::vector{SessionState::connecting, SessionState::playing,
                         SessionState::connecting}));
  EXPECT_EQ(session_->state(), SessionState::connecting);
  EXPECT_EQ(home_.play.live(), session_);
  EXPECT_FALSE(session_->outcome());
  EXPECT_EQ(link_.count("wake_console"), 1U);
  EXPECT_EQ(link_.count("start"), 1U);
  EXPECT_EQ(home_.events.count("ended"), 0U);
}

TEST_F(PlayHomeReconnection, EmptiesEverySeatBeforeItReconnects) {
  EXPECT_EQ(home_.events.count("seat_left"), 2U);
  EXPECT_TRUE(session_->seats().seat(Household::seat(1)).is_empty());
  EXPECT_TRUE(session_->seats().seat(Household::seat(2)).is_empty());
  EXPECT_EQ(session_->seats().seat(controllers::SeatNumber::first()).state(),
            play::SeatState::joining);
  const auto names = home_.events.names();
  ASSERT_FALSE(names.empty());
  EXPECT_EQ(names.back(), "connect_progress");
  const auto fell_back =
      std::ranges::find(names.rbegin(), names.rend(), "state");
  EXPECT_EQ(std::ranges::count(fell_back.base(), names.end(), "seat_left"), 2);
}

TEST_F(PlayHomeReconnection, PlaysAgainWithTheNewStreamOnceReconnected) {
  reconnect();
  EXPECT_EQ(link_.count("start"), 2U);
  EXPECT_EQ(session_->state(), SessionState::playing);
  EXPECT_EQ(session_->seats().seat(controllers::SeatNumber::first()).state(),
            play::SeatState::playing);
  link_.describe(Household::description(1920, 1080));
  link_.show(Household::frame(1));
  SessionRig::eventually([&] { return home_.media.shown().size() == 2; });
  const auto descriptions = home_.media.descriptions();
  ASSERT_EQ(descriptions.size(), 2U);
  EXPECT_EQ(descriptions.front(), Household::description(1280, 720));
  EXPECT_EQ(descriptions.back(), Household::description(1920, 1080));
  const auto shown = home_.media.shown();
  ASSERT_FALSE(shown.empty());
  EXPECT_TRUE(shown.front().profile_changed);
  EXPECT_TRUE(shown.back().profile_changed);
  EXPECT_EQ(shown.back().width, 1920U);
  EXPECT_EQ(shown.back().height, 1080U);
  EXPECT_EQ(link_.count("seat_join"), 2U);
}

TEST_F(PlayHomeReconnection, FindsTheConsoleWithoutAWakeAfterALaterDrop) {
  reconnect();
  EXPECT_TRUE(session_->seats().add_player(
      Household::seat(1), controllers::ControllerKind::dualsense));
  SessionRig::eventually([&] { return link_.count("seat_join") == 3; });
  link_.join(Household::seat(1), {4, 5, 6});
  home_.events.until("seat_joined", 2);

  link_.fail(play::EndReason{Cause::control_timeout});
  Household::reconnecting(home_.events, 3, play::ConnectStep::finding_console,
                          Cause::control_timeout);
  EXPECT_EQ(link_.count("wake_console"), 1U);
  EXPECT_EQ(home_.events.count("seat_left"), 3U);
  home_.clock.skip(std::chrono::seconds{3});
  link_.connect();
  Household::playing_again(home_.events, 3);
  EXPECT_TRUE(session_->leave());
  const auto& outcome = session_->wait_until_ended();
  EXPECT_TRUE(outcome.was_left());
  EXPECT_TRUE(ended_as(outcome, play::Outcome::Ending::left));
  EXPECT_EQ(link_.count("start"), 3U);
  EXPECT_EQ(home_.events.count("ended"), 1U);
  EXPECT_EQ(home_.play.live(), nullptr);
  EXPECT_EQ(home_.events.states(),
            (std::vector{SessionState::connecting, SessionState::playing,
                         SessionState::connecting, SessionState::playing,
                         SessionState::connecting, SessionState::playing,
                         SessionState::leaving, SessionState::ended}));
}

TEST_F(PlayReconnection, EndsAHomeSessionWithTheLastFailureWhenItCannotReturn) {
  ASSERT_NO_FATAL_FAILURE(start(Where::home));
  link_.refuse_start(play::EndReason{Cause::session_request_timeout});
  link_.fail(play::EndReason{Cause::control_network_failure});
  ASSERT_TRUE(home_.skip_until(
      [&] { return session_->state() == SessionState::ended; }));
  const auto& outcome = session_->wait_until_ended();
  EXPECT_FALSE(outcome.succeeded());
  EXPECT_FALSE(outcome.was_left());
  EXPECT_EQ(outcome.cause(), Cause::session_request_timeout);
  EXPECT_TRUE(ended_as(outcome, play::Outcome::Ending::session_failed,
                       Cause::session_request_timeout));
  EXPECT_EQ(link_.count("start"), play::StartPolicy::home_attempts);
  EXPECT_EQ(link_.count("wake_console"), 1U);
  const auto progress = home_.events.progress();
  ASSERT_EQ(progress.size(), play::StartPolicy::home_attempts - 1);
  for (std::uint32_t retry = 0; retry < progress.size(); ++retry) {
    SCOPED_TRACE(retry);
    EXPECT_EQ(progress[retry].attempt, retry + 2);
    EXPECT_TRUE(progress[retry].reconnecting);
    ASSERT_TRUE(progress[retry].wait);
    EXPECT_EQ(progress[retry].wait->next_try_in,
              play::StartPolicy::rest_between_attempts);
    ASSERT_TRUE(progress[retry].previous_failure);
    EXPECT_EQ(progress[retry].previous_failure->cause,
              retry == 0 ? Cause::control_network_failure
                         : Cause::session_request_timeout);
  }
  EXPECT_EQ(home_.events.states(),
            (std::vector{SessionState::connecting, SessionState::playing,
                         SessionState::connecting, SessionState::ended}));
  EXPECT_EQ(home_.events.count("ended"), 1U);
  EXPECT_EQ(home_.play.live(), nullptr);
}

TEST_F(PlayReconnection, ReconnectsAnInternetSessionThreeTimesWithoutAWake) {
  ASSERT_NO_FATAL_FAILURE(start(Where::internet));
  for (std::uint32_t reconnect = 0;
       reconnect < play::StartPolicy::reconnect_attempts; ++reconnect) {
    SCOPED_TRACE(reconnect);
    link_.fail(play::EndReason{Cause::route_failed});
    Household::reconnecting(home_.events, reconnect + 2,
                            play::ConnectStep::opening_account_session,
                            Cause::route_failed);
    home_.clock.skip(std::chrono::seconds{3});
    link_.connect();
    Household::playing_again(home_.events, reconnect + 2);
  }
  link_.fail(play::EndReason{Cause::route_failed});
  const auto& outcome = session_->wait_until_ended();
  EXPECT_EQ(outcome.cause(), Cause::route_failed);
  EXPECT_TRUE(ended_as(outcome, play::Outcome::Ending::session_failed,
                       Cause::route_failed));
  EXPECT_EQ(link_.count("start"), play::StartPolicy::reconnect_attempts + 1U);
  EXPECT_EQ(link_.count("wake_console"), 0U);
}

class PlayEndingWithoutReconnection : public ::testing::Test {
 protected:
  template <class Ending>
  auto ended_once(Where where, const Ending& ending) -> play::Outcome {
    Household home;
    const auto player = SignedInPlayer::fresh();
    play::testing::ScriptedLink link{LiveProviders::threading(),
                                     LiveProviders::steady()};
    link.connect();
    const auto started = home.play.start_scripted(player, where, link, {},
                                                  home.media, home.events);
    EXPECT_TRUE(started);
    const auto session = started.value();
    home.events.until_state(SessionState::playing);
    ending(link, *session);
    auto outcome = session->wait_until_ended();
    EXPECT_EQ(link.count("start"), 1U);
    EXPECT_EQ(link.count("wake_console"), 0U);
    EXPECT_TRUE(home.events.progress().empty());
    const auto states = home.events.states();
    EXPECT_TRUE(
        states == (std::vector{SessionState::connecting, SessionState::playing,
                               SessionState::ended}) ||
        states == (std::vector{SessionState::connecting, SessionState::playing,
                               SessionState::leaving, SessionState::ended}))
        << testing::PrintToString(states);
    return outcome;
  }

  static auto quitting(Quit reason) {
    return [reason](play::testing::ScriptedLink& link, play::Session&) {
      link.quit(play::ConsoleQuit{reason});
    };
  }

  static auto failing(Cause cause) {
    return [cause](play::testing::ScriptedLink& link, play::Session&) {
      link.fail(play::EndReason{cause});
    };
  }
};

TEST_F(PlayEndingWithoutReconnection,
       EndsAHomeOrInternetSessionTheConsoleQuits) {
  for (const auto where : {Where::home, Where::internet}) {
    SCOPED_TRACE(testing::PrintToString(where));
    EXPECT_TRUE(
        ended_as(ended_once(where, quitting(Quit::console_disconnected)),
                 Quit::console_disconnected));
    EXPECT_TRUE(ended_as(ended_once(where, quitting(Quit::console_shut_down)),
                         Quit::console_shut_down));
    EXPECT_TRUE(ended_as(ended_once(where, quitting(Quit::console_in_use)),
                         Quit::console_in_use));
  }
}

TEST_F(PlayEndingWithoutReconnection,
       EndsAHomeOrInternetSessionWhoseMediaPipelineFailed) {
  for (const auto where : {Where::home, Where::internet}) {
    SCOPED_TRACE(testing::PrintToString(where));
    EXPECT_TRUE(ended_as(ended_once(where, failing(Cause::pipeline_failed)),
                         play::Outcome::Ending::session_failed,
                         Cause::pipeline_failed));
  }
}

TEST_F(PlayEndingWithoutReconnection, EndsAHomeOrInternetSessionThatIsLeft) {
  for (const auto where : {Where::home, Where::internet}) {
    SCOPED_TRACE(testing::PrintToString(where));
    EXPECT_TRUE(ended_as(
        ended_once(where,
                   [](play::testing::ScriptedLink&, play::Session& session) {
                     EXPECT_TRUE(session.leave());
                   }),
        play::Outcome::Ending::left));
  }
}

TEST_F(PlayEndingWithoutReconnection, NeverReconnectsACloudSession) {
  EXPECT_TRUE(ended_as(
      ended_once(Where::cloud,
                 [](play::testing::ScriptedLink& link, play::Session&) {
                   link.quit(play::ConsoleQuit{Quit::time_limit_reached,
                                               "MaxPlaytimeTimeout"});
                 }),
      Quit::time_limit_reached));
  EXPECT_TRUE(ended_as(
      ended_once(Where::cloud, failing(Cause::control_network_failure)),
      play::Outcome::Ending::session_failed, Cause::control_network_failure));
  EXPECT_TRUE(ended_as(ended_once(Where::cloud, failing(Cause::route_failed)),
                       play::Outcome::Ending::session_failed,
                       Cause::route_failed));
}

}  // namespace
}  // namespace play_test
