#include "src/core/play/adapters/links/link_failure.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/link_failure_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using link_failures::Failures;
using Stage = remote_play::FailureStage;
using Error = remote_play::StreamError;
using Quit = remote_play::QuitReason;
using Failure = remote_play::ActiveSessionFailure;

static_assert(remote_play::quit_text_limit == 255);

class LinkFailureQuit : public ::testing::Test {
 protected:
  static auto disconnected(std::optional<std::string> reason = std::nullopt)
      -> Failure {
    return Failure{remote_play::ActiveHandshakeState::ready,
                   remote_play::ActiveSessionError::peer_disconnected,
                   {},
                   std::move(reason)};
  }

  static auto silent(std::optional<std::string> reason = std::nullopt)
      -> Failure {
    return Failure{remote_play::ActiveHandshakeState::ready,
                   remote_play::ActiveSessionError::peer_silent,
                   {},
                   std::move(reason)};
  }

  static auto quit_of(Stage stage, Error error, std::uint32_t reason = 0,
                      bool cancelled = false) -> Quit {
    play::LinkFailure failure{stage, error,
                              play::LinkFailure::ConsoleRefusal{reason, 0}};
    if (cancelled) failure.cancel();
    return failure.quit().reason;
  }
};

TEST_F(LinkFailureQuit, CarriesAConsoleShutdownAndItsText) {
  const auto shutdown = disconnected("Server shutting down");
  EXPECT_TRUE(play::LinkFailure::shuts_down(shutdown));
  const auto shut = play::LinkFailure::lost(shutdown).quit();
  EXPECT_EQ(shut.reason, Quit::stream_connection_remote_shutdown);
  EXPECT_EQ(shut.text, "Server shutting down");
  EXPECT_FALSE(remote_play::quit_reason_is_error(shut.reason));
}

TEST_F(LinkFailureQuit, CarriesAConsoleDisconnectWithItsTextCut) {
  const auto ended = disconnected(std::string(300, 'x'));
  EXPECT_FALSE(play::LinkFailure::shuts_down(ended));
  const auto quit = play::LinkFailure::lost(ended).quit();
  EXPECT_EQ(quit.reason, Quit::stream_connection_remote_disconnected);
  EXPECT_EQ(quit.text, std::string(255, 'x'));
  EXPECT_TRUE(remote_play::quit_reason_is_error(quit.reason));
}

TEST_F(LinkFailureQuit, NamesTheConsoleReasonOfALostMediaStream) {
  const auto ended = disconnected("Remote Play session ended");
  EXPECT_TRUE(Failures::contains(
      Failures::at(play::Venue::home(), play::LinkFailure::lost(ended))
          .sentence(),
      "Console reason: Remote Play session ended."));
  EXPECT_EQ(play::LinkFailure::lost(ended).stage(), Stage::media);
}

TEST_F(LinkFailureQuit, GivesASilentPeerNoKnownReason) {
  EXPECT_EQ(play::LinkFailure::lost(silent()).quit().reason,
            Quit::stream_connection_unknown);
}

TEST_F(LinkFailureQuit, EndsATrialOnAServerShutdownAfterAWarning) {
  auto ended = disconnected("Shutdown requested by server");
  EXPECT_EQ(play::LinkFailure::peer_quit(ended, true),
            Quit::stream_connection_trial_ended);
  EXPECT_FALSE(
      remote_play::quit_reason_is_error(Quit::stream_connection_trial_ended));
  EXPECT_EQ(play::LinkFailure::peer_quit(ended),
            Quit::stream_connection_remote_disconnected);
  EXPECT_EQ(play::LinkFailure::peer_quit(ended, false),
            Quit::stream_connection_remote_disconnected);
  ended.peer_reason = std::string{"Remote Play session ended"};
  EXPECT_EQ(play::LinkFailure::peer_quit(ended, true),
            Quit::stream_connection_remote_disconnected);
  ended.peer_reason = std::string{"MaxPlaytimeTimeout"};
  EXPECT_EQ(play::LinkFailure::peer_quit(ended, true),
            Quit::stream_connection_time_limit_reached);
  EXPECT_EQ(play::LinkFailure::peer_quit(silent(), true),
            Quit::stream_connection_unknown);
}

TEST_F(LinkFailureQuit, EndsTheStreamAsALostConnectionWhenThePeerFallsSilent) {
  EXPECT_EQ(play::LinkFailure::lost(silent()).end_cause(),
            play::EndReason::Cause::connection_lost);
  EXPECT_NE(play::LinkFailure::lost(disconnected()).end_cause(),
            play::EndReason::Cause::connection_lost);
}

