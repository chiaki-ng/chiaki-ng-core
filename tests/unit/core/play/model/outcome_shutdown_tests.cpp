#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/model/outcome_rig.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>

namespace play_test {
namespace {

class OutcomeShutdown : public OutcomeRig {};

TEST_F(OutcomeShutdown, KeepsItsEndingAcrossEveryShape) {
  const auto& route = causes[4];
  const auto& shutdown = causes[8];
  EXPECT_EQ(route.typed, Cause::route_failed);
  EXPECT_EQ(shutdown.typed, Cause::shutdown_failed);
  for (const auto phase : phases) {
    for (const auto& cause : causes) {
      for (const bool left : {false, true}) {
        for (const int shape : {0, 1, 2, 3, 4, 5}) {
          SCOPED_TRACE(testing::Message()
                       << testing::PrintToString(phase) << ' '
                       << testing::PrintToString(cause.typed)
                       << " left=" << left << " shape=" << shape);
          const bool stop_failed = shape == 0 || shape == 2 || shape == 4;
          const bool cleanup_failed = shape == 1 || shape == 2 || shape == 5;
          const bool primary = shape == 3 || shape == 4 || shape == 5;
          Outcome::Evidence typed;
          typed.phase = phase;
          typed.left = left;
          if (stop_failed) typed.stop_cause = cause.typed;
          if (cleanup_failed) typed.cleanup = end_reason(shutdown, false);
          if (primary) {
            typed.primary = end_reason(route, left);
            typed.phase_cause = route.typed;
          }
          const Ended shut_down{Ending::shutdown_failed,
                                stop_failed ? cause.typed : shutdown.typed};
          if (!primary || left) {
            if (stop_failed || cleanup_failed)
              ends(typed, shut_down);
            else
              ends(typed, {Ending::left, std::nullopt});
          } else if (phase == Phase::start) {
            ends(typed, {Ending::start_failed, Cause::route_failed});
          } else if (phase == Phase::session) {
            ends(typed, {Ending::session_failed, Cause::route_failed});
          } else if (phase != Phase::none) {
            ends(typed, {Ending::session_failed, Cause::pipeline_failed});
          } else if (stop_failed) {
            ends(typed, shut_down);
          } else {
            ends(typed, {Ending::finished, std::nullopt});
          }
        }
      }
    }
  }
}

TEST_F(OutcomeShutdown, DropsTheReasonOfAFailureTheUserLeft) {
  Outcome::Evidence stopped;
  stopped.stop_cause = Cause::shutdown_failed;
  EXPECT_TRUE(ended_as(Outcome{stopped}, Ending::shutdown_failed,
                       Cause::shutdown_failed));
  EXPECT_EQ(Outcome{stopped}.ending(), Ending::shutdown_failed);
  stopped.left = true;
  stopped.primary = EndReason{Cause::route_failed, {.left = true}};
  EXPECT_TRUE(ended_as(Outcome{stopped}, Ending::shutdown_failed,
                       Cause::shutdown_failed));
  EXPECT_EQ(Outcome{stopped}.reason(), nullptr);
}

}  // namespace
}  // namespace play_test
