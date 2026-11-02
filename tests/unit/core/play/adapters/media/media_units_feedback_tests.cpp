#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/media/feedback_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace play_test {
namespace {

namespace controllers = libchiaking::controllers;
using libchiaking::controllers::FeedbackIntensity;
using libchiaking::play::MediaUnits;
using Kind = libchiaking::controllers::ControllerFeedback::Kind;

class MediaUnitsFeedback : public FeedbackRig {};

class MediaUnitsWire : public FeedbackRig {};

TEST_F(MediaUnitsFeedback, ReadsRumbleForTheSeatItNames) {
  const auto rumble = MediaUnits::feedback(
      wire(MediaKind::rumble,
           {std::byte{2}, std::byte{200}, std::byte{100}, std::byte{7}}));
  ASSERT_TRUE(rumble);
  EXPECT_EQ(rumble->kind(), Kind::rumble);
  EXPECT_EQ(rumble->seat, seat(2));
  ASSERT_TRUE(rumble->get<controllers::Rumble>());
  EXPECT_EQ(*rumble->get<controllers::Rumble>(),
            (controllers::Rumble{200, 100}));
  EXPECT_FALSE(rumble->get<controllers::TriggerEffects>());
}

TEST_F(MediaUnitsFeedback, RefusesAShortRumble) {
  EXPECT_FALSE(MediaUnits::feedback(
      wire(MediaKind::rumble, {std::byte{2}, std::byte{200}})));
}

TEST_F(MediaUnitsFeedback, ReadsTriggerEffectsForTheSeatTheBodyNames) {
  const auto effects =
      MediaUnits::feedback(wire(MediaKind::trigger_effects, trigger_body(6)));
  ASSERT_TRUE(effects);
  EXPECT_EQ(effects->kind(), Kind::trigger_effects);
  EXPECT_EQ(effects->seat, seat(2));
  ASSERT_TRUE(effects->get<controllers::TriggerEffects>());
  EXPECT_EQ(*effects->get<controllers::TriggerEffects>(), triggers());
  const auto packed = effects->get<controllers::TriggerEffects>()->packed();
  ASSERT_EQ(packed.size(), 22);
  EXPECT_EQ(packed[0], 0x21);
  EXPECT_EQ(packed[1], 0xaa);
  EXPECT_EQ(packed[11], 0x26);
  EXPECT_EQ(packed[12], 0xbb);
  EXPECT_EQ(packed[21], 0x08);
}

TEST_F(MediaUnitsFeedback, RefusesShortTriggerEffects) {
  EXPECT_FALSE(MediaUnits::feedback(
      wire(MediaKind::trigger_effects, std::vector<std::byte>(24))));
}

TEST_F(MediaUnitsFeedback, ReadsTheLightbarColourForTheFirstSeat) {
  const auto light = MediaUnits::feedback(
      wire(MediaKind::led_color, {std::byte{1}, std::byte{2}, std::byte{3}}));
  ASSERT_TRUE(light);
  EXPECT_EQ(light->kind(), Kind::led_color);
  EXPECT_EQ(light->seat, SeatNumber::first());
  ASSERT_TRUE(light->get<controllers::Lightbar>());
  EXPECT_EQ(light->get<controllers::Lightbar>()->colour,
            (controllers::SeatLight{1, 2, 3}));
  EXPECT_FALSE(MediaUnits::feedback(
      wire(MediaKind::led_color, {std::byte{1}, std::byte{2}})));
}

TEST_F(MediaUnitsFeedback, ReadsThePlayerIndex) {
  const auto leds =
      MediaUnits::feedback(wire(MediaKind::player_index, {std::byte{3}}));
  ASSERT_TRUE(leds);
  EXPECT_EQ(leds->kind(), Kind::player_index);
  ASSERT_TRUE(leds->get<controllers::PlayerLeds>());
  EXPECT_EQ(leds->get<controllers::PlayerLeds>()->player, 3);
  EXPECT_FALSE(MediaUnits::feedback(wire(MediaKind::player_index, {})));
}

TEST_F(MediaUnitsFeedback, ReadsAConfirmedSeatWithItsColour) {
  const auto confirmed = MediaUnits::feedback(
      wire(MediaKind::pad_confirmed,
           {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{255}}));
  ASSERT_TRUE(confirmed);
  EXPECT_EQ(confirmed->kind(), Kind::seat_confirmed);
  EXPECT_EQ(confirmed->seat, seat(1));
  ASSERT_TRUE(confirmed->get<controllers::SeatConfirmed>());
  EXPECT_EQ(confirmed->get<controllers::SeatConfirmed>()->colour,
            (controllers::SeatLight{0, 0, 255}));
  EXPECT_FALSE(
      MediaUnits::feedback(wire(MediaKind::pad_confirmed, {std::byte{1}})));
}

TEST_F(MediaUnitsFeedback, ReadsTheConsoleIntensities) {
  const auto haptic_level = MediaUnits::feedback(
      wire(MediaKind::haptic_intensity, {std::byte{0x06}}));
  ASSERT_TRUE(haptic_level);
  ASSERT_TRUE(haptic_level->get<controllers::HapticIntensity>());
  EXPECT_EQ(haptic_level->get<controllers::HapticIntensity>()->level,
            FeedbackIntensity::medium);
  const auto trigger_level =
      MediaUnits::feedback(wire(MediaKind::trigger_intensity, {std::byte{3}}));
  ASSERT_TRUE(trigger_level);
  ASSERT_TRUE(trigger_level->get<controllers::TriggerIntensity>());
  EXPECT_EQ(trigger_level->get<controllers::TriggerIntensity>()->level,
            FeedbackIntensity::weak);
  EXPECT_FALSE(MediaUnits::feedback(
      wire(MediaKind::trigger_intensity, {std::byte{3}, std::byte{3}})));
}

TEST_F(MediaUnitsFeedback, ReadsAMotionReset) {
  const auto reset = MediaUnits::feedback(wire(MediaKind::motion_reset, {}));
  ASSERT_TRUE(reset);
  EXPECT_EQ(reset->kind(), Kind::motion_reset);
  EXPECT_TRUE(reset->get<controllers::MotionReset>());
}

TEST_F(MediaUnitsFeedback, ReadsMotionSettings) {
  const auto settings = MediaUnits::feedback(
      wire(MediaKind::motion_settings, {std::byte{0}, std::byte{1}}));
  ASSERT_TRUE(settings);
  EXPECT_EQ(settings->kind(), Kind::motion_settings);
  ASSERT_TRUE(settings->get<controllers::MotionSettings>());
  EXPECT_EQ(*settings->get<controllers::MotionSettings>(),
            (controllers::MotionSettings{0, 1}));
  EXPECT_FALSE(
      MediaUnits::feedback(wire(MediaKind::motion_settings, {std::byte{1}})));
}

TEST_F(MediaUnitsFeedback, LendsHapticAudioWithoutCopyingIt) {
  const auto sound =
      wire(MediaKind::haptics, std::vector<std::byte>(120, std::byte{0x12}), 3);
  const auto audio = MediaUnits::feedback(sound);
  ASSERT_TRUE(audio);
  EXPECT_EQ(audio->kind(), Kind::haptics);
  EXPECT_EQ(audio->seat, seat(3));
  ASSERT_TRUE(audio->get<HapticAudio>());
  EXPECT_EQ(audio->get<HapticAudio>()->pcm.data(), sound.bytes.data());
  EXPECT_EQ(audio->get<HapticAudio>()->pcm.size(), 120);
  EXPECT_TRUE(audio->get<HapticAudio>()->playable());
}

TEST_F(MediaUnitsFeedback, RefusesHapticAudioForASeatOutOfRange) {
  EXPECT_FALSE(MediaUnits::feedback(
      wire(MediaKind::haptics, std::vector<std::byte>(120), 4)));
}

TEST_F(MediaUnitsFeedback, IgnoresAudioAndVideo) {
  EXPECT_FALSE(MediaUnits::feedback(wire(MediaKind::audio, {std::byte{1}})));
  EXPECT_FALSE(MediaUnits::feedback(wire(MediaKind::video, {std::byte{1}})));
}

TEST_F(MediaUnitsWire, WritesTriggerEffectsForTheirSeat) {
  const auto media = MediaUnits::wire({seat(2), triggers()});
  EXPECT_EQ(media.kind, MediaKind::trigger_effects);
  EXPECT_EQ(media.bytes, trigger_body(2));
}

TEST_F(MediaUnitsWire, WritesRumbleForItsSeat) {
  const auto media = MediaUnits::wire({seat(1), controllers::Rumble{9, 8}});
  EXPECT_EQ(media.kind, MediaKind::rumble);
  EXPECT_EQ(media.bytes, (std::vector{std::byte{1}, std::byte{9}, std::byte{8},
                                      std::byte{0}}));
}

TEST_F(MediaUnitsWire, WritesAConfirmedSeatWithItsColour) {
  const auto media =
      MediaUnits::wire({seat(1), controllers::SeatConfirmed{{4, 5, 6}}});
  EXPECT_EQ(media.kind, MediaKind::pad_confirmed);
  EXPECT_EQ(media.bytes, (std::vector{std::byte{1}, std::byte{4}, std::byte{5},
                                      std::byte{6}}));
}

TEST_F(MediaUnitsWire, WritesTheLightbarColour) {
  const auto media =
      MediaUnits::wire({seat(0), controllers::Lightbar{{4, 5, 6}}});
  EXPECT_EQ(media.kind, MediaKind::led_color);
  EXPECT_EQ(media.bytes,
            (std::vector{std::byte{4}, std::byte{5}, std::byte{6}}));
}

TEST_F(MediaUnitsWire, WritesTheHapticIntensity) {
  const auto media = MediaUnits::wire(
      {seat(0), controllers::HapticIntensity{FeedbackIntensity::medium}});
  EXPECT_EQ(media.kind, MediaKind::haptic_intensity);
  EXPECT_EQ(media.bytes, std::vector{std::byte{2}});
}

TEST_F(MediaUnitsWire, WritesThePlayerIndex) {
  const auto media = MediaUnits::wire({seat(0), controllers::PlayerLeds{3}});
  EXPECT_EQ(media.kind, MediaKind::player_index);
  EXPECT_EQ(media.bytes, std::vector{std::byte{3}});
}

TEST_F(MediaUnitsWire, WritesAnEmptyMotionReset) {
  const auto media = MediaUnits::wire({seat(0), controllers::MotionReset{}});
  EXPECT_EQ(media.kind, MediaKind::motion_reset);
  EXPECT_TRUE(media.bytes.empty());
}

}  // namespace
}  // namespace play_test
