#include "src/core/play/adapters/links/link_evidence.hpp"
#include "src/core/play/adapters/links/session_diagnostics.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_engine_rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::media::MediaFailure;
using libchiaking::media::MediaFault;
using play::LinkEvidence;
using play::Outcome;
using Cause = play::EndReason::Cause;
using Ending = Outcome::Ending;
using Phase = Outcome::Phase;
using StreamError = remote_play::StreamError;

class SessionOutcome : public ::testing::Test {
 protected:
  static auto left_at_start() -> Outcome::Evidence {
    Outcome::Evidence evidence;
    evidence.left = true;
    evidence.phase = Phase::start;
    evidence.phase_cause = Cause::route_failed;
    return evidence;
  }

  static auto shutdown_failure() -> play::LinkFailure {
    return SessionEngineRig::failure(StreamError::shutdown_failed,
                                     remote_play::FailureStage::shutdown);
  }

  static auto abandoned_with_cleanup() -> Outcome::Evidence {
    auto evidence = left_at_start();
    auto abandoned = SessionEngineRig::failure(StreamError::route_failed);
    abandoned.cancel();
    evidence.primary = abandoned.end_reason();
    evidence.cleanup = shutdown_failure().end_reason();
    return evidence;
  }

  static auto stopped_badly() -> Outcome::Evidence {
    auto evidence = abandoned_with_cleanup();
    evidence.cleanup.reset();
    evidence.stop_cause = Cause::shutdown_failed;
    return evidence;
  }

  static auto quit(remote_play::QuitReason reason) -> Outcome::Evidence {
    Outcome::Evidence evidence;
    evidence.quit = LinkEvidence::quit({reason, {}});
    return evidence;
  }

  static auto refused_session_quit() -> Outcome::Evidence {
    auto evidence =
        quit(remote_play::QuitReason::session_request_connection_refused);
    evidence.phase = Phase::session;
    evidence.phase_cause =
        LinkEvidence::cause(StreamError::session_init_connection_refused);
    return evidence;
  }

  static auto reported_failure() -> play::LinkFailure {
    auto detail = play::LinkFailure::reported(
        play::EndReason{Cause::control_timeout,
                        {.step = play::EndReason::Step::control_startup,
                         .console_reason = 0x80108b12,
                         .http_status = 503,
                         .action_url = "https://example.test/go",
                         .detail = "boom"}});
    detail.reached_through(play::Venue::internet());
    return detail;
  }

  static auto console_quit() -> remote_play::QuitDetail {
    return {remote_play::QuitReason::ctrl_unknown, "console text"};
  }

  static auto reported_outcome() -> Outcome {
    const auto detail = reported_failure();
    const auto cleanup = shutdown_failure();
    Outcome::Evidence evidence;
    evidence.phase = Phase::session;
    evidence.phase_cause = LinkEvidence::cause(StreamError::ctrl_timeout);
    evidence.primary = detail.end_reason();
    evidence.cleanup = cleanup.end_reason();
    evidence.quit = LinkEvidence::quit(console_quit());
    evidence.trace = play::SessionDiagnostics::outcome_trace(detail, cleanup,
                                                             console_quit());
    return Outcome{evidence};
  }
};

TEST_F(SessionOutcome, LeavingOutranksAPhaseFailure) {
  const Outcome outcome{left_at_start()};
  EXPECT_TRUE(ended_as(outcome, Ending::left));
  EXPECT_EQ(outcome.ending(), Ending::left);
}

TEST_F(SessionOutcome, AFailedCleanupOutranksAnAbandonedFailure) {
  const Outcome outcome{abandoned_with_cleanup()};
  EXPECT_TRUE(
      ended_as(outcome, Ending::shutdown_failed, Cause::shutdown_failed));
  EXPECT_EQ(outcome.reason(), nullptr);
  ASSERT_NE(outcome.cleanup(), nullptr);
  EXPECT_EQ(outcome.cleanup()->cause(), Cause::shutdown_failed);
  EXPECT_EQ(outcome.cleanup()->step(), play::EndReason::Step::shutdown);
  EXPECT_EQ(outcome.cause(), Cause::shutdown_failed);
}

