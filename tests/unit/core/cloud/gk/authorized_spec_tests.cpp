#include "src/core/psn/gk/launch_spec.hpp"
#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace cloud_test {
namespace {

using Invalid = gk::LaunchSpec::Invalid;

class GkAuthorizedSpec : public ::testing::Test {
 protected:
  static auto filled(std::string json, std::string_view field,
                     std::string_view code) -> std::string {
    const auto empty = std::string{"\""} + std::string{field} + "\":\"\"";
    const auto at = json.find(empty);
    EXPECT_NE(at, std::string::npos) << field;
    if (at == std::string::npos) return json;
    return json.replace(at, empty.size(),
                        std::string{"\""} + std::string{field} + "\":\"" +
                            std::string{code} + "\"");
  }

  const gk::TimeZone zone = gk::TimeZone::parse("UTC+00:00").value();
  const gk::LaunchSpec cloud_stream =
      gk::LaunchSpec::cloud_stream("synthetic-entitlement",
                                   gk::Language{"en-US"}, zone, 1080)
          .value();
  const gk::LaunchSpec legacy =
      gk::LaunchSpec::legacy_cloud_stream("synthetic-entitlement",
                                          gk::Language{"en-US"}, zone, 720)
          .value();
  const libchiaking::psn::auth::GkCodes codes =
      GkRig::codes("CLIENT", "SERVER");
};

TEST_F(GkAuthorizedSpec, FillsTheClientAndStreamServerCodesForCloudStream) {
  const auto spec = cloud_stream.authorized(codes);
  ASSERT_TRUE(spec);
  EXPECT_EQ(spec.value().view(), filled(filled(std::string{cloud_stream.json()},
                                               "gkCloudAuthCode", "CLIENT"),
                                        "streamServerAuthCode", "SERVER"));
}

TEST_F(GkAuthorizedSpec, FillsThePs3CodeWithTheServerCodeForLegacy) {
  const auto spec = legacy.authorized(codes);
  ASSERT_TRUE(spec);
  EXPECT_EQ(spec.value().view(),
            filled(filled(filled(std::string{legacy.json()}, "gkCloudAuthCode",
                                 "CLIENT"),
                          "ps3AuthCode", "SERVER"),
                   "streamServerAuthCode", "SERVER"));
}

TEST_F(GkAuthorizedSpec, RefusesASpecOfAnotherShape) {
  const auto empty = gk::LaunchSpec::prebuilt(CloudService::cloud_stream, "{}")
                         .authorized(codes);
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), Invalid::unexpected_shape);
  const auto doubled =
      gk::LaunchSpec::prebuilt(
          CloudService::legacy_cloud_stream,
          std::string{legacy.json()} + std::string{legacy.json()})
          .authorized(codes);
  ASSERT_FALSE(doubled);
  EXPECT_EQ(doubled.error(), Invalid::unexpected_shape);
}

TEST_F(GkAuthorizedSpec, AcceptsALongCodeButNotOneOverTheBodyLimit) {
  EXPECT_TRUE(
      legacy.authorized(GkRig::codes("CLIENT", std::string(8 * 1024, 'x'))));
  const auto oversize = legacy.authorized(
      GkRig::codes("CLIENT", std::string(gk::Client::body_limit, 'x')));
  ASSERT_FALSE(oversize);
  EXPECT_EQ(oversize.error(), Invalid::oversize);
}

}  // namespace
}  // namespace cloud_test
