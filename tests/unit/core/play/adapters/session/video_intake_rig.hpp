#pragma once

#include "src/core/media/video_intake.hpp"
#include "tests/unit/core/play/session_fakes.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

namespace play_test {

class VideoIntakeTest : public testing::Test {
 protected:
  using Intake = libchiaking::media::VideoIntake;
  using Media = libchiaking::net::protocol::remote_play::event::EncodedMedia;

  static auto frame(std::uint16_t index, std::uint8_t profile = 0)
      -> libchiaking::media::VideoFrame {
    return *Intake::frame(api_test::video_media(index, profile));
  }

  static auto frame(Media media) -> libchiaking::media::VideoFrame {
    return *Intake::frame(std::move(media));
  }

  auto clock() -> api_test::ManualClock& { return clock_; }

 private:
  api_test::ManualClock clock_;
};

}  // namespace play_test
