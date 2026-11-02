#include "src/core/net/protocol/remote_play/rp_reason.hpp"
#include "src/core/play/adapters/links/link_evidence.hpp"
#include "src/core/play/adapters/links/link_failure.hpp"
#include "src/core/play/adapters/links/link_stages.hpp"
#include "src/core/play/adapters/session/command_route.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/model/outcome_rig.hpp"

#include <gtest/gtest.h>

#include <magic_enum/magic_enum.hpp>

#include <initializer_list>
#include <optional>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::catalog::ServiceGeneration;
using play::LinkEvidence;
using play::RestOutcome;
using Stage = remote_play::FailureStage;

class OutcomeVocabulary : public OutcomeRig {
 protected:
  static auto asked() -> EndReason {
    return EndReason{Cause::session_request_refused,
                     {.step = EndReason::Step::cloud_authorise,
                      .console_reason = 0x80108b12,
                      .http_status = 403,
                      .left = true,
                      .action_url = "https://example.invalid/settings",
                      .detail = "refused"}};
  }
};

TEST_F(OutcomeVocabulary, NamesTheResultOfEveryCause) {
  EXPECT_EQ(causes.size(), 28u);
  for (const auto& cause : causes) {
    SCOPED_TRACE(testing::PrintToString(cause.typed));
    EXPECT_EQ(play::CommandRoute::result_name(cause.today),
              magic_enum::enum_name(cause.today));
  }
  EXPECT_TRUE(
      play::CommandRoute::result_name(static_cast<StreamError>(200)).empty());
}

TEST_F(OutcomeVocabulary, MapsEveryCauseOneToOne) {
  for (const auto& cause : causes) {
    SCOPED_TRACE(testing::PrintToString(cause.typed));
    EXPECT_EQ(LinkEvidence::cause(cause.today), cause.typed);
    EXPECT_EQ(LinkEvidence::error(cause.typed), cause.today);
  }
}

TEST_F(OutcomeVocabulary, MapsEveryQuitOneToOne) {
  for (const auto& quit : quits) {
    SCOPED_TRACE(testing::PrintToString(quit.typed));
    const auto typed = LinkEvidence::quit({quit.today, "text"});
    ASSERT_TRUE(typed);
    EXPECT_EQ(typed->reason(), quit.typed);
    EXPECT_EQ(typed->text(), "text");
    EXPECT_EQ(LinkEvidence::quit_reason(quit.typed), quit.today);
  }
  EXPECT_FALSE(LinkEvidence::quit({}));
}

TEST_F(OutcomeVocabulary, MapsEveryStageOneToOne) {
  for (unsigned value = 0;
       value <= static_cast<unsigned>(Stage::cloud_launch_check); ++value) {
    SCOPED_TRACE(value);
    const auto stage = static_cast<Stage>(value);
    EXPECT_EQ(static_cast<unsigned>(play::LinkStages::step(stage)), value);
    EXPECT_EQ(play::LinkStages::stage(play::LinkStages::step(stage)), stage);
  }
}

TEST_F(OutcomeVocabulary, MapsTheRestAndTheVenue) {
  EXPECT_EQ(LinkEvidence::rest(play::RestRequest::Outcome::sent),
            RestOutcome::sent);
  EXPECT_EQ(LinkEvidence::rest(play::RestRequest::Outcome::refused),
            RestOutcome::refused);
  EXPECT_EQ(LinkEvidence::rest(play::RestRequest::Outcome::none),
            RestOutcome::none);
  EXPECT_EQ(play::Venue::of(play::Where::home, std::nullopt),
            play::Venue::home());
  EXPECT_EQ(play::Venue::of(play::Where::internet, std::nullopt),
            play::Venue::internet());
  EXPECT_EQ(play::Venue::of(play::Where::cloud, std::nullopt),
            play::Venue::cloud(ServiceGeneration::ps5_cloud));
  EXPECT_EQ(play::Venue::of(play::Where::cloud, ServiceGeneration::old_service),
            play::Venue::cloud(ServiceGeneration::old_service));
}

TEST_F(OutcomeVocabulary, ReportsAnEndReasonAsALinkFailure) {
  const auto failure = play::LinkFailure::reported(asked());
  EXPECT_EQ(failure.stage(), Stage::cloud_authorize);
  EXPECT_EQ(failure.result(), StreamError::session_init_refused);
  EXPECT_TRUE(failure.cancelled());
  EXPECT_EQ(failure.console_reason(), 0x80108b12);
  EXPECT_EQ(failure.http_status(), 403);
}

TEST_F(OutcomeVocabulary, ReadsALinkFailureBackAsTheSameEndReason) {
  const auto reason = play::LinkFailure::reported(asked()).end_reason();
  EXPECT_EQ(reason.cause(), Cause::session_request_refused);
  EXPECT_EQ(reason.step(), EndReason::Step::cloud_authorise);
  EXPECT_EQ(reason.console_reason(), 0x80108b12);
  EXPECT_EQ(reason.http_status(), 403);
  EXPECT_TRUE(reason.was_left());
  EXPECT_EQ(reason.action_url(), "https://example.invalid/settings");
  EXPECT_EQ(reason.detail(), "refused");
}

TEST_F(OutcomeVocabulary, SaysAConsoleNoLongerAcceptsThePairing) {
  using Refusal = play::LinkFailure::ConsoleRefusal;
  for (const auto code : {libchiaking::rp_reason_regist_failed,
                          libchiaking::rp_reason_invalid_psn_id}) {
    SCOPED_TRACE(code);
    const play::LinkFailure refused{Stage::session_init,
                                    StreamError::session_init_refused,
                                    Refusal{code, 403}};
    const auto reason = refused.end_reason();
    EXPECT_EQ(reason.cause(), Cause::pairing_rejected);
    EXPECT_EQ(reason.console_reason(), code);
  }
  const play::LinkFailure crashed{Stage::session_init,
                                  StreamError::session_init_refused,
                                  Refusal{libchiaking::rp_reason_crash, 403}};
  EXPECT_EQ(crashed.end_reason().cause(), Cause::session_request_refused);
  const play::LinkFailure timed_out{
      Stage::session_init, StreamError::session_init_timeout,
      Refusal{libchiaking::rp_reason_regist_failed, 0}};
  EXPECT_EQ(timed_out.end_reason().cause(), Cause::session_request_timeout);
  EXPECT_EQ(LinkEvidence::error(Cause::pairing_rejected),
            StreamError::session_init_refused);
}

}  // namespace
}  // namespace play_test
