#include "src/core/psn/gk/launch_spec.hpp"

#include <gtest/gtest.h>

namespace cloud_test {
namespace {

namespace gk = libchiaking::psn::gk;

TEST(GkResolution, MapsEachSupportedHeightToItsSetting) {
  const auto hd = gk::Resolution::nearest(720);
  EXPECT_EQ(hd.setting(), "720");
  EXPECT_EQ(hd.width(), 1280);
  EXPECT_EQ(hd.height(), 720);
  const auto full = gk::Resolution::nearest(1080);
  EXPECT_EQ(full.setting(), "1080");
  EXPECT_EQ(full.width(), 1920);
  EXPECT_EQ(full.height(), 1080);
  const auto quad = gk::Resolution::nearest(1440);
  EXPECT_EQ(quad.setting(), "1440");
  EXPECT_EQ(quad.width(), 2560);
  EXPECT_EQ(quad.height(), 1440);
  const auto ultra = gk::Resolution::nearest(2160);
  EXPECT_EQ(ultra.setting(), "2160");
  EXPECT_EQ(ultra.width(), 3840);
  EXPECT_EQ(ultra.height(), 2160);
}

TEST(GkResolution, FallsBackToTenEightyForAnyOtherHeight) {
  const auto none = gk::Resolution::nearest(0);
  EXPECT_EQ(none.setting(), "1080");
  EXPECT_EQ(none.width(), 1920);
  EXPECT_EQ(none.height(), 1080);
  const auto negative = gk::Resolution::nearest(-1);
  EXPECT_EQ(negative.setting(), "1080");
  EXPECT_EQ(negative.width(), 1920);
  EXPECT_EQ(negative.height(), 1080);
  const auto huge = gk::Resolution::nearest(9999);
  EXPECT_EQ(huge.setting(), "1080");
  EXPECT_EQ(huge.width(), 1920);
  EXPECT_EQ(huge.height(), 1080);
}

}  // namespace
}  // namespace cloud_test
