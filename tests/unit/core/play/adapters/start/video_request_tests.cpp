#include "src/core/play/adapters/start/link_request.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using LocalVideoCodec = libchiaking::net::protocol::remote_play::VideoCodec;
using play::ConfigurationError;
using play::VideoRequest;

static_assert(play::PortGuessing::minimum_count == 5);
static_assert(play::PortGuessing::default_count == 75);
static_assert(play::PortGuessing::maximum_count == 80);

class VideoRequests : public ::testing::Test {
 protected:
  static auto video(std::uint32_t height, std::uint32_t fps,
                    std::uint32_t bitrate, LocalVideoCodec codec,
                    const play::VideoRequestLimits& limits = {}) -> bool {
    const auto request =
        VideoRequest::create(height, fps, bitrate, codec, limits);
    if (!request) {
      EXPECT_EQ(request.error(), ConfigurationError::invalid_video);
    }
    return request.has_value();
  }
};

TEST_F(VideoRequests, AcceptWhatTheDefaultLimitsAllow) {
  EXPECT_TRUE(video(1080, 60, 30000, LocalVideoCodec::h265_hdr));
  EXPECT_TRUE(video(2160, 60, 30000, LocalVideoCodec::h265_hdr));
  EXPECT_TRUE(video(540, 60, 8000, LocalVideoCodec::h265));
  EXPECT_TRUE(video(360, 30, 2000, LocalVideoCodec::h264));
  EXPECT_TRUE(video(1080, 60, 100000, LocalVideoCodec::h264));
  EXPECT_TRUE(video(1080, 60, 20000, LocalVideoCodec::h264));
  for (const auto height : {720U, 1080U, 1440U, 2160U}) {
    SCOPED_TRACE(height);
    EXPECT_TRUE(video(height, 60, 35000, LocalVideoCodec::h265));
  }
}

TEST_F(VideoRequests, RefuseWhatTheDefaultLimitsForbid) {
  EXPECT_FALSE(video(2160, 60, 30000, LocalVideoCodec::h264));
  EXPECT_FALSE(video(1440, 60, 30000, LocalVideoCodec::h264));
  EXPECT_FALSE(video(1080, 60, 100001, LocalVideoCodec::h264));
  EXPECT_FALSE(video(1080, 60, 1999, LocalVideoCodec::h264));
  EXPECT_FALSE(video(2000, 60, 35000, LocalVideoCodec::h265));
  EXPECT_FALSE(video(1080, 45, 30000, LocalVideoCodec::h264));
  EXPECT_FALSE(video(1080, 60, 30000, static_cast<LocalVideoCodec>(3)));
}

TEST_F(VideoRequests, FollowNarrowerLimits) {
  const play::VideoRequestLimits narrow{.minimum_height = 720,
                                        .maximum_height = 1080,
                                        .maximum_hevc_height = 1080,
                                        .minimum_bitrate_kbps = 2000,
                                        .maximum_bitrate_kbps = 35000};
  EXPECT_TRUE(video(720, 60, 10000, LocalVideoCodec::h264, narrow));
  EXPECT_TRUE(video(1080, 60, 35000, LocalVideoCodec::h265, narrow));
  EXPECT_FALSE(video(540, 60, 10000, LocalVideoCodec::h264, narrow));
  EXPECT_FALSE(video(1440, 60, 10000, LocalVideoCodec::h265, narrow));
  EXPECT_FALSE(video(1080, 60, 35001, LocalVideoCodec::h264, narrow));
}

TEST_F(VideoRequests, DefaultTo720pH264) {
  const VideoRequest standard;
  EXPECT_EQ(standard.height(), 720u);
  EXPECT_EQ(standard.width(), 1280u);
  EXPECT_EQ(standard.fps(), 60u);
  EXPECT_EQ(standard.bitrate_kbps(), 10000u);
  EXPECT_EQ(standard.codec(), LocalVideoCodec::h264);
}

TEST_F(VideoRequests, PickTheCodecForTheConsoleAndFillTheSettings) {
  const auto hdr =
      VideoRequest::create(1080, 60, 30000, LocalVideoCodec::h265_hdr);
  ASSERT_TRUE(hdr);
  EXPECT_EQ(hdr.value().width(), 1920u);
  EXPECT_EQ(hdr.value().codec_for(libchiaking::consoles::Generation::ps5),
            LocalVideoCodec::h265_hdr);
  EXPECT_EQ(hdr.value().codec_for(libchiaking::consoles::Generation::ps4),
            LocalVideoCodec::h264);
  EXPECT_EQ(VideoRequest::create(360, 30, 2000, LocalVideoCodec::h264)
                .value()
                .width(),
            640u);
  const auto settings = hdr.value().settings();
  EXPECT_EQ(settings.width, 1920u);
  EXPECT_EQ(settings.height, 1080u);
  EXPECT_EQ(settings.max_fps, 60u);
  EXPECT_EQ(settings.bitrate_kbps, 30000u);
  EXPECT_TRUE(settings.auto_downgrade);
}

TEST(PortGuessing, IsClamped) {
  EXPECT_FALSE(play::PortGuessing{}.forced());
  EXPECT_EQ(play::PortGuessing{}.count(), 75u);
  EXPECT_EQ(play::PortGuessing(true, 0).count(), 5u);
  EXPECT_EQ(play::PortGuessing(true, 1000).count(), 80u);
  EXPECT_TRUE(play::PortGuessing(true, 70).forced());
  EXPECT_EQ(play::PortGuessing(true, 70).count(), 70u);
}

}  // namespace
}  // namespace play_test