TEST_F(LinkFailureQuit, TreatsTheCloudTimeLimitAsACleanQuit) {
  const auto timed_out = disconnected("MaxPlaytimeTimeout");
  EXPECT_FALSE(play::LinkFailure::shuts_down(timed_out));
  EXPECT_EQ(play::LinkFailure::peer_quit(timed_out),
            Quit::stream_connection_time_limit_reached);
  const auto quit = play::LinkFailure::lost(timed_out).quit();
  EXPECT_EQ(quit.reason, Quit::stream_connection_time_limit_reached);
  EXPECT_EQ(quit.text, "MaxPlaytimeTimeout");
  EXPECT_FALSE(remote_play::quit_reason_is_error(quit.reason));
}

TEST_F(LinkFailureQuit, MatchesTheTimeLimitReasonExactly) {
  auto timed_out = disconnected();
  for (const auto* near :
       {"maxplaytimetimeout", "MaxPlaytimeTimeout ", "MaxPlaytime"}) {
    SCOPED_TRACE(near);
    timed_out.peer_reason = std::string{near};
    EXPECT_EQ(play::LinkFailure::peer_quit(timed_out),
              Quit::stream_connection_remote_disconnected);
  }
  timed_out.peer_reason.reset();
  EXPECT_EQ(play::LinkFailure::peer_quit(timed_out),
            Quit::stream_connection_remote_disconnected);
  timed_out.peer_reason = "Server shutting down";
  EXPECT_EQ(play::LinkFailure::peer_quit(timed_out),
            Quit::stream_connection_remote_shutdown);
  EXPECT_EQ(play::LinkFailure::peer_quit(silent("MaxPlaytimeTimeout")),
            Quit::stream_connection_unknown);
}

TEST_F(LinkFailureQuit, DefaultsToTheReferenceReasonForASessionRequest) {
  EXPECT_EQ(quit_of(Stage::session_init, Error::session_init_in_use),
            Quit::session_request_rp_in_use);
  EXPECT_EQ(
      quit_of(Stage::session_init, Error::session_init_refused, 0x80108b15U),
      Quit::session_request_rp_crash);
  EXPECT_EQ(quit_of(Stage::session_init, Error::session_init_version_mismatch),
            Quit::session_request_rp_version_mismatch);
  EXPECT_EQ(quit_of(Stage::session_init, Error::session_init_timeout),
            Quit::session_request_unknown);
  EXPECT_EQ(
      quit_of(Stage::session_init, Error::session_init_connection_refused),
      Quit::session_request_connection_refused);
  EXPECT_EQ(quit_of(Stage::session_init, Error::route_failed, 0, true),
            Quit::stopped);
}

TEST_F(LinkFailureQuit, DefaultsToTheReferenceReasonForControl) {
  EXPECT_EQ(quit_of(Stage::ctrl_receive, Error::ctrl_failed),
            Quit::ctrl_unknown);
  for (const auto error :
       {Error::ctrl_connection_refused, Error::ctrl_timeout,
        Error::ctrl_refused, Error::ctrl_network_failure, Error::ctrl_failed}) {
    SCOPED_TRACE(testing::PrintToString(error));
    EXPECT_EQ(quit_of(Stage::ctrl_startup, error), Quit::ctrl_connect_failed);
  }
  EXPECT_EQ(quit_of(Stage::ctrl_session_id, Error::ctrl_failed),
            Quit::ctrl_unknown);
}

TEST_F(LinkFailureQuit, DefaultsToTheReferenceReasonForTheStreamAndCloud) {
  EXPECT_EQ(quit_of(Stage::stream_info, Error::pipeline_failed),
            Quit::stream_connection_unknown);
  EXPECT_EQ(quit_of(Stage::cloud_allocate, Error::route_failed), Quit::none);
}

TEST(LinkFailureReported, SurvivesTheRoundTripThroughAnEndReason) {
  using play::EndReason;
  const EndReason reason{EndReason::Cause::control_refused,
                         {.step = EndReason::Step::control_startup,
                          .console_reason = 0x80108b10U,
                          .http_status = 503,
                          .left = true,
                          .action_url = "https://example.invalid/settings",
                          .detail = "refused"}};
  const auto failure = play::LinkFailure::reported(reason);
  EXPECT_EQ(failure.stage(), Stage::ctrl_startup);
  EXPECT_EQ(failure.result(), Error::ctrl_refused);
  EXPECT_TRUE(failure.cancelled());
  EXPECT_EQ(failure.quit().reason, Quit::stopped);
  const auto back = failure.end_reason();
  EXPECT_EQ(back.cause(), reason.cause());
  EXPECT_EQ(back.step(), reason.step());
  EXPECT_EQ(back.console_reason(), reason.console_reason());
  EXPECT_EQ(back.http_status(), reason.http_status());
  EXPECT_TRUE(back.was_left());
  EXPECT_EQ(back.action_url(), reason.action_url());
  EXPECT_EQ(back.detail(), reason.detail());
}

}  // namespace
}  // namespace play_test
