#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/console_commands_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using namespace std::chrono_literals;
using api_test::bytes_of;
using play::InputRefusal;

TEST(ConsoleAudio3d, IsRequestedWhenTheStreamIsReadyAndOnALiveToggle) {
  ConsoleRig console{ConsoleRig::cloud()};
  EXPECT_TRUE(console.commands().set_audio_3d(true));
  EXPECT_TRUE(console.commands().audio_3d());
  EXPECT_TRUE(console.commands().pending(true));
  console.streaming();
  EXPECT_EQ(console.session().count("audio_state"), 0U);

  console.commands().observe(remote_play::event::StreamInfoReady{});
  EXPECT_EQ(console.session().count("audio_state"), 1U);
  EXPECT_EQ(console.session().audio_state_kind,
            remote_play::AudioStateKind::flags);
  EXPECT_EQ(console.session().audio_state,
            bytes_of({3, 0xff, 0, 0, 0, 1, 0, 0, 0}));
  EXPECT_EQ(console.logged("audio 3d requested (stream ready): result=ok"), 1U);

  console.commands().audio_profile_changed();
  EXPECT_EQ(console.session().count("audio_state"), 2U);

  EXPECT_TRUE(console.commands().set_audio_3d(true));
  EXPECT_FALSE(console.commands().pending(true));
  EXPECT_TRUE(console.commands().set_audio_3d(false));
  console.negotiating();
  EXPECT_EQ(console.session().count("audio_state"), 2U);
  EXPECT_FALSE(console.commands().pending(true));
  console.streaming();
  EXPECT_EQ(console.session().count("audio_state"), 2U);
  console.clock().advance(2s);
  EXPECT_TRUE(console.commands().pending(true));
  console.streaming();
  EXPECT_EQ(console.session().count("audio_state"), 3U);
  EXPECT_EQ(console.session().audio_state,
            bytes_of({3, 0xff, 0, 0, 0, 0, 0, 0, 0}));
  console.commands().audio_profile_changed();
  EXPECT_EQ(console.session().count("audio_state"), 3U);
}

TEST(ConsoleAudio3d, IsOnlyRememberedBeforeTheConsoleRuns) {
  ConsoleRig idle{ConsoleRig::cloud(), false};
  EXPECT_TRUE(idle.commands().set_audio_3d(true));
  EXPECT_TRUE(idle.commands().audio_3d());
  EXPECT_FALSE(idle.commands().pending(true));
}

TEST(ConsoleAudio3d, IsSentOnceWhenSetBeforeTheStreamIsReady) {
  ConsoleRig early{ConsoleRig::cloud()};
  EXPECT_TRUE(early.commands().set_audio_3d(true));
  early.streaming();
  early.commands().observe(remote_play::event::StreamInfoReady{});
  EXPECT_EQ(early.session().count("audio_state"), 1U);
  EXPECT_FALSE(early.commands().pending(true));
  early.clock().advance(3s);
  early.streaming();
  EXPECT_EQ(early.session().count("audio_state"), 1U);
}

TEST(ConsoleAudio3d, SelectsTheConsoleProfileAtHomeAndOverTheInternet) {
  for (const auto route :
       {ConsoleRig::Venue::home(), ConsoleRig::Venue::internet()}) {
    SCOPED_TRACE(testing::PrintToString(route.where()));
    ConsoleRig console{route};
    EXPECT_TRUE(console.commands().set_audio_3d(true));
    console.streaming();
    console.commands().observe(remote_play::event::StreamInfoReady{});
    EXPECT_EQ(console.session().count("audio_state"), 1U);
    EXPECT_EQ(console.session().audio_state_kind,
              remote_play::AudioStateKind::channel_count);
    EXPECT_EQ(console.session().audio_state, bytes_of({4}));
    EXPECT_EQ(console.logged("audio 3d profile 4 (stream ready): result=ok"),
              1U);

    EXPECT_TRUE(console.commands().set_audio_3d(false));
    console.negotiating();
    console.clock().advance(2s);
    console.streaming();
    EXPECT_EQ(console.session().count("audio_state"), 2U);
    EXPECT_EQ(console.session().audio_state, bytes_of({0}));
    EXPECT_EQ(console.logged("audio 3d profile 0 (live toggle): result=ok"),
              1U);
  }
}

TEST(ConsoleAudio3d, SendsOnlyTheLatestOfRapidTogglesTwoSecondsApart) {
  ConsoleRig console{ConsoleRig::cloud()};
  EXPECT_TRUE(console.commands().set_audio_3d(true));
  console.streaming();
  console.commands().observe(remote_play::event::StreamInfoReady{});
  EXPECT_EQ(console.session().count("audio_state"), 1U);
  for (const bool enabled : {false, true, false, true, false}) {
    SCOPED_TRACE(enabled);
    EXPECT_TRUE(console.commands().set_audio_3d(enabled));
    console.clock().advance(300ms);
    console.streaming();
  }
  EXPECT_EQ(console.session().count("audio_state"), 1U);
  const auto held =
      console.commands().service(remote_play::StreamState::streaming, 1s);
  EXPECT_GT(held.wait, 0ms);
  EXPECT_LE(held.wait, 500ms);
  console.clock().advance(500ms);
  console.streaming();
  EXPECT_EQ(console.session().count("audio_state"), 2U);
  EXPECT_EQ(console.session().audio_state,
            bytes_of({3, 0xff, 0, 0, 0, 0, 0, 0, 0}));
  EXPECT_FALSE(console.commands().pending(true));
  EXPECT_TRUE(console.commands().set_audio_3d(true));
  console.clock().advance(1s);
  console.streaming();
  EXPECT_EQ(console.session().count("audio_state"), 2U);
  console.clock().advance(1s);
  console.streaming();
  EXPECT_EQ(console.session().count("audio_state"), 3U);
  EXPECT_EQ(console.session().audio_state,
            bytes_of({3, 0xff, 0, 0, 0, 1, 0, 0, 0}));
}

