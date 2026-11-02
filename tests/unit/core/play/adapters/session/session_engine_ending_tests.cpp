#include "src/core/play/adapters/links/link_stages.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_engine_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::testing::ScriptedSession;
using Cause = play::EndReason::Cause;
using Ending = play::Outcome::Ending;
using StreamError = remote_play::StreamError;
using StreamState = remote_play::StreamState;

TEST(SessionEngineEnding, LeavesWithoutPumpingWhenCancelledEarly) {
  SessionEngineRig early;
  early.runner->cancel();
  EXPECT_TRUE(early.stop.stop_requested());
  const auto outcome = early.run();
  EXPECT_EQ(outcome.ending(), Ending::left);
  EXPECT_TRUE(ended_as(outcome, Ending::left));
  EXPECT_EQ(outcome.reason(), nullptr);
  EXPECT_EQ(early.session->calls, (std::vector<std::string>{"start", "stop"}));
}

TEST(SessionEngineEnding, StopsLocallyForHostSleep) {
  SessionEngineRig asleep;
  asleep.streaming();
  asleep.then([&asleep](ScriptedSession&) {
    asleep.runner->stop_for_host_sleep();
    asleep.runner->cancel();
    EXPECT_FALSE(asleep.stop.stop_requested());
    EXPECT_TRUE(asleep.runner->lifecycle().sleeping());
    asleep.session->stop_when_idle = false;
  });
  const auto outcome = asleep.run();
  EXPECT_EQ(asleep.session->pumps, 2);
  EXPECT_EQ(asleep.session->sleep_stops, 1);
  EXPECT_EQ(asleep.session->stops, 0);
  EXPECT_EQ(outcome.ending(), Ending::left);
  EXPECT_EQ(
      asleep.logged("System sleep: stopping local session without PSN cleanup"),
      1U);
}

TEST(SessionEngineEnding, IgnoresHostSleepOnceCancelled) {
  SessionEngineRig cancelled;
  cancelled.streaming();
  cancelled.then([&cancelled](ScriptedSession&) {
    cancelled.runner->cancel();
    cancelled.runner->stop_for_host_sleep();
  });
  const auto outcome = cancelled.run();
  EXPECT_EQ(cancelled.session->sleep_stops, 0);
  EXPECT_EQ(cancelled.session->stops, 1);
  EXPECT_TRUE(cancelled.stop.stop_requested());
  EXPECT_EQ(outcome.ending(), Ending::left);
  EXPECT_EQ(cancelled.logged("System sleep"), 0U);
}

TEST(SessionEngineEnding, CancelsWhenHostSleepComesBeforeTheRun) {
  SessionEngineRig idle;
  idle.runner->stop_for_host_sleep();
  EXPECT_TRUE(idle.stop.stop_requested());
  EXPECT_FALSE(idle.runner->lifecycle().sleeping());
  EXPECT_TRUE(idle.runner->lifecycle().cancelled());
}

TEST(SessionEngineEnding, ClassifiesAFailedStart) {
  SessionEngineRig start;
  start.session->start_result =
      libchiaking::unexpected(StreamError::ctrl_timeout);
  const auto outcome = start.run();
  EXPECT_EQ(outcome.ending(), Ending::start_failed);
  EXPECT_TRUE(ended_as(outcome, Ending::start_failed, Cause::control_timeout));
  EXPECT_EQ(outcome.cause(), Cause::control_timeout);
  EXPECT_EQ(start.session->stops, 1);
  EXPECT_EQ(start.session->pumps, 0);
}

TEST(SessionEngineEnding, ClassifiesAReportedSessionFailure) {
  SessionEngineRig known;
  known.streaming();
  known.then([](ScriptedSession& s) {
    s.fail(SessionEngineRig::failure(StreamError::route_failed));
  });
  const auto outcome = known.run();
  EXPECT_EQ(outcome.ending(), Ending::session_failed);
  EXPECT_TRUE(ended_as(outcome, Ending::session_failed, Cause::route_failed));
  ASSERT_NE(outcome.reason(), nullptr);
  EXPECT_EQ(outcome.reason()->cause(), Cause::route_failed);
  EXPECT_EQ(outcome.reason()->step(), play::EndReason::Step::session_request);
  EXPECT_EQ(outcome.cause(), Cause::route_failed);
  EXPECT_TRUE(outcome.trace().starts_with(
      "route=local stage=session_init result=route_failed "));
  EXPECT_EQ(known.events.count("ended"), 1U);
  ASSERT_TRUE(known.events.ended());
  EXPECT_TRUE(ended_as(*known.events.ended(), Ending::session_failed,
                       Cause::route_failed));
  EXPECT_EQ(
      known.logged(
          "session ended: Local console: " +
          std::string{
              play::LinkStages::name(remote_play::FailureStage::session_init)} +
          " — " +
          std::string{
              SessionEngineRig::failure(StreamError::route_failed).describe()}),
      1U);
  EXPECT_EQ(known.logged("session cleanup failed: "), 0U);
  EXPECT_EQ(known.logged("session failure: route=local stage="), 1U);
}

