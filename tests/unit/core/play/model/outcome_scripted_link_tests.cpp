#include "src/core/net/protocol/remote_play/rp_reason.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/model/outcome_rig.hpp"

#include <libchiaking/testing/play/scripted_link.hpp>

#include <gtest/gtest.h>

#include <magic_enum/magic_enum.hpp>

#include <string>
#include <string_view>

namespace play_test {
namespace {

using libchiaking::play::testing::ScriptedLink;

class OutcomeScriptedLink : public OutcomeRig {
 protected:
  static auto clean(const Outcome& outcome, std::string_view trace)
      -> testing::AssertionResult {
    if (!ended_as(outcome, Ending::finished))
      return testing::AssertionFailure() << "did not finish";
    if (outcome.ending() != Ending::finished)
      return testing::AssertionFailure() << "ending is not finished";
    if (!outcome.succeeded())
      return testing::AssertionFailure() << "did not succeed";
    if (outcome.cause()) return testing::AssertionFailure() << "has a cause";
    if (outcome.was_left()) return testing::AssertionFailure() << "was left";
    if (outcome.reason() != nullptr)
      return testing::AssertionFailure() << "has a reason";
    if (outcome.cleanup() != nullptr)
      return testing::AssertionFailure() << "has a cleanup";
    if (outcome.trace() != trace)
      return testing::AssertionFailure()
             << "trace " << outcome.trace() << " is not " << trace;
    return testing::AssertionSuccess();
  }

  static auto name(StreamError error) -> std::string {
    return std::string{magic_enum::enum_name(error)};
  }

