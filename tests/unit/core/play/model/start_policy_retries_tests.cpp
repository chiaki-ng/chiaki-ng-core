#include "src/core/play/model/start_policy.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/detail/play/outcome.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <initializer_list>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using play::StartPolicy;
using play::Where;
using Cause = play::EndReason::Cause;
using Quit = play::ConsoleQuit::Reason;
using Next = StartPolicy::Next;

static_assert(StartPolicy::home_attempts == 13 &&
              StartPolicy::internet_attempts == 2);
static_assert(StartPolicy::rest_between_attempts == std::chrono::seconds{2});

class StartPolicyRetries : public ::testing::Test {
 protected:
  static auto failed(Cause cause) -> StartPolicy::Failure {
    return {.cause = cause};
  }

  static auto quit(Quit reason) -> StartPolicy::Failure {
    return {.quit = reason};
  }
};

TEST_F(StartPolicyRetries, WakesAHomeConsoleFirstAndAgainWhileRefused) {
  for (const auto cause :
       {Cause::control_network_failure, Cause::control_timeout,
        Cause::session_request_network_failure, Cause::session_request_timeout,
        Cause::session_request_connection_refused}) {
    SCOPED_TRACE(testing::PrintToString(cause));
    EXPECT_TRUE(StartPolicy::worth_another_try(Where::home, failed(cause)));
    StartPolicy policy{Where::home};
    EXPECT_EQ(policy.attempt(), 1u);
    EXPECT_EQ(policy.after(failed(cause)), Next::wake_then_try_again);
    EXPECT_EQ(policy.attempt(), 2u);
    const bool refused = cause == Cause::session_request_connection_refused;
    for (std::uint32_t attempt = 3; attempt <= StartPolicy::home_attempts;
         ++attempt) {
      SCOPED_TRACE(attempt);
      const bool wakes =
          refused && (attempt - 2) % StartPolicy::refused_wake_every == 0;
      EXPECT_EQ(policy.after(failed(cause)),
                wakes ? Next::wake_then_try_again : Next::try_again);
      EXPECT_EQ(policy.attempt(), attempt);
    }
    EXPECT_EQ(policy.after(failed(cause)), Next::give_up);
    EXPECT_EQ(policy.attempt(), 13u);
    EXPECT_EQ(policy.after(failed(cause)), Next::give_up);
  }
}

TEST_F(StartPolicyRetries, WakesAgainWhenTheConsoleQuitRefusingTheRequest) {
  for (const auto reason : {Quit::session_request_unknown,
                            Quit::session_request_connection_refused}) {
    SCOPED_TRACE(testing::PrintToString(reason));
    StartPolicy policy{Where::home};
    EXPECT_EQ(policy.after(quit(reason)), Next::wake_then_try_again);
    EXPECT_EQ(policy.after(quit(reason)), Next::try_again);
    EXPECT_EQ(policy.after(quit(reason)), Next::try_again);
    EXPECT_EQ(policy.after(quit(reason)),
              reason == Quit::session_request_connection_refused
                  ? Next::wake_then_try_again
                  : Next::try_again);
  }
}

TEST_F(StartPolicyRetries, GivesUpAtHomeOnAFailureATryCannotFix) {
  for (const auto cause :
       {Cause::session_request_in_use, Cause::session_request_version_mismatch,
        Cause::session_request_refused, Cause::login_pin_rejected,
        Cause::invalid_preferences, Cause::route_failed, Cause::pipeline_failed,
        Cause::control_refused, Cause::control_connection_refused}) {
    SCOPED_TRACE(testing::PrintToString(cause));
    EXPECT_FALSE(StartPolicy::worth_another_try(Where::home, failed(cause)));
    StartPolicy policy{Where::home};
    EXPECT_EQ(policy.after(failed(cause)), Next::give_up);
    EXPECT_EQ(policy.attempt(), 1u);
    EXPECT_EQ(policy.after(failed(Cause::control_timeout)), Next::give_up);
  }
  for (const auto reason :
       {Quit::stopped, Quit::console_in_use, Quit::remote_play_crashed,
        Quit::remote_play_version_mismatch, Quit::console_shut_down,
        Quit::console_disconnected, Quit::time_limit_reached}) {
    SCOPED_TRACE(testing::PrintToString(reason));
    EXPECT_FALSE(StartPolicy::worth_another_try(Where::home, quit(reason)));
  }
  EXPECT_FALSE(StartPolicy::worth_another_try(
      Where::home, {.cause = Cause::session_request_in_use,
                    .quit = Quit::session_request_unknown}));
  EXPECT_FALSE(StartPolicy::worth_another_try(
      Where::home, {.cause = Cause::control_timeout, .left = true}));
  EXPECT_FALSE(StartPolicy::worth_another_try(Where::home, {}));
}