TEST(SessionEngineEnding, ClassifiesASilentFailureAsAPipelineFailure) {
  SessionEngineRig unknown;
  unknown.streaming();
  unknown.then([](ScriptedSession& s) { s.fail_silently(); });
  const auto outcome = unknown.run();
  EXPECT_EQ(outcome.ending(), Ending::session_failed);
  EXPECT_TRUE(
      ended_as(outcome, Ending::session_failed, Cause::pipeline_failed));
}

TEST(SessionEngineEnding, ClassifiesAFailedWait) {
  SessionEngineRig waiting;
  waiting.session->wait_result =
      libchiaking::unexpected(StreamError::pipeline_failed);
  waiting.streaming();
  const auto outcome = waiting.run();
  EXPECT_TRUE(
      ended_as(outcome, Ending::session_failed, Cause::pipeline_failed));
  EXPECT_EQ(waiting.session->pumps, 1);
  EXPECT_EQ(waiting.session->waits.size(), 1U);
}

TEST(SessionEngineEnding, ClassifiesAFailedShutdown) {
  SessionEngineRig shutdown;
  shutdown.session->stop_result =
      libchiaking::unexpected(StreamError::shutdown_failed);
  const auto outcome = shutdown.run();
  EXPECT_EQ(outcome.ending(), Ending::shutdown_failed);
  EXPECT_TRUE(
      ended_as(outcome, Ending::shutdown_failed, Cause::shutdown_failed));
}

TEST(SessionEngineEnding, ClassifiesAConsoleQuitWithItsText) {
  SessionEngineRig quit;
  quit.streaming();
  quit.then([](ScriptedSession& s) {
    s.quit_with(remote_play::QuitReason::stream_connection_remote_shutdown,
                "bye");
    s.set_state(StreamState::stopped);
  });
  const auto outcome = quit.run();
  EXPECT_EQ(outcome.ending(), Ending::console_quit);
  EXPECT_TRUE(ended_as(outcome, play::ConsoleQuit::Reason::console_shut_down));
  ASSERT_TRUE(outcome.quit());
  EXPECT_EQ(outcome.quit()->reason(),
            play::ConsoleQuit::Reason::console_shut_down);
  EXPECT_EQ(outcome.quit()->text(), "bye");
  EXPECT_EQ(outcome.trace(),
            "quit=stream_connection_remote_shutdown console_reason=\"bye\"");
}

TEST(SessionEngineEnding, ForgivesWhateverFollowsARestRequest) {
  SessionEngineRig h;
  h.streaming();
  h.then([&h](ScriptedSession&) {
    EXPECT_TRUE(
        h.runner->stop_with_rest(play::RestRequest::Reason::host_sleep));
  });
  h.then([&h](ScriptedSession& s) {
    h.runner->video_output_failed(3);
    s.fail(SessionEngineRig::failure(StreamError::route_failed));
    s.fail_cleanup(SessionEngineRig::failure(
        StreamError::shutdown_failed, remote_play::FailureStage::shutdown));
  });
  const auto outcome = h.run();
  EXPECT_EQ(h.session->count("goto_bed"), 1U);
  EXPECT_EQ(h.session->sleep_stops, 1);
  EXPECT_EQ(h.session->stops, 0);
  EXPECT_EQ(outcome.ending(), Ending::left);
  EXPECT_TRUE(ended_as(outcome, Ending::left));
  EXPECT_EQ(outcome.rest(), play::RestOutcome::sent);
  EXPECT_EQ(outcome.reason(), nullptr);
  EXPECT_EQ(outcome.cleanup(), nullptr);
  EXPECT_FALSE(outcome.media_fault());
  EXPECT_FALSE(outcome.quit());
  EXPECT_TRUE(outcome.trace().empty());
  EXPECT_EQ(h.logged("Stream ended after the rest mode request"), 1U);
  EXPECT_EQ(h.logged("Media stopped after the rest mode request: Video "
                     "decoder output"),
            1U);
}

TEST(SessionEngineEnding, RefusesToRestACloudConsole) {
  SessionEngineRig cloud{
      play::Venue::cloud(libchiaking::catalog::ServiceGeneration::ps5_cloud)};
  cloud.streaming();
  cloud.then([&cloud](ScriptedSession&) {
    EXPECT_FALSE(
        cloud.runner->stop_with_rest(play::RestRequest::Reason::leaving));
    EXPECT_TRUE(cloud.stop.stop_requested());
  });
  const auto refused = cloud.run();
  EXPECT_EQ(refused.rest(), play::RestOutcome::refused);
  EXPECT_EQ(refused.ending(), Ending::left);
  EXPECT_EQ(cloud.session->count("goto_bed"), 0U);
}

}  // namespace
}  // namespace play_test
