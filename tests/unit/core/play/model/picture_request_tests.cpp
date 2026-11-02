#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>

namespace play_test {
namespace {

namespace catalog = libchiaking::catalog;
namespace consoles = libchiaking::consoles;
namespace play = libchiaking::play;
using catalog::ServiceGeneration;
using consoles::Generation;
using libchiaking::media::VideoCodec;
using play::CloudQuality;
using play::PictureRequest;

class PictureRequests : public ::testing::Test {
 protected:
  static auto picture(std::uint32_t height, VideoCodec codec = VideoCodec::h265,
                      std::uint32_t fps = 60, std::uint32_t bitrate = 15000)
      -> PictureRequest {
    const auto request = PictureRequest::create(height, fps, bitrate, codec);
    EXPECT_TRUE(request);
    return request ? *request : PictureRequest{};
  }

  static constexpr auto old = ServiceGeneration::old_service;
  static constexpr auto ps5 = ServiceGeneration::ps5_cloud;
};

TEST_F(PictureRequests, DefaultTo720pH264) {
  const PictureRequest standard;
  EXPECT_EQ(standard.height(), 720u);
  EXPECT_EQ(standard.width(), 1280u);
  EXPECT_EQ(standard.fps(), 60u);
  EXPECT_EQ(standard.bitrate_kbps(), 10000u);
  EXPECT_EQ(standard.codec(), VideoCodec::h264);
}

TEST_F(PictureRequests, AcceptOnlyTheListedHeights) {
  for (const auto height : PictureRequest::heights) {
    SCOPED_TRACE(height);
    EXPECT_TRUE(PictureRequest::create(height, 60, 15000, VideoCodec::h265));
  }
  for (const std::uint32_t height : {0U, 480U, 719U, 1081U, 4320U}) {
    SCOPED_TRACE(height);
    EXPECT_FALSE(PictureRequest::create(height, 60, 15000, VideoCodec::h265));
  }
}

TEST_F(PictureRequests, LimitH264To1080p) {
  EXPECT_TRUE(PictureRequest::create(1080, 60, 15000, VideoCodec::h264));
  EXPECT_FALSE(PictureRequest::create(1440, 60, 15000, VideoCodec::h264));
  EXPECT_FALSE(PictureRequest::create(2160, 60, 15000, VideoCodec::h264));
  EXPECT_TRUE(PictureRequest::create(2160, 60, 15000, VideoCodec::h265_hdr));
  EXPECT_FALSE(
      PictureRequest::create(1080, 60, 15000, static_cast<VideoCodec>(3)));
}

TEST_F(PictureRequests, AcceptOnly30Or60Fps) {
  EXPECT_TRUE(PictureRequest::create(720, 30, 15000, VideoCodec::h264));
  for (const std::uint32_t fps : {0U, 24U, 59U, 120U}) {
    SCOPED_TRACE(fps);
    EXPECT_FALSE(PictureRequest::create(720, fps, 15000, VideoCodec::h264));
  }
}

TEST_F(PictureRequests, BoundTheBitrate) {
  EXPECT_TRUE(PictureRequest::create(720, 60, 2000, VideoCodec::h264));
  EXPECT_TRUE(PictureRequest::create(720, 60, 100000, VideoCodec::h264));
  EXPECT_FALSE(PictureRequest::create(720, 60, 1999, VideoCodec::h264));
  EXPECT_FALSE(PictureRequest::create(720, 60, 100001, VideoCodec::h264));
}

TEST_F(PictureRequests, DeriveASixteenByNineWidthAndCompareByValue) {
  EXPECT_EQ(picture(360, VideoCodec::h264).width(), 640u);
  EXPECT_EQ(picture(2160).width(), 3840u);
  EXPECT_EQ(picture(1080), picture(1080));
  EXPECT_NE(picture(1080), picture(720));
}

TEST_F(PictureRequests, DefaultTo1080pOnTheCloud) {
  const auto cloud = PictureRequest::cloud_default();
  EXPECT_EQ(cloud.height(), 1080u);
  EXPECT_EQ(cloud.fps(), 60u);
  EXPECT_EQ(cloud.bitrate_kbps(), 15000u);
}

TEST_F(PictureRequests, ForceH264And720pOnAPs4) {
  for (const auto codec :
       {VideoCodec::h264, VideoCodec::h265, VideoCodec::h265_hdr}) {
    SCOPED_TRACE(testing::PrintToString(codec));
    const auto resolved =
        picture(1080, codec, 30, 30000).on_console(Generation::ps4);
    EXPECT_EQ(resolved.codec(), VideoCodec::h264);
    EXPECT_EQ(resolved.height(), 720u);
    EXPECT_EQ(resolved.width(), 1280u);
    EXPECT_EQ(resolved.fps(), 30u);
    EXPECT_EQ(resolved.bitrate_kbps(), 30000u);
  }
  EXPECT_EQ(picture(2160).on_console(Generation::ps4).height(),
            PictureRequest::ps4_height);
  EXPECT_EQ(picture(1440).on_console(Generation::ps4).height(), 720u);
  EXPECT_EQ(picture(720).on_console(Generation::ps4).height(), 720u);
  EXPECT_EQ(picture(540).on_console(Generation::ps4).height(), 540u);
  EXPECT_EQ(picture(360).on_console(Generation::ps4).height(), 360u);
  EXPECT_EQ(picture(360).on_console(Generation::ps4).codec(), VideoCodec::h264);
}

TEST_F(PictureRequests, GiveAPs5WhatWasAsked) {
  for (const auto height : PictureRequest::heights) {
    SCOPED_TRACE(height);
    const auto resolved = picture(height, VideoCodec::h265_hdr, 30, 42000)
                              .on_console(Generation::ps5);
    EXPECT_EQ(resolved.height(), height);
    EXPECT_EQ(resolved.codec(), VideoCodec::h265_hdr);
    EXPECT_EQ(resolved.fps(), 30u);
    EXPECT_EQ(resolved.bitrate_kbps(), 42000u);
    EXPECT_EQ(resolved.width(), height * 16 / 9);
  }
  EXPECT_EQ(picture(1080, VideoCodec::h264).on_console(Generation::ps5).codec(),
            VideoCodec::h264);
  EXPECT_EQ(picture(1080).on_console(Generation::ps5),
            picture(1080).on_console(Generation::ps5));
  EXPECT_NE(picture(1080).on_console(Generation::ps5),
            picture(1080).on_console(Generation::ps4));
}

TEST_F(PictureRequests, LimitCloudHeightsByService) {
  EXPECT_EQ(picture(360).on_cloud(old).height(), 720u);
  EXPECT_EQ(picture(540).on_cloud(old).height(), 720u);
  EXPECT_EQ(picture(720).on_cloud(old).height(), 720u);
  EXPECT_EQ(picture(1080).on_cloud(old).height(), 1080u);
  EXPECT_EQ(picture(1440).on_cloud(old).height(), 1080u);
  EXPECT_EQ(picture(2160).on_cloud(old).height(), 1080u);

  EXPECT_EQ(picture(360).on_cloud(ps5).height(), 720u);
  EXPECT_EQ(picture(540).on_cloud(ps5).height(), 720u);
  EXPECT_EQ(picture(720).on_cloud(ps5).height(), 720u);
  EXPECT_EQ(picture(1080).on_cloud(ps5).height(), 1080u);
  EXPECT_EQ(picture(1440).on_cloud(ps5).height(), 1440u);
  EXPECT_EQ(picture(2160).on_cloud(ps5).height(), 2160u);
  EXPECT_EQ(picture(2160).on_cloud(ps5).width(), 3840u);
}

TEST_F(PictureRequests, PickTheCloudCodecByServiceAndHeight) {
  EXPECT_EQ(picture(1080, VideoCodec::h264).on_cloud(ps5).codec(),
            VideoCodec::h265);
  EXPECT_EQ(picture(1080, VideoCodec::h265_hdr).on_cloud(ps5).codec(),
            VideoCodec::h265);
  EXPECT_EQ(picture(1440, VideoCodec::h265_hdr).on_cloud(ps5).codec(),
            VideoCodec::h265_hdr);
  EXPECT_EQ(picture(2160, VideoCodec::h265_hdr).on_cloud(ps5).codec(),
            VideoCodec::h265_hdr);
  EXPECT_EQ(picture(2160, VideoCodec::h265).on_cloud(ps5).codec(),
            VideoCodec::h265);
  EXPECT_EQ(picture(2160, VideoCodec::h265_hdr).on_cloud(old).codec(),
            VideoCodec::h264);
  EXPECT_EQ(picture(1080, VideoCodec::h265).on_cloud(old).codec(),
            VideoCodec::h264);
}

TEST_F(PictureRequests, MeasureBandwidthOnlyOnThePs5Cloud) {
  const auto measuring = picture(2160, VideoCodec::h265).measuring_bandwidth();
  EXPECT_TRUE(measuring.measures_bandwidth());
  EXPECT_FALSE(picture(2160, VideoCodec::h265).measures_bandwidth());
  EXPECT_NE(measuring, picture(2160, VideoCodec::h265));
  EXPECT_TRUE(measuring.on_cloud(ps5).measures_bandwidth());
  EXPECT_FALSE(measuring.on_cloud(old).measures_bandwidth());
  EXPECT_FALSE(measuring.on_console(Generation::ps5).measures_bandwidth());
}

TEST_F(PictureRequests, FavourStabilityOnlyOnThePs5Cloud) {
  const auto steady =
      picture(1080, VideoCodec::h265).favouring(CloudQuality::stable);
  EXPECT_EQ(picture(1080, VideoCodec::h265).cloud_quality(),
            CloudQuality::high);
  EXPECT_NE(steady, picture(1080, VideoCodec::h265));
  EXPECT_EQ(steady.on_cloud(ps5).cloud_quality(), CloudQuality::stable);
  EXPECT_EQ(steady.on_cloud(old).cloud_quality(), CloudQuality::high);
  EXPECT_EQ(steady.on_console(Generation::ps5).cloud_quality(),
            CloudQuality::high);
}

TEST_F(PictureRequests, KeepTheBitrateAndFrameRateOnTheCloud) {
  EXPECT_EQ(
      picture(1080, VideoCodec::h265, 30, 20000).on_cloud(old).bitrate_kbps(),
      20000u);
  EXPECT_EQ(picture(1080, VideoCodec::h265, 30, 20000).on_cloud(ps5).fps(),
            30u);
}

}  // namespace
}  // namespace play_test