TEST_F(StartPolicyRetries, StaysGivenUpAfterAHopelessFailure) {
  StartPolicy stopped{Where::home};
  EXPECT_EQ(stopped.after(failed(Cause::control_timeout)),
            Next::wake_then_try_again);
  EXPECT_EQ(stopped.after(failed(Cause::session_request_in_use)),
            Next::give_up);
  EXPECT_EQ(stopped.after(failed(Cause::control_timeout)), Next::give_up);
  EXPECT_EQ(stopped.attempt(), 2u);
}

TEST_F(StartPolicyRetries, WaitsOutTheConsoleClosingRightAfterLeaving) {
  StartPolicy handover{Where::home, play::ConsoleHandover::just_left};
  for (std::uint32_t attempt = 2; attempt <= StartPolicy::handover_attempts + 1;
       ++attempt) {
    SCOPED_TRACE(attempt);
    EXPECT_EQ(handover.after(failed(Cause::session_request_in_use)),
              Next::try_again);
    EXPECT_EQ(handover.attempt(), attempt);
  }
  EXPECT_EQ(handover.after(failed(Cause::session_request_in_use)),
            Next::give_up);

  StartPolicy then_refused{Where::home, play::ConsoleHandover::just_left};
  EXPECT_EQ(then_refused.after(failed(Cause::session_request_in_use)),
            Next::try_again);
  EXPECT_EQ(
      then_refused.after(failed(Cause::session_request_connection_refused)),
      Next::wake_then_try_again);
  EXPECT_EQ(then_refused.attempt(), 3u);
}

TEST_F(StartPolicyRetries, WaitsOutAClosingConsoleOnlyAfterLeavingItAtHome) {
  StartPolicy cancelled{Where::home, play::ConsoleHandover::just_left};
  EXPECT_EQ(
      cancelled.after({.cause = Cause::session_request_in_use, .left = true}),
      Next::give_up);

  StartPolicy fresh{Where::home, play::ConsoleHandover::fresh};
  EXPECT_EQ(fresh.after(failed(Cause::session_request_in_use)), Next::give_up);

  StartPolicy internet{Where::internet, play::ConsoleHandover::just_left};
  EXPECT_EQ(internet.after(failed(Cause::session_request_in_use)),
            Next::try_again);
  EXPECT_EQ(internet.after(failed(Cause::route_failed)), Next::try_again);
  EXPECT_EQ(internet.after(failed(Cause::route_failed)), Next::give_up);

  StartPolicy cloud{Where::cloud, play::ConsoleHandover::just_left};
  EXPECT_EQ(cloud.after(failed(Cause::session_request_in_use)), Next::give_up);
}

TEST_F(StartPolicyRetries, TriesAnInternetStartOnceMoreWithoutAWake) {
  for (const auto cause : {Cause::route_failed, Cause::control_network_failure,
                           Cause::session_request_network_failure}) {
    SCOPED_TRACE(testing::PrintToString(cause));
    EXPECT_TRUE(StartPolicy::worth_another_try(Where::internet, failed(cause)));
    StartPolicy policy{Where::internet};
    EXPECT_EQ(policy.after(failed(cause)), Next::try_again);
    EXPECT_EQ(policy.attempt(), 2u);
    EXPECT_EQ(policy.after(failed(cause)), Next::give_up);
    EXPECT_EQ(policy.attempt(), 2u);
  }
}