TEST_F(SessionOutcome, AFailedStopCountsAsAFailedShutdown) {
  const Outcome outcome{stopped_badly()};
  EXPECT_TRUE(
      ended_as(outcome, Ending::shutdown_failed, Cause::shutdown_failed));
  EXPECT_EQ(outcome.ending(), Ending::shutdown_failed);
}

TEST_F(SessionOutcome, APhaseCauseOutranksTheReportedFailure) {
  auto evidence = stopped_badly();
  evidence.primary =
      SessionEngineRig::failure(StreamError::route_failed).end_reason();
  evidence.phase_cause = Cause::control_failed;
  const Outcome outcome{evidence};
  EXPECT_TRUE(ended_as(outcome, Ending::start_failed, Cause::control_failed));
  EXPECT_EQ(outcome.ending(), Ending::start_failed);
}

TEST_F(SessionOutcome, AConsoleThatShutDownEndsAsAQuit) {
  EXPECT_TRUE(ended_as(
      Outcome{quit(remote_play::QuitReason::stream_connection_remote_shutdown)},
      play::ConsoleQuit::Reason::console_shut_down));
}

TEST_F(SessionOutcome, AStoppedStreamFinishes) {
  const Outcome outcome{quit(remote_play::QuitReason::stopped)};
  EXPECT_TRUE(ended_as(outcome, Ending::finished));
  EXPECT_TRUE(outcome.succeeded());
}

TEST_F(SessionOutcome, NoQuitReasonIsNoQuit) {
  const auto evidence = quit(remote_play::QuitReason::none);
  EXPECT_FALSE(evidence.quit);
  EXPECT_TRUE(ended_as(Outcome{evidence}, Ending::finished));
}

TEST_F(SessionOutcome, LeavingOutranksAConsoleQuit) {
  auto evidence =
      quit(remote_play::QuitReason::stream_connection_remote_shutdown);
  evidence.left = true;
  EXPECT_TRUE(ended_as(Outcome{evidence}, Ending::left));
}

TEST_F(SessionOutcome, ARefusedSessionQuitKeepsItsCause) {
  EXPECT_TRUE(ended_as(Outcome{refused_session_quit()}, Ending::session_failed,
                       Cause::session_request_connection_refused));
}

TEST_F(SessionOutcome, APhaseCauseOutranksAQuit) {
  auto evidence = refused_session_quit();
  evidence.phase_cause = Cause::route_failed;
  EXPECT_TRUE(
      ended_as(Outcome{evidence}, Ending::session_failed, Cause::route_failed));
}

TEST_F(SessionOutcome, AnUnknownSessionFailureIsAPipelineFailure) {
  auto evidence = refused_session_quit();
  evidence.phase = Phase::session_unknown;
  evidence.phase_cause.reset();
  const Outcome outcome{evidence};
  EXPECT_TRUE(
      ended_as(outcome, Ending::session_failed, Cause::pipeline_failed));
  EXPECT_EQ(outcome.cause(), Cause::pipeline_failed);
}

TEST_F(SessionOutcome, AMediaFaultEndsTheRunOnlyInTheMediaPhase) {
  Outcome::Evidence clean_media;
  clean_media.media =
      MediaFault{MediaFailure::decoder_output, "Video decoder output", 7};
  EXPECT_TRUE(ended_as(Outcome{clean_media}, Ending::finished));
  EXPECT_EQ(
      Outcome{clean_media}.media_fault(),
      (MediaFault{MediaFailure::decoder_output, "Video decoder output", 7}));
  clean_media.phase = Phase::media;
  EXPECT_TRUE(ended_as(Outcome{clean_media}, MediaFailure::decoder_output));
}

