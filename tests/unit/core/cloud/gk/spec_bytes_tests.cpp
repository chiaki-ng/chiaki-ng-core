#include "tests/unit/core/cloud/gk/spec_rig.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string_view>
#include <utility>

namespace cloud_test {
namespace {

class GkSpecBytes : public ::testing::Test {
 protected:
  void SetUp() override {
    auto made = gk::LaunchSpec::cloud_stream("synthetic\x01\x1f\"entitlement",
                                             gk::Language{"en-US"}, zone, 1080);
    ASSERT_TRUE(made);
    cloud_stream.emplace(std::move(made.value()));
  }

  const gk::TimeZone zone = gk::TimeZone::parse("UTC+09:00").value();
  std::optional<gk::LaunchSpec> cloud_stream;
};

TEST_F(GkSpecBytes, WritesTheCloudStreamSpecByteForByte) {
  EXPECT_EQ(
      cloud_stream->json(),
      std::string_view{"{\"entitlementId\":"
                       "\"synthetic\\u0001\\u001F\\\"entitlement\",\"npEnv\""
                       ":\"np\",\"language\":\"en\",\"cloudEndpoint\":\"https:"
                       "//cc.prod.gaikai."
                       "com\",\"redirectUri\":\"gaikai://"
                       "local\",\"resolutionSetting\":\"1080\""
                       ",\"clientWidth\":1920,\"clientHeight\":1080,"
                       "\"adaptiveStreamMode\":\"re"
                       "size\",\"useClientBwLadder\":true,"
                       "\"audioUploadEnabled\":true,\"audioUp"
                       "loadNumChannels\":1,\"audioUploadSamplingFrequency\":"
                       "48000,\"acceptButt"
                       "on\":\"X\",\"encryptionSupported\":true,"
                       "\"summerTime\":0,\"timeZone\":\""
                       "UTC+09:00\",\"httpUserAgent\":\"PlayStation "
                       "Portal/6.0.0-rel.444+6a9cea"
                       "6f5\",\"gkCloudAuthCode\":\"\","
                       "\"accessibilityMarqueeSpeed\":0,\"access"
                       "ibilityLargeText\":0,\"accessibilityBoldText\":0,"
                       "\"accessibilityContras"
                       "t\":0,\"accessibilityTtsEnable\":0,"
                       "\"accessibilityTtsSpeed\":3,\"access"
                       "ibilityTtsVolume\":12,\"partyCapability\":false,"
                       "\"homesharing\":false,\""
                       "isFirstBoot\":false,\"isPlusMember\":true,"
                       "\"parentalLevel\":0,\"yuvCoef"
                       "ficient\":\"\",\"videoEncoderProfile\":\"hw5.0\","
                       "\"connectedControllers"
                       "\":[\"ds4\",\"ds5\",\"xinput\"],\"input\":{"
                       "\"controllers\":[\"ds4\",\"d"
                       "s5\",\"xinput\"]},\"model\":\"portal\",\"platform\":"
                       "\"qlite\",\"gaikaiP"
                       "layer\":\"16.4.0\",\"protocolVersion\":23,"
                       "\"ps3AuthCode\":\"\",\"stream"
                       "ServerAuthCode\":\"\",\"videoStreamSettings\":{"
                       "\"clientHeight\":1080,\""
                       "supportedMaxResolution\":1080,"
                       "\"supportedVideoEncoderProfiles\":[\"hevc"
                       "_hw4\"],\"supportedDynamicRange\":\"sdr\","
                       "\"preferredMaxResolution\":10"
                       "80,\"preferredDynamicRange\":\"sdr\",\"hqMode\":1},"
                       "\"audioChannels\":\""
                       "7.1.4\",\"audioEncoderProfile\":\"default\","
                       "\"audioStreamSettings\":{\""
                       "audioEncoderProfile\":\"default\","
                       "\"maxAudioChannels\":\"7.1.4\",\"pref"
                       "erredNumberAudioChannels\":\"7.1.4\"},"
                       "\"capabilities\":[\"cloudDrivenSe"
                       "nkushaTest\",\"cronos\"]}"});
}

TEST_F(GkSpecBytes, WritesTheHdrSpecByteForByte) {
  const auto hdr = gk::LaunchSpec::cloud_stream("ent", gk::Language{"en-US"},
                                                zone, 2160, 23, 6, true);
  ASSERT_TRUE(hdr);
  EXPECT_EQ(
      hdr.value().json(),
      std::string_view{
          "{\"entitlementId\":\"ent\",\"npEnv\":\"np\",\"language\":\"en\","
          "\"cloud"
          "Endpoint\":\"https://"
          "cc.prod.gaikai.com\",\"redirectUri\":\"gaikai://lo"
          "cal\",\"resolutionSetting\":\"2160\",\"clientWidth\":3840,"
          "\"clientHeigh"
          "t\":2160,\"adaptiveStreamMode\":\"resize\",\"useClientBwLadder\":"
          "true,\""
          "audioUploadEnabled\":true,\"audioUploadNumChannels\":1,"
          "\"audioUploadSam"
          "plingFrequency\":48000,\"acceptButton\":\"X\","
          "\"encryptionSupported\":t"
          "rue,\"summerTime\":0,\"timeZone\":\"UTC+09:00\","
          "\"httpUserAgent\":\"Pla"
          "yStation "
          "Portal/6.0.0-rel.444+6a9cea6f5\",\"gkCloudAuthCode\":\"\",\"ac"
          "cessibilityMarqueeSpeed\":0,\"accessibilityLargeText\":0,"
          "\"accessibilit"
          "yBoldText\":0,\"accessibilityContrast\":0,"
          "\"accessibilityTtsEnable\":0,"
          "\"accessibilityTtsSpeed\":3,\"accessibilityTtsVolume\":12,"
          "\"partyCapabil"
          "ity\":false,\"homesharing\":false,\"isFirstBoot\":false,"
          "\"isPlusMember\""
          ":true,\"parentalLevel\":0,\"yuvCoefficient\":\"\","
          "\"videoEncoderProfile"
          "\":\"hw5.0\",\"connectedControllers\":[\"ds4\",\"ds5\","
          "\"xinput\"],\"in"
          "put\":{\"controllers\":[\"ds4\",\"ds5\",\"xinput\"]},\"model\":"
          "\"portal"
          "\",\"platform\":\"qlite\",\"gaikaiPlayer\":\"16.4.0\","
          "\"protocolVersion"
          "\":23,\"ps3AuthCode\":\"\",\"streamServerAuthCode\":\"\","
          "\"videoCodec\""
          ":\"hevc\",\"dynamicRange\":\"HDR\",\"videoStreamSettings\":{"
          "\"clientHei"
          "ght\":2160,\"supportedMaxResolution\":2160,"
          "\"supportedVideoEncoderProfi"
          "les\":[\"hevc_hw4\"],\"supportedDynamicRange\":\"HDR\","
          "\"preferredMaxRe"
          "solution\":2160,\"preferredDynamicRange\":\"HDR\",\"hqMode\":1},"
          "\"audio"
          "Channels\":\"5.1\",\"audioEncoderProfile\":\"default\","
          "\"audioStreamSet"
          "tings\":{\"audioEncoderProfile\":\"default\","
          "\"maxAudioChannels\":\"7.1"
          ".4\",\"preferredNumberAudioChannels\":\"5.1\"},\"capabilities\":["
          "\"clou"
          "dDrivenSenkushaTest\",\"cronos\"]}"});
}

TEST_F(GkSpecBytes, WritesTheLegacyCloudStreamSpecByteForByte) {
  const auto legacy = gk::LaunchSpec::legacy_cloud_stream(
      "ent\xFF", gk::Language{"ja-JP"}, zone, 720);
  ASSERT_TRUE(legacy);
  EXPECT_EQ(
      legacy.value().json(),
      std::string_view{
          "{\"entitlementId\":\"ent\xEF"
          "\xBF"
          "\xBD"
          "\",\"npEnv\":\"np\",\"languag"
          "e\":\"ja\",\"cloudEndpoint\":\"https://"
          "cc.prod.gaikai.com\",\"redirectU"
          "ri\":\"https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
          "grc-respon"
          "se.html\",\"resolutionSetting\":\"720\",\"clientWidth\":1280,"
          "\"clientHe"
          "ight\":720,\"adaptiveStreamMode\":\"resize\",\"useClientBwLadder\":"
          "true"
          ",\"audioUploadEnabled\":true,\"audioUploadNumChannels\":1,"
          "\"audioUpload"
          "SamplingFrequency\":48000,\"acceptButton\":\"X\","
          "\"encryptionSupported\""
          ":true,\"summerTime\":0,\"timeZone\":\"UTC+09:00\",\"httpUserAgent\":"
          "\"M"
          "ozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like "
          "Gec"
          "ko) playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 "
          "Safari/53"
          "7.36 "
          "gkApollo\",\"gkCloudAuthCode\":\"\",\"accessibilityMarqueeSpeed\":"
          "0,\"accessibilityLargeText\":0,\"accessibilityBoldText\":0,"
          "\"accessibil"
          "ityContrast\":0,\"accessibilityTtsEnable\":0,"
          "\"accessibilityTtsSpeed\":"
          "3,\"accessibilityTtsVolume\":12,\"partyCapability\":false,"
          "\"homesharing\""
          ":false,\"isFirstBoot\":false,\"isPlusMember\":true,"
          "\"parentalLevel\":0,"
          "\"yuvCoefficient\":\"\",\"audioChannels\":\"2.1\","
          "\"audioEncoderProfile"
          "\":\"default\",\"videoEncoderProfile\":\"hw4.1\","
          "\"connectedControllers"
          "\":[\"xinput\"],\"input\":{\"controllers\":[\"xinput\"]},\"model\":"
          "\"WI"
          "NDOWS\",\"platform\":\"PC\",\"gaikaiPlayer\":\"12.5.0\","
          "\"protocolVersi"
          "on\":12,\"ps3AuthCode\":\"\",\"streamServerAuthCode\":\"\","
          "\"capabiliti"
          "es\":[\"cloudDrivenSenkushaTest\",\"kratos\"]}"});
}

TEST_F(GkSpecBytes, WritesTheSignedSpecByteForByte) {
  const auto authorized =
      cloud_stream->authorized(GkRig::codes("g\x01k", "s\"v"));
  ASSERT_TRUE(authorized);
  EXPECT_EQ(
      authorized.value().view(),
      std::string_view{
          "{\"entitlementId\":\"synthetic\\u0001\\u001F\\\"entitlement\","
          "\"npEnv\""
          ":\"np\",\"language\":\"en\",\"cloudEndpoint\":\"https://"
          "cc.prod.gaikai."
          "com\",\"redirectUri\":\"gaikai://"
          "local\",\"resolutionSetting\":\"1080\""
          ",\"clientWidth\":1920,\"clientHeight\":1080,"
          "\"adaptiveStreamMode\":\"re"
          "size\",\"useClientBwLadder\":true,\"audioUploadEnabled\":true,"
          "\"audioUp"
          "loadNumChannels\":1,\"audioUploadSamplingFrequency\":48000,"
          "\"acceptButt"
          "on\":\"X\",\"encryptionSupported\":true,\"summerTime\":0,"
          "\"timeZone\":\""
          "UTC+09:00\",\"httpUserAgent\":\"PlayStation "
          "Portal/6.0.0-rel.444+6a9cea"
          "6f5\",\"gkCloudAuthCode\":\"g\\u0001k\","
          "\"accessibilityMarqueeSpeed\":0"
          ",\"accessibilityLargeText\":0,\"accessibilityBoldText\":0,"
          "\"accessibili"
          "tyContrast\":0,\"accessibilityTtsEnable\":0,"
          "\"accessibilityTtsSpeed\":3"
          ",\"accessibilityTtsVolume\":12,\"partyCapability\":false,"
          "\"homesharing\""
          ":false,\"isFirstBoot\":false,\"isPlusMember\":true,"
          "\"parentalLevel\":0,"
          "\"yuvCoefficient\":\"\",\"videoEncoderProfile\":\"hw5.0\","
          "\"connectedCo"
          "ntrollers\":[\"ds4\",\"ds5\",\"xinput\"],\"input\":{"
          "\"controllers\":[\""
          "ds4\",\"ds5\",\"xinput\"]},\"model\":\"portal\",\"platform\":"
          "\"qlite\","
          "\"gaikaiPlayer\":\"16.4.0\",\"protocolVersion\":23,"
          "\"ps3AuthCode\":\"\""
          ",\"streamServerAuthCode\":\"s\\\"v\",\"videoStreamSettings\":{"
          "\"clientH"
          "eight\":1080,\"supportedMaxResolution\":1080,"
          "\"supportedVideoEncoderPro"
          "files\":[\"hevc_hw4\"],\"supportedDynamicRange\":\"sdr\","
          "\"preferredMax"
          "Resolution\":1080,\"preferredDynamicRange\":\"sdr\",\"hqMode\":1}"
          ",\"aud"
          "ioChannels\":\"7.1.4\",\"audioEncoderProfile\":\"default\","
          "\"audioStrea"
          "mSettings\":{\"audioEncoderProfile\":\"default\","
          "\"maxAudioChannels\":\""
          "7.1.4\",\"preferredNumberAudioChannels\":\"7.1.4\"},"
          "\"capabilities\":[\""
          "cloudDrivenSenkushaTest\",\"cronos\"]}"});
}

}  // namespace
}  // namespace cloud_test