TEST_F(StartPolicyRetries, GivesUpOnTheInternetOnAFailureATryCannotFix) {
  for (const auto cause :
       {Cause::control_timeout, Cause::session_request_timeout,
        Cause::session_request_connection_refused,
        Cause::session_request_in_use}) {
    SCOPED_TRACE(testing::PrintToString(cause));
    EXPECT_FALSE(
        StartPolicy::worth_another_try(Where::internet, failed(cause)));
  }
  EXPECT_FALSE(StartPolicy::worth_another_try(
      Where::internet, quit(Quit::session_request_unknown)));
  EXPECT_FALSE(StartPolicy::worth_another_try(
      Where::internet, {.cause = Cause::route_failed, .left = true}));
}

TEST_F(StartPolicyRetries, NeverReconnectsASessionEndedOnPurpose) {
  for (const auto reason :
       {Quit::stopped, Quit::console_in_use, Quit::console_disconnected,
        Quit::console_shut_down, Quit::time_limit_reached}) {
    SCOPED_TRACE(testing::PrintToString(reason));
    EXPECT_TRUE(StartPolicy::ended_on_purpose(quit(reason)));
    for (const auto where : {Where::home, Where::internet, Where::cloud}) {
      for (const auto cause :
           {Cause::control_network_failure, Cause::control_timeout,
            Cause::session_request_network_failure,
            Cause::session_request_timeout,
            Cause::session_request_connection_refused, Cause::route_failed,
            Cause::pipeline_failed}) {
        SCOPED_TRACE(testing::PrintToString(where));
        SCOPED_TRACE(testing::PrintToString(cause));
        EXPECT_FALSE(StartPolicy::worth_another_try(
            where, {.cause = cause, .quit = reason}));
        StartPolicy policy{where};
        EXPECT_EQ(policy.after({.cause = cause, .quit = reason}),
                  Next::give_up);
        EXPECT_EQ(policy.attempt(), 1u);
      }
    }
  }
  EXPECT_TRUE(StartPolicy::ended_on_purpose(
      {.cause = Cause::control_timeout, .left = true}));
}

TEST_F(StartPolicyRetries, ReconnectsASessionEndedByAFault) {
  for (const auto reason :
       {Quit::session_request_unknown, Quit::session_request_connection_refused,
        Quit::remote_play_crashed, Quit::remote_play_version_mismatch,
        Quit::control_unknown, Quit::control_connect_failed,
        Quit::control_connection_refused, Quit::stream_unknown,
        Quit::internet_pairing_failed}) {
    SCOPED_TRACE(testing::PrintToString(reason));
    EXPECT_FALSE(StartPolicy::ended_on_purpose(quit(reason)));
  }
  EXPECT_FALSE(StartPolicy::ended_on_purpose({}));
  EXPECT_FALSE(StartPolicy::ended_on_purpose(failed(Cause::pipeline_failed)));
  EXPECT_TRUE(StartPolicy::worth_another_try(
      Where::home, {.cause = Cause::control_network_failure,
                    .quit = Quit::control_unknown}));
  EXPECT_TRUE(StartPolicy::worth_another_try(
      Where::internet,
      {.cause = Cause::route_failed, .quit = Quit::stream_unknown}));
}

TEST_F(StartPolicyRetries, SpendsTheSameAttemptsOnADropAsOnAFailedStart) {
  StartPolicy home{Where::home};
  EXPECT_EQ(home.after(failed(Cause::session_request_timeout)),
            Next::wake_then_try_again);
  EXPECT_EQ(home.after(failed(Cause::control_network_failure)),
            Next::try_again);
  EXPECT_EQ(home.attempt(), 3u);
  for (std::uint32_t attempt = 4; attempt <= StartPolicy::home_attempts;
       ++attempt) {
    SCOPED_TRACE(attempt);
    EXPECT_EQ(home.after(failed(Cause::control_timeout)), Next::try_again);
  }
  EXPECT_EQ(home.after(failed(Cause::control_network_failure)), Next::give_up);

  StartPolicy internet{Where::internet};
  EXPECT_EQ(internet.after(failed(Cause::control_network_failure)),
            Next::try_again);
  EXPECT_EQ(internet.after(failed(Cause::route_failed)), Next::give_up);
  EXPECT_EQ(internet.attempt(), 2u);
}

