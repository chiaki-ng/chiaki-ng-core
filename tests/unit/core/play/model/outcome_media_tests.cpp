#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/model/outcome_rig.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>

namespace play_test {
namespace {

class OutcomeMedia : public OutcomeRig {};

TEST_F(OutcomeMedia, CountsAFailureOnlyInTheMediaPhase) {
  for (const auto kind : media_failures) {
    for (const auto phase : phases) {
      for (const bool left : {false, true}) {
        for (const bool with_cause : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << testing::PrintToString(kind) << ' '
                       << testing::PrintToString(phase) << " left=" << left
                       << " cause=" << with_cause);
          Outcome::Evidence typed;
          typed.media = MediaFault{kind, "stage", 7};
          typed.phase = phase;
          typed.left = left;
          if (with_cause) typed.phase_cause = Cause::pipeline_failed;
          if (phase == Phase::media)
            ends(typed, {Ending::media_failed, std::nullopt});
          else if (phase == Phase::none)
            ends(typed, {Ending::finished, std::nullopt});
          else if (phase == Phase::start && with_cause)
            ends(typed, {Ending::start_failed, Cause::pipeline_failed});
          else if (phase == Phase::session && with_cause)
            ends(typed, {Ending::session_failed, Cause::pipeline_failed});
          else
            ends(typed, {Ending::session_failed, Cause::pipeline_failed});
        }
      }
    }
  }
}

TEST_F(OutcomeMedia, KeepsTheFaultKind) {
  for (const auto kind : media_failures) {
    SCOPED_TRACE(testing::PrintToString(kind));
    Outcome::Evidence typed;
    typed.media = MediaFault{kind, "stage", 7};
    typed.phase = Phase::media;
    const Outcome outcome{typed};
    EXPECT_TRUE(ended_as(outcome, kind));
    ASSERT_TRUE(outcome.media_fault());
    EXPECT_EQ(outcome.media_fault()->kind, kind);
  }
}

TEST_F(OutcomeMedia, FinishesCleanlyOutsideTheMediaPhase) {
  Outcome::Evidence clean;
  clean.media =
      MediaFault{MediaFailure::decoder_output, "Video decoder output", 7};
  EXPECT_TRUE(ended_as(Outcome{clean}, Ending::finished));
  clean.phase = Phase::media;
  const Outcome failed{clean};
  EXPECT_TRUE(ended_as(failed, MediaFailure::decoder_output));
  ASSERT_TRUE(failed.media_fault());
  EXPECT_EQ(failed.media_fault()->cause, 7);
}

}  // namespace
}  // namespace play_test
