#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/openssl/household.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace play_test {

using test_support::play::ScriptedCloudPs;
using test_support::play::SignedInPlayer;
namespace {

namespace consoles = libchiaking::consoles;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace shared = libchiaking::progress;
using play::SessionState;
using play::StartRefusal;
using play::Where;
using Cause = play::EndReason::Cause;
using Generation = consoles::Generation;
using Route = shared::InternetRoute;

class PlayInternetStartThroughTheAccountSession : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto player = SignedInPlayer::fresh_for_remote_play();
    const auto paired =
        home_.paired(*player, Household::living_room, Generation::ps5, {},
                     Household::device(std::byte{7}));
    home_.offline.answer =
        ScriptedCloudPs::account_listing("07", R"(["remotePlay"])");
    const auto started = home_.play.start_over_internet(
        player, paired, {}, home_.media, home_.events);
    ASSERT_TRUE(started);
    const auto session = started.value();
    EXPECT_EQ(session->where(), Where::internet);
    EXPECT_EQ(session->console(), paired.get());
    ASSERT_TRUE(home_.skip_until(
        [&] { return session->state() == SessionState::ended; }));
    outcome_ = session->wait_until_ended();
    progress_ = home_.events.progress();
  }

  Household home_;
  std::optional<play::Outcome> outcome_;
  std::vector<play::ConnectProgress> progress_;
};

TEST_F(PlayInternetStartThroughTheAccountSession,
       EndsWhenTheAccountSessionCannotBeOpened) {
  ASSERT_TRUE(outcome_);
  EXPECT_FALSE(outcome_->succeeded());
  EXPECT_FALSE(outcome_->was_left());
  ASSERT_NE(outcome_->reason(), nullptr);
  EXPECT_EQ(outcome_->cause(), Cause::route_failed);
  EXPECT_EQ(outcome_->reason()->step(),
            play::EndReason::Step::account_session_open);
  EXPECT_EQ(outcome_->reason()->http_status(), 404);
  EXPECT_TRUE(ended_as(*outcome_, play::Outcome::Ending::session_failed,
                       Cause::route_failed));
  EXPECT_EQ(home_.play.live(), nullptr);
}

TEST_F(PlayInternetStartThroughTheAccountSession,
       ListsTheConsolesOnceAndOpensTheSessionOnEveryAttempt) {
  EXPECT_EQ(home_.offline.requests, 1);
  EXPECT_EQ(home_.ps.requests, play::StartPolicy::internet_attempts);
  std::uint32_t last_attempt = 1;
  for (const auto& step : progress_)
    last_attempt = std::max(last_attempt, step.attempt);
  EXPECT_EQ(last_attempt, play::StartPolicy::internet_attempts);
  EXPECT_EQ(std::ranges::count(progress_, play::ConnectStep::waking_console,
                               &play::ConnectProgress::step),
            0);
  EXPECT_EQ(
      std::ranges::count_if(
          progress_,
          [](const auto& step) {
            return step.route == Route{Route::Step::opening_account_session} &&
                   step.step == play::ConnectStep::opening_account_session;
          }),
      play::StartPolicy::internet_attempts);
}

TEST_F(PlayInternetStartThroughTheAccountSession,
       WaitsOnceBeforeItsSecondAttempt) {
  const auto again = std::ranges::find_if(
      progress_, [](const auto& step) { return step.wait.has_value(); });
  ASSERT_NE(again, progress_.end());
  EXPECT_EQ(again->attempt, 2U);
  EXPECT_FALSE(again->reconnecting);
  EXPECT_FALSE(again->route);
  EXPECT_EQ(again->wait,
            (shared::Wait{shared::WaitReason::connection_retry,
                          play::StartPolicy::rest_between_attempts}));
  ASSERT_TRUE(again->previous_failure);
  EXPECT_EQ(again->previous_failure->cause, Cause::route_failed);
  EXPECT_EQ(
      std::ranges::count_if(
          progress_, [](const auto& step) { return step.wait.has_value(); }),
      1);
}

TEST_F(PlayInternetStartThroughTheAccountSession, KeepsTheBearerOutOfTheLog) {
  for (const auto& line : home_.log.messages)
    EXPECT_EQ(line.find(SignedInPlayer::remote_play_bearer), std::string::npos)
        << line;
}

class PlayInternetStartCancellation : public ::testing::Test {
 protected:
  auto start(const platform::StopSource& stop) {
    return Household::refusal(home_.play.start_over_internet(
        signed_in_, unlinked_, {}, home_.media, home_.events, stop.token()));
  }

  Household home_;
  std::shared_ptr<libchiaking::users::PsUser> signed_in_ =
      SignedInPlayer::fresh_for_remote_play();
  std::shared_ptr<consoles::Console> unlinked_ =
      home_.paired(*signed_in_, "aabbccddee03", Generation::ps5);
};

TEST_F(PlayInternetStartCancellation, AsksNothingWhenStoppedBeforeItBegins) {
  platform::StopSource before;
  EXPECT_TRUE(before.request_stop());
  EXPECT_EQ(start(before), StartRefusal::Kind::cancelled);
  EXPECT_EQ(home_.offline.requests, 0);
}

TEST_F(PlayInternetStartCancellation, IsCancelledWhileItAsksForTheConsoleList) {
  platform::StopSource during;
  home_.offline.interrupt = [&during] {
    static_cast<void>(during.request_stop());
  };
  EXPECT_EQ(start(during), StartRefusal::Kind::cancelled);
  EXPECT_EQ(home_.offline.requests, 1);
  home_.offline.interrupt = nullptr;
}

TEST_F(PlayInternetStartCancellation,
       FindsTheConsoleUnreachableWhenTheListIsNotAnswered) {
  platform::StopSource untouched;
  const auto unanswered = home_.play.start_over_internet(
      signed_in_, unlinked_, {}, home_.media, home_.events, untouched.token());
  EXPECT_EQ(Household::refusal(unanswered),
            StartRefusal::Kind::console_unreachable);
  ASSERT_FALSE(unanswered);
  EXPECT_EQ(unanswered.error().unreachable(), consoles::Unreachable::network);
  EXPECT_EQ(home_.play.live(), nullptr);
  EXPECT_TRUE(home_.events.names().empty());
  EXPECT_EQ(home_.ps.requests, 0U);
  EXPECT_EQ(home_.websocket.connects.load(), 0);
}

}  // namespace
}  // namespace play_test