TEST_F(SessionOutcome, NotStartedCarriesItsCause) {
  const auto again = Outcome::not_started(Cause::invalid_state);
  EXPECT_EQ(again.ending(), Ending::not_started);
  EXPECT_TRUE(ended_as(again, Ending::not_started, Cause::invalid_state));
}

TEST_F(SessionOutcome, AnInvalidConfigurationNeverStartsForInvalidPreferences) {
  const auto invalid = Outcome::not_started(
      LinkEvidence::cause(StreamError::invalid_configuration));
  EXPECT_EQ(invalid.ending(), Ending::not_started);
  EXPECT_TRUE(
      ended_as(invalid, Ending::not_started, Cause::invalid_preferences));
}

TEST_F(SessionOutcome, AnUnavailableProviderNeverStarts) {
  const auto unavailable = Outcome::not_started(
      LinkEvidence::cause(StreamError::provider_unavailable));
  EXPECT_TRUE(
      ended_as(unavailable, Ending::not_started, Cause::provider_unavailable));
  EXPECT_EQ(unavailable.cause(), Cause::provider_unavailable);
  EXPECT_EQ(unavailable.ending(), Ending::not_started);
  EXPECT_EQ(unavailable.reason(), nullptr);
  EXPECT_TRUE(unavailable.trace().empty());
}

TEST_F(SessionOutcome, CarriesTheReportedFailureTyped) {
  const auto outcome = reported_outcome();
  EXPECT_TRUE(
      ended_as(outcome, Ending::session_failed, Cause::control_timeout));
  ASSERT_NE(outcome.reason(), nullptr);
  EXPECT_EQ(outcome.reason()->action_url(), "https://example.test/go");
  EXPECT_EQ(outcome.reason()->cause(), Cause::control_timeout);
  EXPECT_EQ(outcome.reason()->step(), play::EndReason::Step::control_startup);
  EXPECT_EQ(outcome.reason()->http_status(), 503);
  EXPECT_EQ(outcome.reason()->console_reason(), 0x80108b12);
  EXPECT_EQ(outcome.reason()->detail(), "boom");
}

TEST_F(SessionOutcome, CarriesTheCleanupAndTheQuitTyped) {
  const auto outcome = reported_outcome();
  ASSERT_NE(outcome.cleanup(), nullptr);
  EXPECT_EQ(outcome.cleanup()->cause(), Cause::shutdown_failed);
  EXPECT_EQ(outcome.cleanup()->step(), play::EndReason::Step::shutdown);
  ASSERT_TRUE(outcome.quit());
  EXPECT_EQ(outcome.quit()->reason(),
            play::ConsoleQuit::Reason::control_unknown);
  EXPECT_EQ(outcome.quit()->text(), "console text");
}

TEST_F(SessionOutcome, TracesTheFailureForLogs) {
  EXPECT_EQ(reported_outcome().trace(),
            "route=remote stage=ctrl_startup result=ctrl_timeout HTTP=503 "
            "reason=0x80108b12 "
            "received=0 cancelled=0 detail=\"the response timed out\" cleanup: "
            "stage=shutdown "
            "result=shutdown_failed detail=\"resource cleanup failed\" "
            "quit=ctrl_unknown "
            "console_reason=\"console text\" error_message=\"boom\"");
}

TEST_F(SessionOutcome, KeepsTheActionUrlOutOfTheSentence) {
  EXPECT_EQ(reported_failure().sentence().find("https://"), std::string::npos);
}

TEST_F(SessionOutcome, TracesAConsoleRefusalAsEightHexDigits) {
  const auto odd =
      play::LinkFailure{remote_play::FailureStage::session_init,
                        StreamError::session_init_refused,
                        play::LinkFailure::ConsoleRefusal{0x1f, 0}};
  EXPECT_NE(play::SessionDiagnostics::outcome_trace(odd, shutdown_failure(),
                                                    console_quit())
                .find(" reason=0x0000001f "),
            std::string::npos);
}

}  // namespace
}  // namespace play_test
