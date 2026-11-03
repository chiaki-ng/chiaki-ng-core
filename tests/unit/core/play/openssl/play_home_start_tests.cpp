#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/openssl/household.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace play_test {

using test_support::play::SessionRig;
namespace {

namespace play = libchiaking::play;
namespace shared = libchiaking::progress;
using play::SessionState;
using play::Where;
using Cause = play::EndReason::Cause;
using Generation = libchiaking::consoles::Generation;
using FakeConsole = libchiaking::testing::FakeConsole;

class PlayHomeStartAgainstARefusingConsole : public ::testing::Test {
 protected:
  void SetUp() override {
    console_.rest();
    const auto player = home_.home_user();
    const auto paired = home_.paired(*player, Household::living_room,
                                     Generation::ps5, "127.0.0.1");
    const auto started = home_.play.start_on_console(player, paired, {},
                                                     home_.media, home_.events);
    ASSERT_TRUE(started);
    const auto session = started.value();
    EXPECT_EQ(session->where(), Where::home);
    EXPECT_EQ(session->console(), paired.get());
    EXPECT_TRUE(home_.play.live() == session ||
                session->state() == SessionState::ended);
    ASSERT_TRUE(home_.skip_rests_until(
        [&] { return session->state() == SessionState::ended; }));
    outcome_ = session->wait_until_ended();
    console_.serve();
    for (std::uint32_t counted = 1; counted < play::StartPolicy::home_attempts;
         ++counted)
      if (counted == 1 ||
          (counted - 1) % play::StartPolicy::refused_wake_every == 0)
        waking_attempts_.push_back(counted + 1);
    progress_ = home_.events.progress();
  }

  Household home_;
  FakeConsole console_{home_.network,
                       FakeConsole::Identity{
                           .hardware_id = std::string{Household::living_room}}};
  std::optional<play::Outcome> outcome_;
  std::vector<std::uint32_t> waking_attempts_;
  std::vector<play::ConnectProgress> progress_;
};

TEST_F(PlayHomeStartAgainstARefusingConsole,
       WakesItWithTheSavedCredentialOnTheAttemptsThePolicyNames) {
  ASSERT_EQ(console_.wakeups().size(), waking_attempts_.size());
  EXPECT_NE(console_.wakeups().front().find(
                "user-credential:" + std::to_string(0x3a5f9c1eU) + "\n"),
            std::string::npos);
  const auto waking =
      std::ranges::count(progress_, play::ConnectStep::waking_console,
                         &play::ConnectProgress::step);
  EXPECT_EQ(waking, static_cast<std::ptrdiff_t>(waking_attempts_.size()));
  std::vector<std::uint32_t> seen_attempts;
  for (const auto& step : progress_)
    if (step.step == play::ConnectStep::waking_console)
      seen_attempts.push_back(step.attempt);
  EXPECT_EQ(seen_attempts, waking_attempts_);
}

TEST_F(PlayHomeStartAgainstARefusingConsole,
       ReportsTheFirstWakeAsAConsoleWakingWait) {
  const auto woke =
      std::ranges::find(progress_, play::ConnectStep::waking_console,
                        &play::ConnectProgress::step);
  ASSERT_NE(woke, progress_.end());
  EXPECT_EQ(woke->attempt, 2U);
  EXPECT_EQ(woke->wait,
            (shared::Wait{shared::WaitReason::console_waking,
                          play::StartPolicy::rest_between_attempts}));
  EXPECT_FALSE(woke->reconnecting);
  ASSERT_TRUE(woke->previous_failure);
  EXPECT_TRUE(woke->previous_failure->cause);
  EXPECT_TRUE(play::StartPolicy::worth_another_try(
      Where::home, {.cause = woke->previous_failure->cause}));
}

TEST_F(PlayHomeStartAgainstARefusingConsole,
       ReportsEveryOtherRetryAsAConnectionRetryWait) {
  for (const auto& step : progress_) {
    if (step.step == play::ConnectStep::waking_console) continue;
    SCOPED_TRACE(step.attempt);
    EXPECT_EQ(step.wait,
              (shared::Wait{shared::WaitReason::connection_retry,
                            play::StartPolicy::rest_between_attempts}));
    ASSERT_TRUE(step.previous_failure);
    EXPECT_TRUE(step.previous_failure->cause);
    EXPECT_FALSE(step.route);
    EXPECT_TRUE(play::StartPolicy::worth_another_try(
        Where::home, {.cause = step.previous_failure->cause}));
  }
}

TEST_F(PlayHomeStartAgainstARefusingConsole, CountsItsAttemptsUpToThePolicy) {
  std::uint32_t last_attempt = 0;
  for (const auto& step : progress_) {
    EXPECT_GE(step.attempt, last_attempt);
    last_attempt = step.attempt;
  }
  EXPECT_EQ(last_attempt, play::StartPolicy::home_attempts);
  EXPECT_GE(progress_.size(), play::StartPolicy::home_attempts - 1);
}

TEST_F(PlayHomeStartAgainstARefusingConsole,
       EndsWithTheRefusalAfterTheLastAttempt) {
  ASSERT_TRUE(outcome_);
  EXPECT_FALSE(outcome_->succeeded());
  EXPECT_FALSE(outcome_->was_left());
  ASSERT_TRUE(outcome_->cause());
  EXPECT_TRUE(play::StartPolicy::worth_another_try(
      Where::home, {.cause = outcome_->cause()}));
  EXPECT_EQ(outcome_->cause(), Cause::session_request_connection_refused);
  EXPECT_TRUE(ended_as(*outcome_, play::Outcome::Ending::session_failed,
                       Cause::session_request_connection_refused));
  EXPECT_EQ(home_.events.states(),
            (std::vector{SessionState::connecting, SessionState::ended}));
  EXPECT_EQ(home_.events.count("ended"), 1U);
  EXPECT_EQ(home_.play.live(), nullptr);
  EXPECT_EQ(home_.ps.requests, 0U);
}

TEST(PlayHomeStart, CanBeLeftBetweenAttempts) {
  Household home;
  const auto player = home.home_user();
  const auto paired = home.paired(*player, Household::living_room,
                                  Generation::ps5, "127.0.0.1");
  FakeConsole console{home.network,
                      FakeConsole::Identity{
                          .hardware_id = std::string{Household::living_room}}};
  const auto started =
      home.play.start_on_console(player, paired, {}, home.media, home.events);
  ASSERT_TRUE(started);
  home.events.until("connect_progress");
  SessionRig::eventually([&] {
    console.serve();
    return !console.wakeups().empty();
  });
  EXPECT_TRUE(started.value()->leave() ||
              started.value()->state() == SessionState::ended);
  const auto& outcome = started.value()->wait_until_ended();
  EXPECT_TRUE(outcome.was_left());
  EXPECT_TRUE(ended_as(outcome, play::Outcome::Ending::left));
  EXPECT_EQ(home.play.live(), nullptr);
}

}  // namespace
}  // namespace play_test
