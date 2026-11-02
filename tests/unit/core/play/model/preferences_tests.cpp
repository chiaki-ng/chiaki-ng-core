#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using libchiaking::media::VideoCodec;
using play::OutputRequest;
using play::PictureRequest;
using play::Preferences;
using play::SoundRequest;

constexpr play::AccessibilityRequest small{true, 8, 24, -1, true, false};
static_assert(small.screen_reader() && small.screen_reader_speed() == 8);
static_assert(small.screen_reader_volume() == 24 && small.text_size() == -1);
static_assert(small.bold_text() && !small.higher_contrast());
constexpr play::AccessibilityRequest very_large{false, 0, 0, 2, false, true};
static_assert(very_large.text_size() == 2 && very_large.higher_contrast());
constexpr play::AccessibilityRequest out_of_range{false, 9,     25,
                                                  3,     false, false};
static_assert(out_of_range.screen_reader_speed() == 3);
static_assert(out_of_range.screen_reader_volume() == 12);
static_assert(out_of_range.text_size() == 0);
static_assert(play::AccessibilityRequest{false, -1, -1, -2, false, false}
                  .text_size() == 0);

TEST(OutputRequest, LimitsNothingByDefault) {
  const OutputRequest open;
  EXPECT_FALSE(open.largest_picture_side());
  EXPECT_FALSE(open.audio_failure_ends_session());
  EXPECT_EQ(*OutputRequest::create(std::nullopt, false), open);
}

TEST(OutputRequest, RefusesAPictureSideOfZero) {
  EXPECT_FALSE(OutputRequest::create(0, false));
}

TEST(OutputRequest, KeepsWhatTheOutputCanTake) {
  const OutputRequest open;
  const auto limited = OutputRequest::create(4096, true);
  ASSERT_TRUE(limited);
  EXPECT_EQ(limited->largest_picture_side(), 4096U);
  EXPECT_TRUE(limited->audio_failure_ends_session());
  const auto strict = OutputRequest::create(std::nullopt, true);
  ASSERT_TRUE(strict);
  EXPECT_FALSE(strict->largest_picture_side());
  EXPECT_TRUE(strict->audio_failure_ends_session());
  EXPECT_NE(*limited, open);
}

TEST(Preferences, CarryTheOutputRequest) {
  const auto limited = OutputRequest::create(4096, true);
  ASSERT_TRUE(limited);
  Preferences preferences;
  preferences.output = *limited;
  EXPECT_NE(preferences, Preferences{});
  EXPECT_EQ(preferences.output, *limited);
}

TEST(Preferences, ArePassedAsOneValue) {
  Preferences preferences;
  EXPECT_EQ(preferences, Preferences{});
  const auto picture =
      PictureRequest::create(1080, 60, 15000, VideoCodec::h265);
  ASSERT_TRUE(picture);
  preferences.picture = *picture;
  preferences.sound = *SoundRequest::create(6);
  EXPECT_NE(preferences, Preferences{});
  EXPECT_EQ(preferences.picture.height(), 1080u);
}

}  // namespace
}  // namespace play_test
