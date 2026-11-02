#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/guest_visit_stage.hpp"

#include <gtest/gtest.h>

namespace kakusan_test {
namespace {

using manual::Progress;

class GuestVisitFailure : public GuestVisitTest {
 protected:
  auto connected() {
    return [this] { return stage.count<GuestVisit::Connected>() == 1; };
  }

  Stage stage;
};

TEST_F(GuestVisitFailure, RefusesAnUnreadableInvitationAsText) {
  const auto unreadable = stage.make("garbage");
  ASSERT_TRUE(refused(unreadable, Refusal::Kind::invalid_text));
  EXPECT_EQ(VisitOutcome::refusal_base + unreadable.error().code(), 5003);
  EXPECT_EQ(unreadable.error().name(), "INVALID_TEXT");
  EXPECT_TRUE(stage.events.empty());
  EXPECT_TRUE(stage.waits.empty());
}

TEST_F(GuestVisitFailure, EndsAVisitCancelledBeforeItRunsWithoutEvents) {
  auto stopped = stage.make(stage.invitation()).value();
  stopped->cancel();
  const auto never = stopped->run();
  EXPECT_EQ(never.status(), VisitOutcome::Status::cancelled);
  EXPECT_TRUE(stage.events.empty());
  EXPECT_TRUE(refused(stopped->leave(), Refusal::Kind::not_visiting));
  EXPECT_EQ(stopped->run().code(), 2);
}

TEST_F(GuestVisitFailure, EndsTheVisitWhenTheCodeIsWrong) {
  auto mistaken = stage.make(stage.invitation()).value();
  stage.pair(*mistaken, false);
  const auto rejected = mistaken->run();
  EXPECT_EQ(rejected.status(), VisitOutcome::Status::visit_failed);
  EXPECT_EQ(rejected.refusal(), Refusal::Kind::verification_failed);
  EXPECT_EQ(rejected.code(), 5008);
  EXPECT_EQ(rejected.message(),
            "The shared session ended: VERIFICATION_FAILED.");
  EXPECT_EQ(stage.count<GuestVisit::Connected>(), 0U);
  EXPECT_EQ(stage.count<GuestVisit::Disconnected>(), 1U);
  ASSERT_NE(stage.state(), nullptr);
  EXPECT_EQ(stage.state()->progress, Progress::gone);
  EXPECT_EQ(stage.state()->refusal, Refusal::Kind::verification_failed);
}

TEST_F(GuestVisitFailure, EndsTheRunWhenTheDecoderRejectsVideo) {
  auto run = stage.make(stage.invitation()).value();
  stage.screen.verdict = {libchiaking::media::VideoVerdict::submit_failed, -12};
  stage.pair(*run);
  stage.cue(connected(), [&] { stage.forward_source(); });
  const auto outcome = run->run();
  EXPECT_EQ(outcome.status(), VisitOutcome::Status::media_failed);
  EXPECT_EQ(outcome.code(), 4803);
  EXPECT_FALSE(outcome.refusal());
  ASSERT_TRUE(outcome.media_fault());
  EXPECT_EQ(outcome.media_fault()->kind,
            libchiaking::media::MediaFailure::decoder_submission);
  EXPECT_EQ(outcome.message(), "Video decoder submission failed (code -12).");
  EXPECT_EQ(stage.logged("media failure: kind=decoder_submission cause=-12"),
            1U);
  ASSERT_NE(stage.state(), nullptr);
  EXPECT_EQ(stage.state()->progress, Progress::gone);
}

TEST_F(GuestVisitFailure, EndsTheRunOnTheFirstVideoOutputFailure) {
  auto broken = stage.make(stage.invitation()).value();
  stage.pair(*broken);
  stage.cue(connected(), [&] {
    const auto wakes = stage.wakes;
    broken->video_output_failed(7);
    broken->video_output_failed(8);
    EXPECT_EQ(stage.wakes, wakes + 2);
  });
  const auto failed_output = broken->run();
  EXPECT_EQ(failed_output.code(), 4805);
  EXPECT_EQ(failed_output.message(), "Video decoder output failed (code 7).");
}

}  // namespace
}  // namespace kakusan_test
