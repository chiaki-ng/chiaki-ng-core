#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/guest_visit_stage.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <variant>

namespace kakusan_test {

using test_support::play::SourceStream;
namespace {

using namespace std::chrono_literals;
using libchiaking::kakusan::EndReason;
using manual::Progress;
using manual::VerificationCode;

class GuestVisitPlay : public GuestVisitTest {
 protected:
  auto connected() {
    return [this] { return stage.count<GuestVisit::Connected>() == 1; };
  }

  Stage stage;
};

TEST_F(GuestVisitPlay, KnowsItsHostButCannotRememberItBeforeVerifying) {
  const auto made = stage.make(stage.invitation());
  ASSERT_TRUE(made);
  const auto& run = made.value();
  EXPECT_EQ(run->host().host_hint,
            Identities::hint_of(TestPlatform::identity(0x10).key()));
  EXPECT_FALSE(run->host_key());
  const auto unverified = run->remember_host("Host");
  ASSERT_FALSE(unverified);
  EXPECT_EQ(unverified.error().kind,
            libchiaking::kakusan::RememberRefusal::Kind::not_verified);
}

TEST_F(GuestVisitPlay, PlaysMediaAndControllersUntilCancelled) {
  auto run = stage.make(stage.invitation()).value();
  std::size_t admitted_from = 0;
  std::chrono::microseconds cancelled_at{};
  stage.pair(*run);
  stage.cue(connected(), [&] {
    admitted_from = stage.waits.size();
    const auto* active = stage.state();
    ASSERT_NE(active, nullptr);
    EXPECT_EQ(active->progress, Progress::playing);
    EXPECT_TRUE(active->grant.has_value());
    EXPECT_EQ(stage.index_of<GuestVisit::Connected>(), stage.events.size() - 1);
    ASSERT_GE(stage.events.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<GuestVisit::VisitChanged>(
        stage.events[stage.events.size() - 2]));
    const auto* ready = stage.find<GuestVisit::StreamReady>();
    ASSERT_NE(ready, nullptr);
    EXPECT_EQ(ready->frames_per_second, SourceStream::video_fps);
    EXPECT_EQ(stage.screen.infos, 1);
    EXPECT_EQ(stage.screen.profiles, SourceStream::description().profiles);
    EXPECT_LT(stage.index_of<GuestVisit::ReplyReady>(),
              stage.index_of<GuestVisit::CodeToReadOut>());

    const auto remembered = run->remember_host(" The Host ");
    ASSERT_TRUE(remembered);
    EXPECT_EQ(remembered.value().nickname, "The Host");
    ASSERT_EQ(stage.store.saved().size(), 1U);
    EXPECT_EQ(stage.store.saved().front().key,
              TestPlatform::identity(0x10).key());

    const auto wakes = stage.wakes;
    EXPECT_TRUE(run->send(pressed()));
    EXPECT_EQ(stage.wakes, wakes + 1);
    stage.forward_source();
    stage.host->source_feedback(rumble(1, 255, 128));
  });
  stage.cue(
      [&] {
        return !stage.screen.feedback.empty() &&
               stage.screen.played().size() ==
                   SourceStream::video_frames.size() &&
               stage.screen.audio.size() ==
                   SourceStream::audio_frames.size() &&
               stage.side.pressed();
      },
      [&] {
        const auto& felt = stage.screen.feedback.front();
        EXPECT_EQ(felt.kind(),
                  libchiaking::controllers::ControllerFeedback::Kind::rumble);
        EXPECT_EQ(felt.seat, libchiaking::controllers::SeatNumber::first());
        const auto* shaken = felt.get<libchiaking::controllers::Rumble>();
        ASSERT_NE(shaken, nullptr);
        EXPECT_EQ(shaken->left, 255U);
        EXPECT_EQ(shaken->right, 128U);
        const auto played = stage.screen.played();
        for (std::size_t index = 0; index < played.size(); ++index)
          EXPECT_EQ(played[index], test_support::Bytes::unhex(
                                       SourceStream::video_frames[index]));
        for (std::size_t index = 0; index < stage.screen.audio.size(); ++index)
          EXPECT_EQ(
              stage.screen.audio[index],
              test_support::Bytes::unhex(SourceStream::audio_frames[index]));
        EXPECT_EQ(stage.screen.headers, 1);
        EXPECT_EQ(stage.screen.gaps, 0);
        EXPECT_EQ(run->video_statistics().units, stage.screen.frames.size());
        EXPECT_TRUE(std::all_of(
            stage.waits.begin() + static_cast<std::ptrdiff_t>(admitted_from),
            stage.waits.end(), [](auto wait) { return wait <= 2ms; }));
        cancelled_at = stage.elapsed();
        run->cancel();
      });

  const auto outcome = run->run();
  EXPECT_EQ(outcome.status(), VisitOutcome::Status::cancelled);
  EXPECT_EQ(outcome.code(), 2);
  EXPECT_FALSE(outcome.succeeded());
  EXPECT_FALSE(outcome.refusal());
  EXPECT_TRUE(outcome.message().empty());
  EXPECT_LT(stage.elapsed() - cancelled_at, 1000ms);
  EXPECT_TRUE(stage.cues.empty());
  const auto* closed = stage.state();
  ASSERT_NE(closed, nullptr);
  EXPECT_EQ(closed->progress, Progress::gone);
  EXPECT_EQ(closed->ended, EndReason::Kind::left);
  EXPECT_EQ(stage.count<GuestVisit::StreamStopped>(), 1U);
  EXPECT_EQ(stage.count<GuestVisit::Connected>(), 1U);
  EXPECT_EQ(stage.count<GuestVisit::Disconnected>(), 1U);
  EXPECT_EQ(stage.index_of<GuestVisit::Disconnected>(),
            stage.events.size() - 1);
  EXPECT_NE(std::ranges::find(stage.waits, 8ms), stage.waits.end());
  EXPECT_TRUE(
      std::ranges::all_of(stage.waits, [](auto wait) { return wait <= 8ms; }));
  EXPECT_GT(stage.logged("Kakusan visit progress"), 0U);
  EXPECT_EQ(run->video_statistics().units, stage.screen.frames.size());

  EXPECT_TRUE(refused(run->verify(*VerificationCode::parse("1234")),
                      Refusal::Kind::not_visiting));
  EXPECT_TRUE(refused(run->send(pressed()), Refusal::Kind::not_visiting));
  EXPECT_TRUE(refused(run->leave(), Refusal::Kind::not_visiting));
  const auto again = run->run();
  EXPECT_EQ(again.status(), VisitOutcome::Status::cancelled);
  EXPECT_EQ(again.code(), 2);
}

TEST_F(GuestVisitPlay, CancelsWithinTheLeaveDeadlineWhenTheHostIsSilent) {
  auto run = stage.make(stage.invitation()).value();
  std::chrono::microseconds cancelled_at{};
  stage.pair(*run);
  stage.cue(connected(), [&] {
    stage.host_answers = false;
    cancelled_at = stage.elapsed();
    run->cancel();
  });
  const auto outcome = run->run();
  EXPECT_EQ(outcome.status(), VisitOutcome::Status::cancelled);
  EXPECT_EQ(outcome.code(), 2);
  EXPECT_LE(stage.elapsed() - cancelled_at, 1500ms);
  EXPECT_EQ(stage.count<GuestVisit::Disconnected>(), 1U);
}

TEST_F(GuestVisitPlay, CompletesALeaveWhenItsDeadlinePasses) {
  std::chrono::microseconds cancelled_at{};
  GuestVisit::Pacing pacing;
  pacing.leave_deadline = 100ms;
  auto brief = stage.make(stage.invitation(), pacing).value();
  stage.pair(*brief);
  stage.cue(connected(), [&] {
    stage.host_answers = false;
    cancelled_at = stage.elapsed();
    EXPECT_TRUE(brief->leave());
    EXPECT_TRUE(brief->leave());
    EXPECT_TRUE(refused(brief->verify(*VerificationCode::parse("1234")),
                        Refusal::Kind::not_visiting));
    EXPECT_TRUE(refused(brief->send(pressed()), Refusal::Kind::not_visiting));
  });
  const auto expired = brief->run();
  EXPECT_EQ(expired.status(), VisitOutcome::Status::completed);
  EXPECT_EQ(expired.code(), 0);
  EXPECT_EQ(stage.elapsed() - cancelled_at, 100ms);
  EXPECT_EQ(stage.count<GuestVisit::Disconnected>(), 0U);
  ASSERT_NE(stage.state(), nullptr);
  EXPECT_NE(stage.state()->progress, Progress::gone);
  EXPECT_TRUE(refused(brief->leave(), Refusal::Kind::not_visiting));
}

TEST_F(GuestVisitPlay, CompletesTheRunWhenTheGuestLeaves) {
  auto run = stage.make(stage.invitation()).value();
  stage.pair(*run);
  stage.cue(connected(), [&] { EXPECT_TRUE(run->leave()); });
  const auto outcome = run->run();
  EXPECT_TRUE(outcome.succeeded());
  EXPECT_EQ(outcome.code(), 0);
  EXPECT_TRUE(outcome.message().empty());
  ASSERT_NE(stage.state(), nullptr);
  EXPECT_EQ(stage.state()->progress, Progress::gone);
  EXPECT_EQ(stage.count<GuestVisit::Disconnected>(), 1U);
  EXPECT_TRUE(refused(run->leave(), Refusal::Kind::not_visiting));
}

}  // namespace
}  // namespace kakusan_test