TEST_F(StartPolicyRetries, NeverTriesACloudStartAgain) {
  for (const auto cause :
       {Cause::route_failed, Cause::control_network_failure,
        Cause::control_timeout, Cause::session_request_network_failure}) {
    SCOPED_TRACE(testing::PrintToString(cause));
    EXPECT_FALSE(StartPolicy::worth_another_try(Where::cloud, failed(cause)));
    StartPolicy policy{Where::cloud};
    EXPECT_EQ(policy.after(failed(cause)), Next::give_up);
    EXPECT_EQ(policy.attempt(), 1u);
  }
  EXPECT_EQ(StartPolicy::attempts(Where::cloud), 1u);
  EXPECT_EQ(StartPolicy::attempts(Where::home), 13u);
  EXPECT_EQ(StartPolicy::attempts(Where::internet), 2u);
}

class StartPolicyDrops : public StartPolicyRetries {
 protected:
  static auto drop(Cause cause, std::chrono::seconds played)
      -> StartPolicy::Failure {
    return {.cause = cause, .played_for = played};
  }
};

TEST_F(StartPolicyDrops, ReconnectsALostConnectionOverTheInternetThreeTimes) {
  StartPolicy policy{Where::internet};
  EXPECT_EQ(policy.after(drop(Cause::connection_lost, std::chrono::seconds{5})),
            Next::try_again);
  EXPECT_EQ(policy.after(failed(Cause::route_failed)), Next::try_again);
  EXPECT_EQ(policy.after(drop(Cause::connection_lost, std::chrono::seconds{5})),
            Next::try_again);
  EXPECT_EQ(policy.after(drop(Cause::connection_lost, std::chrono::seconds{5})),
            Next::give_up);
}

TEST_F(StartPolicyDrops, CountsAfreshOnlyAfterAMinuteOfPlay) {
  StartPolicy policy{Where::internet};
  for (int drop_index = 0; drop_index < 5; ++drop_index) {
    SCOPED_TRACE(drop_index);
    EXPECT_EQ(policy.after(drop(Cause::control_network_failure,
                                StartPolicy::settled_play)),
              Next::try_again);
  }
}

TEST_F(StartPolicyDrops, ReconnectsAConsoleEndOnlyAfterNetworkTrouble) {
  StartPolicy::Failure ended{.quit = Quit::console_disconnected,
                             .played_for = std::chrono::seconds{90}};
  StartPolicy calm{Where::internet};
  EXPECT_EQ(calm.after(ended), Next::give_up);
  ended.since_trouble = StartPolicy::trouble_window + std::chrono::seconds{1};
  StartPolicy long_ago{Where::internet};
  EXPECT_EQ(long_ago.after(ended), Next::give_up);
  ended.since_trouble = StartPolicy::trouble_window;
  StartPolicy troubled{Where::internet};
  EXPECT_EQ(troubled.after(ended), Next::try_again);
  ended.left = true;
  StartPolicy left{Where::internet};
  EXPECT_EQ(left.after(ended), Next::give_up);
}

TEST_F(StartPolicyDrops, NeverReconnectsWhenTurnedOffOrOnTheCloud) {
  StartPolicy off{Where::home, play::ConsoleHandover::fresh, false};
  EXPECT_EQ(
      off.after(drop(Cause::control_network_failure, std::chrono::seconds{90})),
      Next::give_up);
  StartPolicy cloud{Where::cloud};
  EXPECT_EQ(cloud.after(drop(Cause::connection_lost, std::chrono::seconds{90})),
            Next::give_up);
}

