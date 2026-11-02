#include "tests/unit/core/cloud/gk/spec_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cloud_test {
namespace {

using JsonTree = SpecRig::JsonTree;
using Texts = std::vector<std::string>;

class GkLegacyCloudStreamSpec : public ::testing::Test {
 protected:
  static constexpr std::string_view expected_head =
      R"({"entitlementId":"synthetic-entitlement","npEnv":"np","language":"en","cloudEndpoint":"https://cc.prod.gaikai.com","redirectUri":"https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/grc-response.html","resolutionSetting":"1080","clientWidth":1920,"clientHeight":1080,"adaptiveStreamMode":"resize","useClientBwLadder":true,"audioUploadEnabled":true,"audioUploadNumChannels":1,"audioUploadSamplingFrequency":48000,"acceptButton":"X","encryptionSupported":true,"summerTime":0,"timeZone":"UTC+09:00","httpUserAgent":"Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 Safari/537.36 gkApollo","gkCloudAuthCode":"","accessibilityMarqueeSpeed":0,"accessibilityLargeText":0,"accessibilityBoldText":0,"accessibilityContrast":0,"accessibilityTtsEnable":0,"accessibilityTtsSpeed":3,"accessibilityTtsVolume":12,"partyCapability":false,"homesharing":false,"isFirstBoot":false,"isPlusMember":true,"parentalLevel":0,"yuvCoefficient":"","audioChannels":"2.1","audioEncoderProfile":"default","videoEncoderProfile":"hw4.1","connectedControllers":["xinput"],"input":{"controllers":["xinput"]},"model":"WINDOWS","platform":"PC","gaikaiPlayer":"12.5.0","protocolVersion":)";
  static constexpr std::string_view expected_tail =
      R"(,"ps3AuthCode":"","streamServerAuthCode":"","capabilities":["cloudDrivenSenkushaTest","kratos"]})";
  static constexpr std::array<std::string_view, 44> fields{
      "entitlementId",
      "npEnv",
      "language",
      "cloudEndpoint",
      "redirectUri",
      "resolutionSetting",
      "clientWidth",
      "clientHeight",
      "adaptiveStreamMode",
      "useClientBwLadder",
      "audioUploadEnabled",
      "audioUploadNumChannels",
      "audioUploadSamplingFrequency",
      "acceptButton",
      "encryptionSupported",
      "summerTime",
      "timeZone",
      "httpUserAgent",
      "gkCloudAuthCode",
      "accessibilityMarqueeSpeed",
      "accessibilityLargeText",
      "accessibilityBoldText",
      "accessibilityContrast",
      "accessibilityTtsEnable",
      "accessibilityTtsSpeed",
      "accessibilityTtsVolume",
      "partyCapability",
      "homesharing",
      "isFirstBoot",
      "isPlusMember",
      "parentalLevel",
      "yuvCoefficient",
      "audioChannels",
      "audioEncoderProfile",
      "videoEncoderProfile",
      "connectedControllers",
      "input",
      "model",
      "platform",
      "gaikaiPlayer",
      "protocolVersion",
      "ps3AuthCode",
      "streamServerAuthCode",
      "capabilities"};

  void SetUp() override {
    auto made = make();
    ASSERT_TRUE(made);
    encoded = std::move(made.value());
    auto parsed = SpecRig::object(encoded);
    ASSERT_TRUE(parsed);
    root = std::move(*parsed);
  }

  static auto make(std::string_view entitlement = "synthetic-entitlement",
                   std::string_view zone = "UTC+09:00", int resolution = 1080)
      -> SpecRig::Json {
    return SpecRig::json_of(gk::LaunchSpec::legacy_cloud_stream(
        entitlement, gk::Language{"en-US"},
        gk::TimeZone{zone, SpecRig::local_clock}, resolution));
  }

  std::string encoded;
  SpecRig::Tree root;
};

TEST_F(GkLegacyCloudStreamSpec, WritesTheDesktopAppSpecByteForByte) {
  EXPECT_EQ(encoded, std::string{expected_head} +
                         std::to_string(SpecRig::legacy_version) +
                         std::string{expected_tail});
}

TEST_F(GkLegacyCloudStreamSpec, ListsEveryFieldInOrder) {
  EXPECT_TRUE(SpecRig::has_keys(root, fields));
}

TEST_F(GkLegacyCloudStreamSpec, NamesTheDesktopAppEndpointsAndProfiles) {
  EXPECT_EQ(JsonTree::text_at(root, "redirectUri"),
            "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
            "grc-response.html");
  EXPECT_EQ(JsonTree::text_at(root, "httpUserAgent"),
            "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, "
            "like "
            "Gecko) playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 "
            "Safari/537.36 gkApollo");
  EXPECT_EQ(JsonTree::text_at(root, "audioChannels"), "2.1");
  EXPECT_EQ(JsonTree::text_at(root, "videoEncoderProfile"), "hw4.1");
  EXPECT_EQ(JsonTree::integer_at(root, "protocolVersion"),
            SpecRig::legacy_version);
}

TEST_F(GkLegacyCloudStreamSpec, HasNoStreamSettings) {
  EXPECT_EQ(JsonTree::member(root, "videoStreamSettings"), nullptr);
  EXPECT_EQ(JsonTree::member(root, "audioStreamSettings"), nullptr);
}

TEST_F(GkLegacyCloudStreamSpec, ListsOneControllerAndTheKratosCapability) {
  const auto* controllers = JsonTree::member(root, "connectedControllers");
  ASSERT_NE(controllers, nullptr);
  ASSERT_EQ(controllers->size(), std::size_t{1});
  EXPECT_EQ(JsonTree::text((*controllers)[0]), "xinput");
  const auto* caps = JsonTree::member(root, "capabilities");
  ASSERT_NE(caps, nullptr);
  ASSERT_EQ(caps->size(), std::size_t{2});
  EXPECT_EQ(JsonTree::text((*caps)[1]), "kratos");
}

TEST_F(GkLegacyCloudStreamSpec, EscapesTheEntitlement) {
  const auto quoted = make("e\"\\\n");
  ASSERT_TRUE(quoted);
  EXPECT_NE(quoted.value().find("\"entitlementId\":\"e\\\"\\\\\\n\""),
            std::string::npos);
}

TEST_F(GkLegacyCloudStreamSpec, RefusesAnEmptyOversizedOrNulEntitlement) {
  EXPECT_FALSE(make(""));
  EXPECT_FALSE(make(std::string(4097, 'x')));
  EXPECT_FALSE(make(std::string_view{"a\0b", 3}));
}

TEST_F(GkLegacyCloudStreamSpec, GrowsPastSixteenKilobytesWhenEscaped) {
  const auto escaped = make(std::string(4096, '\x01'));
  ASSERT_TRUE(escaped);
  EXPECT_GT(escaped.value().size(), std::size_t{16 * 1024});
}

TEST_F(GkLegacyCloudStreamSpec, AcceptsAnyZoneName) {
  const auto lenient_zone = make("synthetic-entitlement", "UTC-0330");
  ASSERT_TRUE(lenient_zone);
  EXPECT_NE(lenient_zone.value().find("\"timeZone\":\"UTC-03:30\""),
            std::string::npos);
  EXPECT_TRUE(make("synthetic-entitlement", "UTC+24:00"));
  EXPECT_TRUE(make("synthetic-entitlement", ""));
}

}  // namespace
}  // namespace cloud_test
