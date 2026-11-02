#include "src/core/media/adapters/audio_output_layout.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace play_test {
namespace {

using libchiaking::media::AudioLayoutError;
using libchiaking::media::AudioLayoutSource;
using Layout = libchiaking::media::AudioOutputLayout;
using libchiaking::net::protocol::remote_play::AudioChannelPosition;

class AudioOutputLayout : public ::testing::Test {
 protected:
  template <std::size_t N>
  static auto targets(const Layout& layout,
                      const std::array<std::uint8_t, N>& expected)
      -> ::testing::AssertionResult {
    const auto routes = layout.routes();
    if (routes.size() != N)
      return ::testing::AssertionFailure()
             << "has " << routes.size() << " routes, expected " << N;
    if (!std::ranges::equal(routes, expected, {}, &Layout::Route::target))
      return ::testing::AssertionFailure() << "routes to other targets";
    return ::testing::AssertionSuccess();
  }

  static constexpr std::array<AudioChannelPosition, 12> unknown{};
};

TEST_F(AudioOutputLayout, AssumesTheConsoleChannelOrderForSixChannels) {
  const auto six = Layout::plan(6, unknown);
  ASSERT_TRUE(six);
  EXPECT_EQ(six.value().source(), AudioLayoutSource::assumed);
  EXPECT_EQ(six.value().output_channels(), 6);
  EXPECT_TRUE(
      targets(six.value(), std::array<std::uint8_t, 6>{0, 1, 2, 3, 4, 5}));
}

TEST_F(AudioOutputLayout, AssumesTheConsoleChannelOrderForEightChannels) {
  const auto eight = Layout::plan(8, unknown);
  ASSERT_TRUE(eight);
  EXPECT_EQ(eight.value().source(), AudioLayoutSource::assumed);
  EXPECT_EQ(eight.value().output_channels(), 8);
  EXPECT_TRUE(targets(eight.value(),
                      std::array<std::uint8_t, 8>{0, 1, 2, 3, 6, 7, 4, 5}));
}

TEST_F(AudioOutputLayout, AssumesTwelveChannelsPassAtUnityGain) {
  const auto twelve = Layout::plan(12);
  ASSERT_TRUE(twelve);
  EXPECT_EQ(twelve.value().source(), AudioLayoutSource::assumed);
  EXPECT_EQ(twelve.value().output_channels(), 12);
  EXPECT_EQ(twelve.value().input_channels(), 12);
  EXPECT_TRUE(targets(
      twelve.value(),
      std::array<std::uint8_t, 12>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}));
  for (const auto& route : twelve.value().routes()) {
    EXPECT_EQ(route.gain, 32768);
  }
  EXPECT_EQ(Layout::unity_gain, 32768);
  EXPECT_EQ(Layout::height_gain, 23170);
}

TEST_F(AudioOutputLayout, PassesStereoStraightThrough) {
  const std::array stereo{AudioChannelPosition::front_left,
                          AudioChannelPosition::front_right};
  const auto two = Layout::plan(2, stereo);
  ASSERT_TRUE(two);
  EXPECT_EQ(two.value().source(), AudioLayoutSource::direct);
  EXPECT_FALSE(two.value().remaps());
  EXPECT_EQ(two.value().output_channels(), 2);
  const std::array<std::int16_t, 4> input{1, -2, 3, -4};
  std::array<std::int16_t, 4> output{};
  EXPECT_EQ(two.value().apply(input, output), 2);
  EXPECT_EQ(output, input);
}

TEST_F(AudioOutputLayout, PassesMonoStraightThrough) {
  const auto one = Layout::plan(1);
  ASSERT_TRUE(one);
  EXPECT_EQ(one.value().output_channels(), 1);
  EXPECT_FALSE(one.value().remaps());
}

TEST_F(AudioOutputLayout, FollowsReportedPositions) {
  const std::array reported{
      AudioChannelPosition::front_left,   AudioChannelPosition::front_right,
      AudioChannelPosition::front_center, AudioChannelPosition::lfe,
      AudioChannelPosition::side_left,    AudioChannelPosition::side_right};
  const auto layout = Layout::plan(6, reported);
  ASSERT_TRUE(layout);
  EXPECT_EQ(layout.value().source(), AudioLayoutSource::reported);
  EXPECT_EQ(layout.value().output_channels(), 6);
  EXPECT_TRUE(layout.value().remaps());
  EXPECT_TRUE(
      targets(layout.value(), std::array<std::uint8_t, 6>{0, 1, 2, 3, 4, 5}));
}

