#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <utility>

namespace play_test {
namespace {

using libchiaking::media::VideoFrame;

class VideoFrames : public ::testing::Test {
 protected:
  static auto frame() -> VideoFrame {
    VideoFrame::Facts facts;
    facts.bytes = {std::byte{1}, std::byte{2}, std::byte{3}};
    facts.frame_index = 9;
    facts.codec = 2;
    facts.frames_lost = 4;
    facts.recovered = true;
    return VideoFrame{std::move(facts)};
  }
};

TEST_F(VideoFrames, ArePaddedForDecoders) {
  const auto video = frame();
  EXPECT_EQ(video.size(), 3u);
  ASSERT_EQ(video.bytes().size(), 3u);
  EXPECT_EQ(video.bytes()[2], std::byte{3});
  ASSERT_EQ(video.padded_bytes().size(), 3 + VideoFrame::tail_padding);
  EXPECT_EQ(video.padded_bytes()[3], std::byte{0});
}

TEST_F(VideoFrames, KeepTheirFacts) {
  const auto video = frame();
  EXPECT_EQ(video.frame_index(), 9u);
  EXPECT_EQ(video.codec(), 2);
  EXPECT_EQ(video.frames_lost(), 4u);
  EXPECT_TRUE(video.recovered());
  EXPECT_FALSE(video.profile_header());
  EXPECT_FALSE(video.dynamic_range_flag());
  EXPECT_FALSE(video.suppress_presentation());
  EXPECT_EQ(video.adaptive_stream_index(), 0u);
}

}  // namespace
}  // namespace play_test
