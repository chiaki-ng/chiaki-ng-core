#include "libchiaking/play_types.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace play_test {
namespace {

using Audio = libchiaking::controllers::HapticAudio;
using Block = libchiaking::controllers::HapticBlock;
using libchiaking::controllers::FeedbackIntensity;

class HapticBlock : public ::testing::Test {
 protected:
  static auto loud() -> std::array<std::byte, Block::size> {
    std::array<std::byte, Block::size> pcm{};
    pcm[0] = std::byte{0x10};
    pcm[1] = std::byte{0x27};
    pcm[2] = std::byte{0xf0};
    pcm[3] = std::byte{0xd8};
    pcm[4] = std::byte{0x80};
    return pcm;
  }
};

TEST(HapticAudio, PublishesItsFrameShape) {
  EXPECT_EQ(Audio::frame_bytes, 4);
  EXPECT_EQ(Audio::max_bytes, 4096);
  EXPECT_EQ(Audio::sample_rate, 3000);
}

TEST(HapticAudio, IsPlayableOnlyInWholeFramesUpToItsLimit) {
  const std::vector<std::byte> pcm(4100);
  EXPECT_FALSE(Audio{}.playable());
  EXPECT_FALSE(Audio{std::span{pcm}.first(3)}.playable());
  EXPECT_TRUE(Audio{std::span{pcm}.first(4)}.playable());
  EXPECT_TRUE(Audio{std::span{pcm}.first(4096)}.playable());
  EXPECT_FALSE(Audio{pcm}.playable());
}

TEST_F(HapticBlock, SaturatesScaledSamples) {
  const auto pcm = loud();
  const Block block{pcm, FeedbackIntensity::medium, 4.0F};
  const auto samples = block.scaled_samples();
  ASSERT_EQ(samples.size(), 128);
  EXPECT_EQ(samples[0], std::numeric_limits<std::int16_t>::max());
  EXPECT_EQ(samples[1], std::numeric_limits<std::int16_t>::min());
  EXPECT_EQ(samples[2], 512);
  EXPECT_EQ(samples[3], 0);
  EXPECT_FALSE(block.silent());
  EXPECT_EQ(block.intensity(), FeedbackIntensity::medium);
}

TEST_F(HapticBlock, ScalesSamplesDownByItsGain) {
  const auto pcm = loud();
  const Block half{pcm, FeedbackIntensity::strong, 0.5F};
  EXPECT_EQ(half.scaled_samples()[0], 5000);
  EXPECT_EQ(half.scaled_samples()[1], -5000);
}

TEST_F(HapticBlock, IsSilentWhenEverySampleStaysBelowTheThreshold) {
  std::array<std::byte, Block::size> faint{};
  faint[0] = std::byte{0xff};
  EXPECT_TRUE(Block(faint, FeedbackIntensity::strong, 1.0F).silent());
  EXPECT_FALSE(Block(faint, FeedbackIntensity::strong, 1.1F).silent());
  EXPECT_TRUE(Block(loud(), FeedbackIntensity::off, 0.0F).silent());
}

}  // namespace
}  // namespace play_test
