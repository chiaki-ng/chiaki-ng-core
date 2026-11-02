#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/model/outcome_rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace play_test {
namespace {

using libchiaking::play::LaunchRefused;
using libchiaking::play::RestOutcome;

class OutcomeEndReason : public OutcomeRig {
 protected:
  static auto refused() -> EndReason {
    return EndReason{Cause::route_failed,
                     {.step = EndReason::Step::cloud_allocate,
                      .console_reason = 0x80108b12,
                      .http_status = 403,
                      .action_url = std::string(EndReason::url_limit + 40, 'u'),
                      .detail = std::string(EndReason::detail_limit + 40, 'd'),
                      .launch_refused = LaunchRefused::no_server(
                          LaunchRefused::NoServer::queue_timed_out)}};
  }
};

TEST_F(OutcomeEndReason, CarriesOnlyItsCauseByDefault) {
  const EndReason plain{Cause::session_request_in_use};
  EXPECT_EQ(plain.cause(), Cause::session_request_in_use);
  EXPECT_EQ(plain.step(), EndReason::Step::unknown);
  EXPECT_EQ(plain.console_reason(), 0u);
  EXPECT_EQ(plain.http_status(), 0);
  EXPECT_FALSE(plain.was_left());
  EXPECT_TRUE(plain.action_url().empty());
  EXPECT_TRUE(plain.detail().empty());
  EXPECT_FALSE(plain.launch_refused());
}

TEST_F(OutcomeEndReason, CarriesTypedFactsWithinTheirLimits) {
  const auto reason = refused();
  EXPECT_EQ(reason.step(), EndReason::Step::cloud_allocate);
  EXPECT_EQ(reason.console_reason(), 0x80108b12);
  EXPECT_EQ(reason.http_status(), 403);
  EXPECT_EQ(reason.action_url().size(), EndReason::url_limit);
  EXPECT_EQ(reason.detail().size(), EndReason::detail_limit);
  ASSERT_TRUE(reason.launch_refused());
  EXPECT_EQ(reason.launch_refused()->kind(), LaunchRefused::Kind::no_server);
}

TEST_F(OutcomeEndReason, ReachesTheOutcomeWithTheRestAndTrace) {
  Outcome::Evidence evidence;
  evidence.phase = Phase::start;
  evidence.phase_cause = Cause::route_failed;
  evidence.primary = refused();
  evidence.rest = RestOutcome::refused;
  evidence.trace = "stage=31";
  const Outcome outcome{evidence};
  EXPECT_TRUE(ended_as(outcome, Ending::start_failed, Cause::route_failed));
  ASSERT_NE(outcome.launch_refused(), nullptr);
  EXPECT_EQ(outcome.launch_refused()->no_server(),
            LaunchRefused::NoServer::queue_timed_out);
  EXPECT_EQ(outcome.rest(), RestOutcome::refused);
  EXPECT_EQ(outcome.trace(), "stage=31");
  EXPECT_EQ(Outcome{Outcome::Evidence{}}.launch_refused(), nullptr);
}

TEST_F(OutcomeEndReason, LimitsAConsoleQuitTextAndNamesItsErrors) {
  const ConsoleQuit quit{ConsoleQuit::Reason::console_in_use,
                         std::string(ConsoleQuit::text_limit + 9, 't')};
  EXPECT_EQ(quit.text().size(), ConsoleQuit::text_limit);
  EXPECT_EQ(quit.reason(), ConsoleQuit::Reason::console_in_use);
  EXPECT_TRUE(quit.is_error());
  EXPECT_FALSE(ConsoleQuit{ConsoleQuit::Reason::stopped}.is_error());
  EXPECT_FALSE(ConsoleQuit{ConsoleQuit::Reason::console_shut_down}.is_error());
  EXPECT_FALSE(ConsoleQuit{ConsoleQuit::Reason::time_limit_reached}.is_error());
  EXPECT_FALSE(ConsoleQuit{ConsoleQuit::Reason::trial_ended}.is_error());
  EXPECT_EQ(quit, (ConsoleQuit{ConsoleQuit::Reason::console_in_use,
                               std::string(ConsoleQuit::text_limit, 't')}));
}

}  // namespace
}  // namespace play_test