TEST_F(StartPolicyDrops, WaitsOutAConsoleStillClosingTheDroppedSession) {
  StartPolicy policy{Where::internet};
  EXPECT_EQ(
      policy.after(drop(Cause::connection_lost, std::chrono::seconds{90})),
      Next::try_again);
  for (std::uint32_t wait = 0; wait < StartPolicy::handover_attempts; ++wait)
    EXPECT_EQ(policy.after(failed(Cause::session_request_in_use)),
              Next::try_again);
  EXPECT_EQ(policy.after(failed(Cause::route_failed)), Next::try_again);
}

static_assert(StartPolicy::offline_wait == std::chrono::seconds{90});

class StartPolicyOffline : public StartPolicyDrops {
 protected:
  static auto offline(std::chrono::seconds offline_for)
      -> StartPolicy::Failure {
    return {.cause = Cause::route_failed, .offline_for = offline_for};
  }

  static auto dropped_policy(Where where) -> StartPolicy {
    StartPolicy policy{where};
    EXPECT_NE(
        policy.after(drop(Cause::connection_lost, std::chrono::seconds{5})),
        Next::give_up);
    return policy;
  }
};

TEST_F(StartPolicyOffline, KeepsReconnectingWithoutCountingWhileOffline) {
  auto policy = dropped_policy(Where::internet);
  for (const auto offline_for :
       {std::chrono::seconds{0}, std::chrono::seconds{45},
        StartPolicy::offline_wait - std::chrono::seconds{1}}) {
    SCOPED_TRACE(offline_for.count());
    EXPECT_EQ(policy.after(offline(offline_for)), Next::try_again);
  }
  EXPECT_EQ(policy.attempt(), 5u);
  for (std::uint32_t left = 1; left < StartPolicy::reconnect_attempts; ++left)
    EXPECT_EQ(policy.after(failed(Cause::route_failed)), Next::try_again);
  EXPECT_EQ(policy.after(failed(Cause::route_failed)), Next::give_up);
}

TEST_F(StartPolicyOffline, CountsOfflineFailuresOnceTheWaitIsOver) {
  auto policy = dropped_policy(Where::internet);
  EXPECT_EQ(policy.after(offline(std::chrono::seconds{89})), Next::try_again);
  for (std::uint32_t left = 1; left < StartPolicy::reconnect_attempts; ++left)
    EXPECT_EQ(policy.after(offline(StartPolicy::offline_wait)),
              Next::try_again);
  EXPECT_EQ(policy.after(offline(std::chrono::seconds{91})), Next::give_up);
}

TEST_F(StartPolicyOffline, TriesTheLastReconnectionAgainWhenItFoundNoNetwork) {
  auto policy = dropped_policy(Where::internet);
  for (std::uint32_t left = 1; left < StartPolicy::reconnect_attempts; ++left)
    EXPECT_EQ(policy.after(failed(Cause::route_failed)), Next::try_again);
  EXPECT_EQ(policy.after(offline(std::chrono::seconds{0})), Next::try_again);
  EXPECT_EQ(policy.after(failed(Cause::route_failed)), Next::give_up);
}

TEST_F(StartPolicyOffline, NeverWakesAHomeConsoleWhileOffline) {
  StartPolicy policy{Where::home};
  EXPECT_EQ(policy.after(
                drop(Cause::control_network_failure, std::chrono::seconds{90})),
            Next::wake_then_try_again);
  for (std::uint32_t wait = 0; wait < StartPolicy::refused_wake_every * 2;
       ++wait)
    EXPECT_EQ(policy.after({.cause = Cause::session_request_connection_refused,
                            .offline_for = std::chrono::seconds{wait}}),
              Next::try_again);
}

TEST_F(StartPolicyOffline, StillGivesUpOnWhatATryCannotFix) {
  auto policy = dropped_policy(Where::internet);
  EXPECT_EQ(policy.after({.cause = Cause::route_failed,
                          .left = true,
                          .offline_for = std::chrono::seconds{0}}),
            Next::give_up);
}

}  // namespace
}  // namespace play_test
