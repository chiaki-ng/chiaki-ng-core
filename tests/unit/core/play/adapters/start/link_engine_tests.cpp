#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/play/adapters/session/session_core.hpp"
#include "src/core/play/adapters/start/link_preferences.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace play_test {
namespace {

using namespace std::chrono_literals;
using libchiaking::catalog::ServiceGeneration;
using libchiaking::consoles::Generation;
namespace play = libchiaking::play;
namespace takion = libchiaking::net::protocol::takion;
using play::LinkPreferences;

TEST(LinkEngine, RunsOnTheCoreDefaults) {
  const auto core = play::Session::Core::core_settings();
  EXPECT_EQ(core.controller_depth, 64u);
  EXPECT_EQ(core.controller_depth, play::Controllers::default_depth);
  EXPECT_EQ(core.seat_depth, 16u);
  EXPECT_EQ(core.seat_depth, play::Seats::default_depth);
  EXPECT_EQ(core.commands.microphone_depth, play::Microphone::default_depth);
  EXPECT_EQ(core.idle_wait, 8ms);
  EXPECT_EQ(core.pump_error_tolerance, 64u);
  EXPECT_FALSE(core.video.dimension);
  EXPECT_FALSE(core.audio_failure_ends_run);
}

TEST(LinkEngine, TakesWhatTheOutputCanTake) {
  const auto core = play::Session::Core::core_settings();
  const auto plain = LinkPreferences::engine({}, core);
  EXPECT_EQ(plain.controller_depth, 64u);
  EXPECT_FALSE(plain.video.dimension);
  EXPECT_FALSE(plain.audio_failure_ends_run);
  const auto limited =
      LinkPreferences::engine(*play::OutputRequest::create(4096, true), core);
  EXPECT_EQ(limited.video.dimension, 4096U);
  EXPECT_TRUE(limited.audio_failure_ends_run);
  EXPECT_EQ(limited.controller_depth, 64u);
  EXPECT_EQ(limited.seat_depth, 16u);
  EXPECT_EQ(limited.idle_wait, 8ms);
}

TEST(LinkEngine, KeepsWhatTheCoreSettingsAlreadyLimit) {
  auto seam = play::Session::Core::core_settings();
  seam.video.dimension = 1024;
  seam.audio_failure_ends_run = true;
  const auto kept = LinkPreferences::engine({}, seam);
  EXPECT_EQ(kept.video.dimension, 1024U);
  EXPECT_TRUE(kept.audio_failure_ends_run);
}

TEST(LinkEngine, NamesTheTakionVersionsTheBuildChose) {
  using Target = takion::BuildTarget;
  EXPECT_EQ(play::DeveloperTuning::built_protocol_version(Generation::ps4),
            takion::takion_build_version(Target::ps4));
  EXPECT_EQ(play::DeveloperTuning::built_protocol_version(Generation::ps5),
            takion::takion_build_version(Target::ps5));
  EXPECT_EQ(play::DeveloperTuning::built_protocol_version(
                ServiceGeneration::old_service),
            takion::takion_build_version(Target::legacy_cloud_stream));
  EXPECT_EQ(play::DeveloperTuning::built_protocol_version(
                ServiceGeneration::ps5_cloud),
            takion::takion_build_version(Target::cloud_stream));
  EXPECT_EQ(play::DeveloperTuning::build_protocol_override().value_or(0),
            takion::takion_build_version_override);
}

}  // namespace
}  // namespace play_test
