#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/media/feedback_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace play_test {

using test_support::LiveProviders;
namespace {

namespace controllers = libchiaking::controllers;
using controllers::FeedbackIntensity;

class FeedbackInboxGain : public FeedbackRig {};

TEST_F(FeedbackInboxGain, DefaultsToTheDocumentedLimits) {
  EXPECT_EQ(Inbox::Configuration{}.gain_ceiling, 2.4F);
  EXPECT_EQ(Inbox::Configuration{}.stale_after, std::chrono::milliseconds{40});
  EXPECT_EQ(Inbox::Configuration{}.buffered_blocks, 2);
}

TEST_F(FeedbackInboxGain, StartsAtUnityWithStrongHaptics) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(inbox.gain(), 1.0F);
  EXPECT_EQ(inbox.haptic_intensity(), FeedbackIntensity::strong);
}

TEST_F(FeedbackInboxGain, ClampsTheGainToTheCeilingAndZero) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  inbox.set_gain(9.0F);
  EXPECT_EQ(inbox.gain(), 2.4F);
  inbox.set_gain(-1.0F);
  EXPECT_EQ(inbox.gain(), 0.0F);
  inbox.set_gain(std::numeric_limits<float>::quiet_NaN());
  EXPECT_EQ(inbox.gain(), 0.0F);
  inbox.set_gain(2.0F);
  EXPECT_EQ(inbox.haptic_gain(), 2.0F);
}

TEST_F(FeedbackInboxGain, ScalesHapticGainByTheConsoleLevel) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  inbox.set_gain(2.0F);
  const std::array<float, 4> scale{0.0F, 1.0F, 0.5F, 0.33F};
  for (std::uint8_t level = 0; level < 4; ++level) {
    SCOPED_TRACE(+level);
    const auto typed = static_cast<FeedbackIntensity>(level);
    EXPECT_EQ(
        inbox.accept({seat(0), controllers::HapticIntensity{typed}}, start),
        Admission::held);
    EXPECT_EQ(inbox.haptic_intensity(), typed);
    EXPECT_EQ(inbox.haptic_gain(), 2.0F * scale[level]);
  }
}

TEST_F(FeedbackInboxGain, IgnoresAnIntensityTheConsoleNeverSends) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  inbox.set_gain(2.0F);
  const auto unknown = static_cast<FeedbackIntensity>(255);
  static_cast<void>(
      inbox.accept({seat(0), controllers::HapticIntensity{unknown}}, start));
  EXPECT_EQ(inbox.haptic_intensity(), FeedbackIntensity::strong);
  EXPECT_EQ(inbox.haptic_gain(), 2.0F);
}

TEST_F(FeedbackInboxGain, NumbersTheIntensitiesAsTheConsoleDoes) {
  EXPECT_EQ(static_cast<int>(FeedbackIntensity::off), 0);
  EXPECT_EQ(static_cast<int>(FeedbackIntensity::strong), 1);
  EXPECT_EQ(static_cast<int>(FeedbackIntensity::medium), 2);
  EXPECT_EQ(static_cast<int>(FeedbackIntensity::weak), 3);
}

TEST_F(FeedbackInboxGain, FollowsAConfiguredCeiling) {
  Inbox loud{LiveProviders::threading(), LiveProviders::steady(),
             Inbox::Configuration{.gain_ceiling = 4.0F}};
  loud.set_gain(9.0F);
  EXPECT_EQ(loud.gain(), 4.0F);
  Inbox quiet{LiveProviders::threading(), LiveProviders::steady(),
              Inbox::Configuration{.gain_ceiling = 0.5F}};
  EXPECT_EQ(quiet.gain(), 0.5F);
}

TEST_F(FeedbackInboxGain, LetsASeatGainReplaceTheGlobalGain) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  inbox.set_gain(2.0F);
  EXPECT_EQ(inbox.gain(seat(1)), 2.0F);
  EXPECT_EQ(inbox.haptic_gain(seat(1)), 2.0F);
  inbox.set_gain(seat(1), 0.5F);
  inbox.set_gain(seat(2), 9.0F);
  inbox.set_gain(seat(3), std::numeric_limits<float>::quiet_NaN());
  EXPECT_EQ(inbox.gain(), 2.0F);
  EXPECT_EQ(inbox.gain(seat(0)), 2.0F);
  EXPECT_EQ(inbox.gain(seat(1)), 0.5F);
  EXPECT_EQ(inbox.gain(seat(2)), 2.4F);
  EXPECT_EQ(inbox.gain(seat(3)), 0.0F);
}

TEST_F(FeedbackInboxGain, ScalesASeatGainByTheConsoleLevel) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  inbox.set_gain(2.0F);
  inbox.set_gain(seat(1), 0.5F);
  EXPECT_EQ(
      inbox.accept(
          {seat(0), controllers::HapticIntensity{FeedbackIntensity::medium}},
          start),
      Admission::held);
  EXPECT_EQ(inbox.haptic_gain(), 1.0F);
  EXPECT_EQ(inbox.haptic_gain(seat(1)), 0.25F);
  EXPECT_EQ(haptics(inbox, 256, 0x01, 0), Admission::held);
  EXPECT_EQ(haptics(inbox, 256, 0x01, 1), Admission::held);
  const auto loud = inbox.take_haptic_block(seat(0), start);
  ASSERT_TRUE(loud);
  EXPECT_EQ(loud->gain(), 1.0F);
  const auto quiet = inbox.take_haptic_block(seat(1), start);
  ASSERT_TRUE(quiet);
  EXPECT_EQ(quiet->gain(), 0.25F);
  EXPECT_EQ(quiet->intensity(), FeedbackIntensity::medium);
}

TEST_F(FeedbackInboxGain, KeepsASeatGainWhenTheSeatIsForgotten) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  inbox.set_gain(2.0F);
  inbox.set_gain(seat(1), 0.5F);
  EXPECT_EQ(
      inbox.accept(
          {seat(0), controllers::HapticIntensity{FeedbackIntensity::medium}},
          start),
      Admission::held);
  inbox.forget(seat(1));
  EXPECT_EQ(inbox.gain(seat(1)), 0.5F);
  inbox.set_gain(seat(1), std::nullopt);
  EXPECT_EQ(inbox.gain(seat(1)), 2.0F);
  EXPECT_EQ(inbox.haptic_gain(seat(1)), 1.0F);
}

}  // namespace
}  // namespace play_test