TEST_F(AudioOutputLayout, AssumesTheOrderWhenReportedPositionsRepeat) {
  const std::array reported{
      AudioChannelPosition::front_left,   AudioChannelPosition::front_right,
      AudioChannelPosition::front_center, AudioChannelPosition::lfe,
      AudioChannelPosition::side_left,    AudioChannelPosition::side_left};
  const auto repeated = Layout::plan(6, reported);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(repeated.value().source(), AudioLayoutSource::assumed);
}

TEST_F(AudioOutputLayout, AssumesTheOrderWhenTooFewPositionsAreReported) {
  const std::array reported{
      AudioChannelPosition::front_left,   AudioChannelPosition::front_right,
      AudioChannelPosition::front_center, AudioChannelPosition::lfe,
      AudioChannelPosition::side_left,    AudioChannelPosition::side_left};
  const auto too_few = Layout::plan(6, std::span{reported}.first(4));
  ASSERT_TRUE(too_few);
  EXPECT_EQ(too_few.value().source(), AudioLayoutSource::assumed);
}

TEST_F(AudioOutputLayout, RefusesChannelCountsItCannotLayOut) {
  const auto five = Layout::plan(5);
  ASSERT_FALSE(five);
  EXPECT_EQ(five.error(), AudioLayoutError::unknown_layout);
  const auto thirteen = Layout::plan(13);
  ASSERT_FALSE(thirteen);
  EXPECT_EQ(thirteen.error(), AudioLayoutError::invalid_channel_count);
  const auto none = Layout::plan(0);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error(), AudioLayoutError::invalid_channel_count);
}

TEST_F(AudioOutputLayout, KeepsTheHeightsOfTwelveChannels) {
  const auto layout = Layout::plan(12);
  ASSERT_TRUE(layout);
  const std::array<std::int16_t, 12> input{1, 2, 3, 4,  5,  6,
                                           7, 8, 9, 10, 11, 12};
  std::array<std::int16_t, 24> output{};
  EXPECT_EQ(layout.value().apply(input, output), 1);
  const std::array<std::int16_t, 12> atmos{1, 2, 3, 4,  5,  6,
                                           7, 8, 9, 10, 11, 12};
  EXPECT_TRUE(std::ranges::equal(std::span{output}.first(12), atmos));
}

TEST_F(AudioOutputLayout, ClampsReportedHeightsFoldedIntoEightChannels) {
  const std::array reported_heights{AudioChannelPosition::front_left,
                                    AudioChannelPosition::front_right,
                                    AudioChannelPosition::front_center,
                                    AudioChannelPosition::lfe,
                                    AudioChannelPosition::back_left,
                                    AudioChannelPosition::back_right,
                                    AudioChannelPosition::top_front_left,
                                    AudioChannelPosition::top_front_right};
  const auto folded = Layout::plan(8, reported_heights);
  ASSERT_TRUE(folded);
  EXPECT_EQ(folded.value().output_channels(), 8);
  const std::array<std::int16_t, 8> loud{30000, -30000, 0,     0,
                                         0,     0,      30000, -30000};
  std::array<std::int16_t, 8> clamped{};
  EXPECT_EQ(folded.value().apply(loud, clamped), 1);
  EXPECT_EQ(clamped[0], std::numeric_limits<std::int16_t>::max());
  EXPECT_EQ(clamped[1], std::numeric_limits<std::int16_t>::min());
}

TEST_F(AudioOutputLayout, ReordersEightChannels) {
  const auto eight = Layout::plan(8);
  ASSERT_TRUE(eight);
  const std::array<std::int16_t, 8> seven{1, 2, 3, 4, 5, 6, 7, 8};
  std::array<std::int16_t, 8> routed{};
  EXPECT_EQ(eight.value().apply(seven, routed), 1);
  EXPECT_EQ(routed, (std::array<std::int16_t, 8>{1, 2, 3, 4, 7, 8, 5, 6}));
}

TEST_F(AudioOutputLayout, AppliesNothingToAnOutputTooSmallForAFrame) {
  const auto eight = Layout::plan(8);
  ASSERT_TRUE(eight);
  const std::array<std::int16_t, 8> seven{1, 2, 3, 4, 5, 6, 7, 8};
  std::array<std::int16_t, 4> small{};
  EXPECT_EQ(eight.value().apply(seven, small), 0);
}

}  // namespace
}  // namespace play_test
