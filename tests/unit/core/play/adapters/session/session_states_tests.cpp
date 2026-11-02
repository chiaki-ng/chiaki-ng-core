#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_case.hpp"

#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

namespace play_test {

using test_support::play::SignedInPlayer;

using SteadyClock = libchiaking::platform::SteadyClock;

using test_support::LiveProviders;
using test_support::play::RecordedEvents;
using test_support::play::RecordedMedia;
using test_support::play::SessionRig;
namespace {

namespace play = libchiaking::play;
using namespace std::chrono_literals;
using libchiaking::catalog::ServiceGeneration;
using play::EndReason;
using play::Outcome;
using play::SessionState;
using play::Where;

class PlaySession : public SessionCase {
 protected:
  static auto through_leaving() -> std::vector<SessionState> {
    return {SessionState::connecting, SessionState::playing,
            SessionState::leaving, SessionState::ended};
  }

  static auto straight_to_ended() -> std::vector<SessionState> {
    return {SessionState::connecting, SessionState::ended};
  }
};

TEST_F(PlaySession, StartsConnectingWithWhatItWasOpenedWith) {
  SessionRig rig;
  EXPECT_EQ(rig.session->state(), SessionState::connecting);
  EXPECT_EQ(rig.session->where(), Where::home);
  EXPECT_EQ(rig.session->console(), nullptr);
  EXPECT_EQ(rig.session->player().online_id(), "player");
  EXPECT_EQ(rig.session->launch(), nullptr);
  EXPECT_FALSE(rig.session->outcome());
}

TEST_F(PlaySession, ReturnsFromBeginWhileConnecting) {
  SessionRig rig;
  rig.begin();
  EXPECT_FALSE(rig.core->begin());
  EXPECT_EQ(rig.session->state(), SessionState::connecting);
  EXPECT_FALSE(rig.session->outcome());
  rig.events.until_state(SessionState::connecting);
  EXPECT_EQ(rig.events.count("ended"), 0U);
}

TEST_F(PlaySession, PlaysAndFinishesOnTheCoresOwnThread) {
  SessionRig rig;
  rig.begin();
  rig.events.until_state(SessionState::connecting);
  rig.link.connect();
  rig.events.until_state(SessionState::playing);
  EXPECT_EQ(rig.session->state(), SessionState::playing);
  rig.link.finish();
  const auto& outcome = rig.session->wait_until_ended();
  EXPECT_TRUE(outcome.succeeded());
  EXPECT_TRUE(ended_as(outcome, Outcome::Ending::finished));
  EXPECT_EQ(outcome.ending(), Outcome::Ending::finished);
  EXPECT_EQ(rig.session->state(), SessionState::ended);
  ASSERT_TRUE(rig.session->outcome());
  EXPECT_TRUE(rig.session->outcome()->succeeded());
  EXPECT_EQ(rig.events.count("ended"), 1U);
  ASSERT_TRUE(rig.events.ended());
  EXPECT_TRUE(rig.events.ended()->succeeded());

  const auto threads = rig.events.threads();
  EXPECT_EQ(threads.size(), 1U);
  EXPECT_FALSE(
      threads.contains(libchiaking::platform::ThreadIdentity::current()));
  const std::vector expected{SessionState::connecting, SessionState::playing,
                             SessionState::ended};
  EXPECT_EQ(rig.events.states(), expected);
  ASSERT_FALSE(rig.events.names().empty());
  EXPECT_EQ(rig.events.names().back(), "ended");
  EXPECT_EQ(rig.link.count("start"), 1U);
  EXPECT_EQ(rig.link.count("stop"), 1U);
}

TEST_F(PlaySession, LetsTwoThreadsWaitForTheSameEnding) {
  SessionRig rig;
  rig.start_playing();
  std::array<const Outcome*, 2> seen{};
  auto first =
      LiveProviders::spawn("first", [&] { seen[0] = &rig.session->wait_until_ended(); });
  auto second =
      LiveProviders::spawn("second", [&] { seen[1] = &rig.session->wait_until_ended(); });
  LiveProviders::nap(20ms);
  EXPECT_EQ(seen[0], nullptr);
  EXPECT_EQ(seen[1], nullptr);
  EXPECT_FALSE(rig.session->outcome());
  EXPECT_TRUE(rig.session->leave());
  static_cast<void>(first.join());
  static_cast<void>(second.join());
  ASSERT_NE(seen[0], nullptr);
  EXPECT_EQ(seen[0], seen[1]);
  EXPECT_EQ(seen[0], &rig.session->wait_until_ended());
  EXPECT_EQ(seen[0]->ending(), Outcome::Ending::left);
  EXPECT_TRUE(ended_as(*seen[0], Outcome::Ending::left));
  EXPECT_TRUE(seen[0]->was_left());
  EXPECT_FALSE(seen[0]->succeeded());
  EXPECT_EQ(rig.events.count("ended"), 1U);
  EXPECT_FALSE(rig.session->leave());
}

TEST_F(PlaySession, PassesThroughLeavingWhenLeft) {
  SessionRig left;
  left.start_playing();
  EXPECT_TRUE(left.session->leave());
  const auto state = left.session->state();
  EXPECT_TRUE(state == SessionState::leaving || state == SessionState::ended)
      << "state " << testing::PrintToString(state);
  left.session->wait_until_ended();
  EXPECT_EQ(left.events.states(), through_leaving());
  EXPECT_EQ(left.events.threads().size(), 1U);
}

TEST_F(PlaySession, PassesThroughLeavingWhenTheConsoleLeaves) {
  SessionRig console_left;
  console_left.start_playing();
  console_left.link.begin_leaving();
  console_left.session->wait_until_ended();
  EXPECT_EQ(console_left.events.states(), through_leaving());
  ASSERT_TRUE(console_left.session->outcome());
  EXPECT_TRUE(console_left.session->outcome()->succeeded());
}

TEST_F(PlaySession, EndsStraightAwayWhenTheStartIsRefused) {
  SessionRig refused;
  refused.link.refuse_start(
      EndReason{EndReason::Cause::control_network_failure,
                {.step = EndReason::Step::control_startup}});
  refused.begin();
  const auto& outcome = refused.session->wait_until_ended();
  EXPECT_EQ(refused.events.states(), straight_to_ended());
  EXPECT_EQ(outcome.ending(), Outcome::Ending::start_failed);
  EXPECT_TRUE(ended_as(outcome, Outcome::Ending::start_failed,
                       EndReason::Cause::control_network_failure));
  EXPECT_EQ(outcome.cause(), EndReason::Cause::control_network_failure);
  ASSERT_NE(outcome.reason(), nullptr);
  EXPECT_EQ(outcome.reason()->step(), EndReason::Step::control_startup);
  EXPECT_EQ(refused.link.count("start"), 1U);
}

TEST_F(PlaySession, NeverPlaysWhenLeftBeforeItBegins) {
  SessionRig before_start;
  EXPECT_TRUE(before_start.session->leave());
  EXPECT_EQ(before_start.session->state(), SessionState::leaving);
  before_start.begin();
  EXPECT_TRUE(ended_as(before_start.session->wait_until_ended(),
                       Outcome::Ending::left));
  const std::vector never_played{SessionState::connecting,
                                 SessionState::leaving, SessionState::ended};
  EXPECT_EQ(before_start.events.states(), never_played);
}

TEST_F(PlaySession, EndsStraightAwayWhenTheLinkFinishesWhileConnecting) {
  SessionRig connecting;
  connecting.begin();
  connecting.events.until_state(SessionState::connecting);
  connecting.link.finish();
  connecting.session->wait_until_ended();
  EXPECT_EQ(connecting.events.states(), straight_to_ended());
}

class PlaySessionOpening : public SessionCase {
 protected:
  auto open(std::shared_ptr<libchiaking::users::PsUser> player,
            std::unique_ptr<play::Link> wire, Where where,
            std::optional<ServiceGeneration> service) {
    return play::Session::Core::open({.player = std::move(player),
                                      .console = nullptr,
                                      .where = where,
                                      .service = service,
                                      .link = std::move(wire),
                                      .stop = {},
                                      .media = media_,
                                      .events = events_,
                                      .clock = clock_,
                                      .threading = LiveProviders::threading(),
                                      .journal = nullptr});
  }

