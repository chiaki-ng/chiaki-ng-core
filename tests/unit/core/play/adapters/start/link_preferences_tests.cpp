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
namespace remote_play = libchiaking::net::protocol::remote_play;
using play::LinkPreferences;
using MediaCodec = libchiaking::media::VideoCodec;
using LinkCodec = remote_play::VideoCodec;

class LinkPicture : public ::testing::Test {
 protected:
  static auto picture(std::uint32_t height, std::uint32_t fps,
                      std::uint32_t bitrate, MediaCodec codec)
      -> play::Preferences {
    play::Preferences preferences;
    preferences.picture =
        play::PictureRequest::create(height, fps, bitrate, codec).value();
    return preferences;
  }
};

TEST_F(LinkPicture, AsksAPs4ForH264At720LinesAtMost) {
  const auto preferences = picture(1080, 60, 15000, MediaCodec::h265);
  const auto ps5 =
      LinkPreferences::on_console(preferences, Generation::ps5).value();
  EXPECT_EQ(ps5.video.height(), 1080u);
  EXPECT_EQ(ps5.video.width(), 1920u);
  EXPECT_EQ(ps5.video.fps(), 60u);
  EXPECT_EQ(ps5.video.bitrate_kbps(), 15000u);
  EXPECT_EQ(ps5.video.codec(), LinkCodec::h265);
  const auto ps4 =
      LinkPreferences::on_console(preferences, Generation::ps4).value();
  EXPECT_EQ(ps4.video.height(), 720u);
  EXPECT_EQ(ps4.video.codec(), LinkCodec::h264);
  EXPECT_EQ(ps4.video.fps(), 60u);
  EXPECT_EQ(ps4.video.bitrate_kbps(), 15000u);
}

TEST_F(LinkPicture, KeepsASmallerPictureOnAPs4AndHdrOnAPs5) {
  const auto preferences = picture(540, 30, 4000, MediaCodec::h265_hdr);
  const auto small =
      LinkPreferences::on_console(preferences, Generation::ps4).value();
  EXPECT_EQ(small.video.height(), 540u);
  EXPECT_EQ(small.video.fps(), 30u);
  EXPECT_EQ(small.video.codec(), LinkCodec::h264);
  const auto hdr =
      LinkPreferences::on_console(preferences, Generation::ps5).value();
  EXPECT_EQ(hdr.video.codec(), LinkCodec::h265_hdr);
  EXPECT_EQ(hdr.video.height(), 540u);
}

TEST_F(LinkPicture, FollowsTheCloudServiceForHeights) {
  const auto large = picture(2160, 60, 30000, MediaCodec::h265);
  const auto ps5 =
      LinkPreferences::on_cloud(large, ServiceGeneration::ps5_cloud).value();
  EXPECT_EQ(ps5.video.height(), 2160u);
  EXPECT_EQ(ps5.video.codec(), LinkCodec::h265);
  const auto old =
      LinkPreferences::on_cloud(large, ServiceGeneration::old_service).value();
  EXPECT_EQ(old.video.height(), 1080u);
  EXPECT_EQ(old.video.codec(), LinkCodec::h264);

  const auto tiny = picture(360, 30, 2000, MediaCodec::h264);
  EXPECT_EQ(LinkPreferences::on_cloud(tiny, ServiceGeneration::ps5_cloud)
                .value()
                .video.height(),
            720u);
  EXPECT_EQ(LinkPreferences::on_cloud(tiny, ServiceGeneration::old_service)
                .value()
                .video.height(),
            720u);
  EXPECT_EQ(LinkPreferences::on_cloud(tiny, ServiceGeneration::ps5_cloud)
                .value()
                .video.codec(),
            LinkCodec::h265);
}

}  // namespace
}  // namespace play_test
