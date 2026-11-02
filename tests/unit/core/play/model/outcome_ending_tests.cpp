#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/model/outcome_rig.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>

namespace play_test {
namespace {

class OutcomeEnding : public OutcomeRig {};

TEST_F(OutcomeEnding, KeepsTheCauseOfASessionThatNeverStarted) {
  for (const auto& cause : causes) {
    SCOPED_TRACE(testing::PrintToString(cause.typed));
    const auto outcome = Outcome::not_started(cause.typed);
    EXPECT_EQ(outcome.ending(), Ending::not_started);
    EXPECT_EQ(outcome.cause(), cause.typed);
    EXPECT_FALSE(outcome.succeeded());
    EXPECT_TRUE(ended_as(outcome, Ending::not_started, cause.typed));
  }
}

TEST_F(OutcomeEnding, PutsANeverStartedSessionAboveEverythingElse) {
  Outcome::Evidence typed;
  typed.not_started = Cause::route_failed;
  typed.left = true;
  typed.phase = Phase::media;
  typed.quit = ConsoleQuit{ConsoleQuit::Reason::console_shut_down};
  ends(typed, {Ending::not_started, Cause::route_failed});
}

TEST_F(OutcomeEnding, FollowsEveryPhaseAndCause) {
  const Ended finished{Ending::finished, std::nullopt};
  const Ended left_early{Ending::left, std::nullopt};
  const Ended unknown{Ending::session_failed, Cause::pipeline_failed};
  for (const auto phase : phases) {
    for (const auto& cause : causes) {
      for (const bool left : {false, true}) {
        for (const bool with_primary : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << testing::PrintToString(phase) << ' '
                       << testing::PrintToString(cause.typed)
                       << " left=" << left << " primary=" << with_primary);
          Outcome::Evidence typed;
          typed.phase = phase;
          typed.phase_cause = cause.typed;
          typed.left = left;
          if (with_primary) typed.primary = end_reason(cause, left);
          Ended caused = unknown;
          if (phase == Phase::none)
            caused = finished;
          else if (phase == Phase::start)
            caused = {Ending::start_failed, cause.typed};
          else if (phase == Phase::session)
            caused = {Ending::session_failed, cause.typed};
          ends(typed, left ? left_early : caused);

          typed.phase_cause.reset();
          ends(typed, left                   ? left_early
                      : phase == Phase::none ? finished
                                             : unknown);
        }
      }
    }
  }
}

TEST_F(OutcomeEnding, TellsAFailedStartFromAFailedSession) {
  Outcome::Evidence started;
  started.phase = Phase::start;
  started.phase_cause = Cause::control_network_failure;
  EXPECT_TRUE(ended_as(Outcome{started}, Ending::start_failed,
                       Cause::control_network_failure));
  EXPECT_EQ(Outcome{started}.ending(), Ending::start_failed);
  started.phase = Phase::session;
  EXPECT_TRUE(ended_as(Outcome{started}, Ending::session_failed,
                       Cause::control_network_failure));
  EXPECT_EQ(Outcome{started}.ending(), Ending::session_failed);
  started.phase = Phase::session_unknown;
  EXPECT_TRUE(ended_as(Outcome{started}, Ending::session_failed,
                       Cause::pipeline_failed));
  EXPECT_EQ(Outcome{started}.cause(), Cause::pipeline_failed);
  started.phase = Phase::session;
  started.phase_cause = Cause::session_request_connection_refused;
  EXPECT_TRUE(ended_as(Outcome{started}, Ending::session_failed,
                       Cause::session_request_connection_refused));
  started.phase = Phase::start;
  started.phase_cause = Cause::control_timeout;
  EXPECT_TRUE(
      ended_as(Outcome{started}, Ending::start_failed, Cause::control_timeout));
}

TEST_F(OutcomeEnding, RanksLeavingThenCleanupThenFailure) {
  Outcome::Evidence evidence;
  evidence.left = true;
  evidence.phase = Phase::start;
  evidence.phase_cause = Cause::route_failed;
  EXPECT_TRUE(ended_as(Outcome{evidence}, Ending::left));
  EXPECT_EQ(Outcome{evidence}.ending(), Ending::left);

  evidence.primary = EndReason{Cause::route_failed, {.left = true}};
  evidence.cleanup =
      EndReason{Cause::shutdown_failed, {.step = EndReason::Step::shutdown}};
  EXPECT_TRUE(ended_as(Outcome{evidence}, Ending::shutdown_failed,
                       Cause::shutdown_failed));
  EXPECT_EQ(Outcome{evidence}.reason(), nullptr);
  ASSERT_NE(Outcome{evidence}.cleanup(), nullptr);
  EXPECT_EQ(Outcome{evidence}.cleanup()->step(), EndReason::Step::shutdown);

  evidence.cleanup.reset();
  evidence.stop_cause = Cause::shutdown_failed;
  EXPECT_TRUE(ended_as(Outcome{evidence}, Ending::shutdown_failed,
                       Cause::shutdown_failed));
  EXPECT_EQ(Outcome{evidence}.ending(), Ending::shutdown_failed);

  evidence.primary = EndReason{Cause::route_failed};
  evidence.phase_cause = Cause::control_failed;
  EXPECT_TRUE(
      ended_as(Outcome{evidence}, Ending::start_failed, Cause::control_failed));
  EXPECT_EQ(Outcome{evidence}.ending(), Ending::start_failed);
  const Outcome failed{evidence};
  ASSERT_NE(failed.reason(), nullptr);
  EXPECT_EQ(failed.reason()->cause(), Cause::route_failed);
}

}  // namespace
}  // namespace play_test