  auto wire(Where where) -> std::unique_ptr<play::Link> {
    return link_.core().link({}, where);
  }

  auto player() const -> const std::shared_ptr<libchiaking::users::PsUser>& {
    return player_;
  }

 private:
  RecordedEvents events_;
  RecordedMedia media_;
  SteadyClock clock_;
  play::testing::ScriptedLink link_{LiveProviders::threading(), LiveProviders::steady()};
  std::shared_ptr<libchiaking::users::PsUser> player_ = SignedInPlayer::fresh();
};

TEST_F(PlaySessionOpening, NeedsAPlayer) {
  const auto no_player =
      open(nullptr, wire(Where::home), Where::home, std::nullopt);
  ASSERT_FALSE(no_player);
  EXPECT_EQ(no_player.error(), EndReason::Cause::invalid_preferences);
}

TEST_F(PlaySessionOpening, NeedsALink) {
  const auto no_link = open(player(), nullptr, Where::home, std::nullopt);
  ASSERT_FALSE(no_link);
  EXPECT_EQ(no_link.error(), EndReason::Cause::invalid_preferences);
}

TEST_F(PlaySessionOpening, NeedsAServiceOnlyForTheCloud) {
  EXPECT_FALSE(open(player(), wire(Where::cloud), Where::cloud, std::nullopt));
  EXPECT_FALSE(open(player(), wire(Where::home), Where::home,
                    ServiceGeneration::ps5_cloud));
  const auto cloud = open(player(), wire(Where::cloud), Where::cloud,
                          ServiceGeneration::ps5_cloud);
  ASSERT_TRUE(cloud);
  EXPECT_EQ(cloud.value()->where(), Where::cloud);
  EXPECT_EQ(cloud.value()->cloud_service(), ServiceGeneration::ps5_cloud);
}

}  // namespace
}  // namespace play_test
