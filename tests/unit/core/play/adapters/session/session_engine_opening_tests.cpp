#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_engine_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::RecordedEvents;
using test_support::play::SignedInPlayer;
namespace {

namespace play = libchiaking::play;
using namespace std::chrono_literals;
using Cause = play::EndReason::Cause;

TEST(SessionEngine, RefusesToOpenWithoutALink) {
  libchiaking::platform::StopSource stop;
  libchiaking::testing::ManualClock clock;
  libchiaking::testing::RecordingMedia media;
  RecordedEvents events;
  const auto missing = play::Session::Core::open(
      {.player = SignedInPlayer::fresh(),
       .console = nullptr,
       .where = play::Where::home,
       .service = std::nullopt,
       .link = nullptr,
       .stop = stop,
       .media = media,
       .events = events,
       .clock = clock,
       .threading = LiveProviders::threading(),
       .journal = nullptr,
       .first_controller = libchiaking::controllers::ControllerKind::dualsense,
       .settings = {},
       .share = nullptr});
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error(), Cause::invalid_preferences);
}

TEST(SessionSettings, PacesTheLoopByDefault) {
  const play::SessionSettings settings;
  EXPECT_EQ(settings.idle_wait, 8ms);
  EXPECT_EQ(settings.pump_error_tolerance, 64);
  EXPECT_EQ(settings.statistics_interval, 250ms);
}

TEST(SessionSettings, BoundsItsQueuesByDefault) {
  const play::SessionSettings settings;
  EXPECT_EQ(settings.controller_depth, 32);
  EXPECT_EQ(settings.seat_depth, 8);
  EXPECT_EQ(settings.commands.microphone_depth, 16);
  EXPECT_EQ(settings.commands.rest_flush, 400ms);
}

TEST(SessionSettings, ToleratesAudioFailuresByDefault) {
  const play::SessionSettings settings;
  EXPECT_FALSE(settings.audio_failure_ends_run);
}

TEST(SessionSettings, NamesEveryMediaStageByDefault) {
  const play::SessionSettings settings;
  EXPECT_EQ(settings.stages.profile_validation,
            "Video stream profile validation");
  EXPECT_EQ(settings.stages.frame_validation,
            "Video frame metadata validation");
  EXPECT_EQ(settings.stages.format_setup, "Video decoder format setup");
  EXPECT_EQ(settings.stages.submission, "Video decoder submission");
  EXPECT_EQ(settings.stages.recovery_request, "Video recovery request");
  EXPECT_EQ(settings.stages.decoder_output, "Video decoder output");
  EXPECT_EQ(settings.stages.audio_output, "Audio output");
}

}  // namespace
}  // namespace play_test
