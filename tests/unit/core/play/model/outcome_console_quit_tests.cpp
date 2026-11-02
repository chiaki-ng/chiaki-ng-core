#include "src/core/net/protocol/remote_play/rp_reason.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/model/outcome_rig.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;

class OutcomeConsoleQuit : public OutcomeRig {};

TEST_F(OutcomeConsoleQuit, CountsOnlyWhenNothingElseEndedTheSession) {
  for (const auto& quit : quits) {
    for (const bool left : {false, true}) {
      for (const auto phase : phases) {
        SCOPED_TRACE(testing::Message()
                     << testing::PrintToString(quit.typed) << ' '
                     << testing::PrintToString(phase) << " left=" << left);
        Outcome::Evidence typed;
        typed.quit = ConsoleQuit{quit.typed, "console said so"};
        typed.left = left;
        typed.phase = phase;
        if (left)
          ends(typed, {Ending::left, std::nullopt});
        else if (phase != Phase::none)
          ends(typed, {Ending::session_failed, Cause::pipeline_failed});
        else if (quit.typed == ConsoleQuit::Reason::stopped)
          ends(typed, {Ending::finished, std::nullopt});
        else
          ends(typed, {Ending::console_quit, std::nullopt});
      }
    }
  }
}

TEST_F(OutcomeConsoleQuit, FinishesWhenTheConsoleStoppedAndFailsOtherwise) {
  for (const auto& quit : quits) {
    SCOPED_TRACE(testing::PrintToString(quit.typed));
    Outcome::Evidence typed;
    typed.quit = ConsoleQuit{quit.typed};
    const Outcome outcome{typed};
    if (quit.typed == ConsoleQuit::Reason::stopped) {
      EXPECT_TRUE(ended_as(outcome, Ending::finished));
      EXPECT_TRUE(outcome.succeeded());
    } else {
      EXPECT_TRUE(ended_as(outcome, quit.typed));
      EXPECT_FALSE(outcome.succeeded());
    }
    EXPECT_EQ(ConsoleQuit{quit.typed}.is_error(),
              remote_play::quit_reason_is_error(quit.today));
  }
  Outcome::Evidence shut_down;
  shut_down.quit = ConsoleQuit{ConsoleQuit::Reason::console_shut_down};
  EXPECT_TRUE(
      ended_as(Outcome{shut_down}, ConsoleQuit::Reason::console_shut_down));
}

TEST_F(OutcomeConsoleQuit, FinishesWithoutEvidenceAndLeavesWhenLeft) {
  ends(Outcome::Evidence{}, {Ending::finished, std::nullopt});
  EXPECT_TRUE(ended_as(Outcome{Outcome::Evidence{}}, Ending::finished));
  EXPECT_TRUE(Outcome{Outcome::Evidence{}}.succeeded());
  Outcome::Evidence left;
  left.left = true;
  EXPECT_TRUE(ended_as(Outcome{left}, Ending::left));
  EXPECT_EQ(Outcome{left}.ending(), Ending::left);
}

}  // namespace
}  // namespace play_test