TEST(ConsoleAudio3d, IsRefusedOnTheOldService) {
  ConsoleRig console{ConsoleRig::cloud(ConsoleRig::Generation::old_service)};
  EXPECT_TRUE(ConsoleRig::refused(console.commands().set_audio_3d(true),
                                  InputRefusal::wrong_route));
  EXPECT_FALSE(console.commands().audio_3d());
  EXPECT_FALSE(console.commands().pending(true));
  console.streaming();
  console.commands().observe(remote_play::event::StreamInfoReady{});
  EXPECT_EQ(console.session().count("audio_state"), 0U);
}

auto speaker_settings() -> play::CommandSettings {
  play::CommandSettings settings;
  settings.controller_speaker = true;
  return settings;
}

TEST(ConsoleAudioRoutes, ReportTheControllerSpeakerOnPs5CloudForThirtySeconds) {
  ConsoleRig console{ConsoleRig::cloud(), true, speaker_settings()};
  console.streaming();
  EXPECT_EQ(console.session().count("audio_route"), 0U);
  EXPECT_FALSE(console.commands().pending(true));

  console.commands().observe(remote_play::event::StreamInfoReady{});
  EXPECT_EQ(console.session().count("audio_state"), 1U);
  EXPECT_EQ(console.session().audio_state_kind,
            remote_play::AudioStateKind::full);
  EXPECT_EQ(console.session().audio_state.size(), 73U);
  EXPECT_EQ(console.session().count("audio_route"), 2U);
  EXPECT_EQ(console.session().audio_route,
            bytes_of({1, 1, 0, 0, 0, 0, 0, 0, 0, 127, 0, 0, 0, 0, 0, 0, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(console.logged("audio layout (stream ready): result=ok"), 1U);
  EXPECT_EQ(
      console.logged("controller speaker route (stream ready): result=ok"), 1U);
  EXPECT_EQ(console.logged("main output route (stream ready): result=ok"), 1U);

  console.clock().advance(500ms);
  console.streaming();
  EXPECT_EQ(console.session().count("audio_route"), 2U);
  const auto held =
      console.commands().service(remote_play::StreamState::streaming, 1s);
  EXPECT_GT(held.wait, 0ms);
  EXPECT_LE(held.wait, 500ms);
  console.clock().advance(500ms);
  EXPECT_TRUE(console.commands().pending(true));
  console.streaming();
  EXPECT_EQ(console.session().count("audio_route"), 4U);
  for (int second = 2; second <= 30; ++second) {
    console.clock().advance(1s);
    console.streaming();
  }
  EXPECT_EQ(console.session().count("audio_route"), 62U);
  console.clock().advance(5s);
  EXPECT_FALSE(console.commands().pending(true));
  console.streaming();
  EXPECT_EQ(console.session().count("audio_route"), 62U);
  EXPECT_EQ(console.session().count("audio_state"), 1U);
  EXPECT_EQ(
      console.logged("controller speaker route (stream ready): result=ok"), 1U);
}

TEST(ConsoleAudioRoutes, ReportOnlyTheMainOutputWithoutTheControllerSpeaker) {
  ConsoleRig console{ConsoleRig::cloud()};
  console.streaming();
  console.commands().observe(remote_play::event::StreamInfoReady{});
  EXPECT_EQ(console.session().count("audio_route"), 1U);
  EXPECT_EQ(console.session().count("audio_state"), 0U);
  ASSERT_EQ(console.session().audio_route.size(), 33U);
  EXPECT_EQ(console.session().audio_route[0], std::byte{1});
  EXPECT_EQ(console.logged("main output route (stream ready): result=ok"), 1U);
  EXPECT_EQ(console.logged("audio layout (stream ready): result=ok"), 0U);
  for (int second = 1; second <= 30; ++second) {
    console.clock().advance(1s);
    console.streaming();
  }
  EXPECT_EQ(console.session().count("audio_route"), 31U);
  EXPECT_FALSE(console.commands().pending(true));
}

TEST(ConsoleAudioRoutes, AreNotReportedOutsideThePs5Cloud) {
  for (const auto route :
       {ConsoleRig::Venue::home(), ConsoleRig::Venue::internet(),
        ConsoleRig::cloud(ConsoleRig::Generation::old_service)}) {
    SCOPED_TRACE(testing::PrintToString(route.where()));
    ConsoleRig console{route, true, speaker_settings()};
    console.streaming();
    console.commands().observe(remote_play::event::StreamInfoReady{});
    console.clock().advance(2s);
    console.streaming();
    EXPECT_EQ(console.session().count("audio_state"), 0U);
    EXPECT_EQ(console.session().count("audio_route"), 0U);
    EXPECT_FALSE(console.commands().pending(true));
  }
}

TEST(ConsoleAudio3d, IsRefusedOnTheOldServiceBeforeItRuns) {
  ConsoleRig idle{ConsoleRig::cloud(ConsoleRig::Generation::old_service),
                  false};
  EXPECT_TRUE(ConsoleRig::refused(idle.commands().set_audio_3d(true),
                                  InputRefusal::wrong_route));
}

}  // namespace
}  // namespace play_test