  static auto name(QuitReason reason) -> std::string {
    return std::string{magic_enum::enum_name(reason)};
  }
};

TEST_F(OutcomeScriptedLink, FinishesARunThatEndsCleanly) {
  EXPECT_TRUE(
      clean(ended([](ScriptedLink& link) { link.finish(); }), "quit=stopped"));
  EXPECT_TRUE(clean(ended([](ScriptedLink& link) {
                      link.connect();
                      link.finish();
                    }),
                    "quit=stopped"));
  EXPECT_TRUE(clean(ended([](ScriptedLink& link) {
                      link.connect();
                      link.begin_leaving();
                    }),
                    "quit=stopped"));
}

TEST_F(OutcomeScriptedLink, LeavesARunLeftBeforeItStarted) {
  const auto left = ended([](ScriptedLink& link) { link.connect(); }, true);
  EXPECT_TRUE(ended_as(left, Ending::left));
  EXPECT_EQ(left.ending(), Ending::left);
  EXPECT_TRUE(left.was_left());
  EXPECT_FALSE(left.cause());
  EXPECT_EQ(left.reason(), nullptr);
  EXPECT_EQ(left.cleanup(), nullptr);
  EXPECT_EQ(
      left.trace(),
      "route=local stage=unknown result=route_failed HTTP=0 reason=0x00000000 "
      "received=0 "
      "cancelled=1 detail=\"cancelled\" quit=stopped");
}

TEST_F(OutcomeScriptedLink, ReportsAFailedCleanup) {
  const auto cleanup = ended([](ScriptedLink& link) {
    link.connect();
    link.fail_cleanup(
        EndReason{Cause::shutdown_failed, {.step = EndReason::Step::shutdown}});
    link.finish();
  });
  EXPECT_TRUE(
      ended_as(cleanup, Ending::shutdown_failed, Cause::shutdown_failed));
  EXPECT_EQ(cleanup.ending(), Ending::shutdown_failed);
  EXPECT_EQ(cleanup.cause(), Cause::shutdown_failed);
  EXPECT_EQ(cleanup.reason(), nullptr);
  ASSERT_NE(cleanup.cleanup(), nullptr);
  EXPECT_EQ(cleanup.cleanup()->step(), EndReason::Step::shutdown);
  EXPECT_EQ(cleanup.trace(),
            " cleanup: stage=shutdown result=shutdown_failed detail=\"resource "
            "cleanup failed\" quit=stopped");
}

TEST_F(OutcomeScriptedLink, FailsTheStartTheConsoleRefused) {
  for (const auto& cause : causes) {
    SCOPED_TRACE(testing::PrintToString(cause.typed));
    const auto refused = ended([&cause](ScriptedLink& link) {
      link.refuse_start(
          EndReason{cause.typed, {.step = EndReason::Step::session_request}});
    });
    EXPECT_TRUE(ended_as(refused, Ending::start_failed, cause.typed));
    EXPECT_EQ(refused.cause(), cause.typed);
    EXPECT_FALSE(refused.succeeded());
    EXPECT_FALSE(refused.was_left());
    ASSERT_NE(refused.reason(), nullptr);
    EXPECT_EQ(refused.reason()->cause(), cause.typed);
    EXPECT_EQ(refused.reason()->step(), EndReason::Step::session_request);
    EXPECT_EQ(refused.cleanup(), nullptr);
  }
}

TEST_F(OutcomeScriptedLink, FailsTheSessionThatBrokeAfterConnecting) {
  for (const auto& cause : causes) {
    SCOPED_TRACE(testing::PrintToString(cause.typed));
    const auto failed = ended([&cause](ScriptedLink& link) {
      link.connect();
      link.fail(EndReason{
          cause.typed,
          {.step = EndReason::Step::control_receive, .http_status = 403}});
    });
    EXPECT_TRUE(ended_as(failed, Ending::session_failed, cause.typed));
    EXPECT_EQ(failed.cause(), cause.typed);
    ASSERT_NE(failed.reason(), nullptr);
    EXPECT_EQ(failed.reason()->step(), EndReason::Step::control_receive);
    EXPECT_EQ(failed.reason()->http_status(), 403);
    EXPECT_TRUE(failed.trace().starts_with(
        "route=local stage=ctrl_receive result=" + name(cause.today) +
        " HTTP=403 reason=0x00000000 received=0 cancelled=0 detail=\""))
        << failed.trace();
    EXPECT_NE(failed.trace().find("quit="), std::string::npos);
    ASSERT_TRUE(failed.quit());
    EXPECT_TRUE(failed.quit()->is_error());
  }
}

TEST_F(OutcomeScriptedLink, FailsTheSessionThatBrokeBeforeConnecting) {
  for (const auto& cause : causes) {
    SCOPED_TRACE(testing::PrintToString(cause.typed));
    const auto early = ended([&cause](ScriptedLink& link) {
      link.fail(EndReason{cause.typed,
                          {.step = EndReason::Step::account_session_open}});
    });
    EXPECT_TRUE(ended_as(early, Ending::session_failed, cause.typed));
    EXPECT_EQ(early.cause(), cause.typed);
    ASSERT_NE(early.reason(), nullptr);
    EXPECT_EQ(early.reason()->step(), EndReason::Step::account_session_open);
  }
}

TEST_F(OutcomeScriptedLink, FailsWithoutAReasonWhenTheLinkGaveUpOnItsOwn) {
  for (const auto& cause : causes) {
    SCOPED_TRACE(testing::PrintToString(cause.typed));
    const auto abandoned = ended([&cause](ScriptedLink& link) {
      link.connect();
      link.fail(EndReason{cause.typed,
                          {.step = EndReason::Step::media, .left = true}});
    });
    EXPECT_TRUE(ended_as(abandoned, Ending::session_failed, cause.typed));
    EXPECT_EQ(abandoned.cause(), cause.typed);
    EXPECT_EQ(abandoned.reason(), nullptr);
    EXPECT_FALSE(abandoned.was_left());
    EXPECT_NE(abandoned.trace().find(" received=0 cancelled=1"),
              std::string::npos);
  }
}

TEST_F(OutcomeScriptedLink, ReportsWhatTheConsoleSaidWhenItQuit) {
  for (const auto& quit : quits) {
    SCOPED_TRACE(testing::PrintToString(quit.typed));
    const bool stopped = quit.typed == ConsoleQuit::Reason::stopped;
    const auto told = ended([&quit](ScriptedLink& link) {
      link.connect();
      link.quit(ConsoleQuit{quit.typed, "the console said so"});
    });
    EXPECT_EQ(told.ending(), stopped ? Ending::finished : Ending::console_quit);
    EXPECT_FALSE(told.cause());
    ASSERT_TRUE(told.quit());
    EXPECT_EQ(told.quit()->reason(), quit.typed);
    EXPECT_EQ(told.quit()->text(), "the console said so");
    EXPECT_EQ(told.reason(), nullptr);
    EXPECT_EQ(told.cleanup(), nullptr);
    EXPECT_EQ(told.trace(), "quit=" + name(quit.today) +
                                " console_reason=\"the console said so\"");
    const auto silent = ended(
        [&quit](ScriptedLink& link) { link.quit(ConsoleQuit{quit.typed}); });
    EXPECT_EQ(silent.ending(), told.ending());
    ASSERT_TRUE(silent.quit());
    EXPECT_EQ(silent.quit()->reason(), quit.typed);
    EXPECT_TRUE(silent.quit()->text().empty());
    EXPECT_EQ(silent.trace(), "quit=" + name(quit.today));
  }
}

TEST_F(OutcomeScriptedLink, NamesAStartFailureAndAConsoleInUse) {
  const auto started = ended([](ScriptedLink& link) {
    link.refuse_start(EndReason{Cause::control_network_failure,
                                {.step = EndReason::Step::control_startup}});
  });
  EXPECT_TRUE(
      ended_as(started, Ending::start_failed, Cause::control_network_failure));
  const auto in_use = ended([](ScriptedLink& link) {
    link.connect();
    link.quit(ConsoleQuit{ConsoleQuit::Reason::console_in_use});
  });
  EXPECT_TRUE(ended_as(in_use, ConsoleQuit::Reason::console_in_use));
  ASSERT_TRUE(in_use.quit());
  EXPECT_EQ(in_use.quit()->reason(), ConsoleQuit::Reason::console_in_use);
}

TEST_F(OutcomeScriptedLink, FailsTheStartOfAConsoleThatDroppedThePairing) {
  const auto rejected = ended([](ScriptedLink& link) {
    link.refuse_start(
        EndReason{Cause::pairing_rejected,
                  {.step = EndReason::Step::session_request,
                   .console_reason = libchiaking::rp_reason_regist_failed,
                   .http_status = 403}});
  });
  EXPECT_TRUE(
      ended_as(rejected, Ending::start_failed, Cause::pairing_rejected));
  ASSERT_NE(rejected.reason(), nullptr);
  EXPECT_EQ(rejected.reason()->cause(), Cause::pairing_rejected);
  EXPECT_EQ(rejected.reason()->console_reason(),
            libchiaking::rp_reason_regist_failed);
}

}  // namespace
}  // namespace play_test
