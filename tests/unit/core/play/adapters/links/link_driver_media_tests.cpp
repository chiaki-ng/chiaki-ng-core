#include "src/core/media/video_intake.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/link_driver_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::media::VideoFrame;
using link_drivers::Rig;
using remote_play::EncodedMediaKind;

class LinkDriverMedia : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(rig_.driver().start());
    video_.kind = EncodedMediaKind::video;
    video_.bytes = {std::byte{1}, std::byte{2}, std::byte{3}};
    video_.frame_index = 77;
    video_.codec = 2;
    video_.adaptive_stream_index = 1;
    video_.frames_lost = 4;
    video_.frame_recovered = true;
    video_.suppress_presentation = true;
    audio_.kind = EncodedMediaKind::audio;
    audio_.bytes = {std::byte{9}, std::byte{8}};
    rig_.session().pending_events.emplace_back(video_);
    rig_.session().pending_events.emplace_back(audio_);
    ASSERT_TRUE(rig_.driver().pump_once());
    ASSERT_EQ(rig_.sink().frames.size(), 1U);
    ASSERT_EQ(rig_.sink().media_events.size(), 1U);
  }

  auto frame() -> const VideoFrame& { return rig_.sink().frames.front(); }
  auto media() -> const remote_play::event::EncodedMedia& {
    return rig_.sink().media_events.front();
  }
  auto video() const -> const remote_play::event::EncodedMedia& {
    return video_;
  }
  auto audio() -> remote_play::event::EncodedMedia& { return audio_; }

 private:
  Rig rig_;
  remote_play::event::EncodedMedia video_;
  remote_play::event::EncodedMedia audio_;
};

TEST_F(LinkDriverMedia, HandsVideoOnAsAFrameWithReadableTailPadding) {
  EXPECT_EQ(VideoFrame::tail_padding, 64U);
  EXPECT_EQ(frame().size(), 3U);
  EXPECT_EQ(frame().bytes().size(), 3U);
  EXPECT_TRUE(std::ranges::equal(frame().bytes(), video().bytes));
  EXPECT_EQ(frame().padded_bytes().size(), 3 + VideoFrame::tail_padding);
  EXPECT_EQ(frame().padded_bytes().data(), frame().bytes().data());
  EXPECT_TRUE(std::ranges::all_of(
      frame().padded_bytes().subspan(3),
      [](std::byte value) { return value == std::byte{}; }));
}

TEST_F(LinkDriverMedia, KeepsTheFrameDetails) {
  EXPECT_EQ(frame().frame_index(), 77U);
  EXPECT_EQ(frame().codec(), 2U);
  EXPECT_EQ(frame().adaptive_stream_index(), 1U);
  EXPECT_EQ(frame().frames_lost(), 4U);
  EXPECT_TRUE(frame().recovered());
  EXPECT_TRUE(frame().suppress_presentation());
  EXPECT_FALSE(frame().dynamic_range_flag());
}

TEST_F(LinkDriverMedia, PassesOtherMediaThroughAsEvents) {
  EXPECT_EQ(media().kind, EncodedMediaKind::audio);
  EXPECT_EQ(media().bytes, audio().bytes);
  EXPECT_FALSE(libchiaking::media::VideoIntake::frame(std::move(audio())));
  EXPECT_EQ(audio().bytes.size(), 2U);
}

}  // namespace
}  // namespace play_test
