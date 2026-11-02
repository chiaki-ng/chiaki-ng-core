#include "tests/unit/core/cloud/gk/spec_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cloud_test {
namespace {

using JsonTree = SpecRig::JsonTree;
using Invalid = SpecRig::Invalid;
using Texts = std::vector<std::string>;

class GkCloudStreamSpec : public ::testing::Test {
 protected:
  static constexpr std::array<std::string_view, 46> fields{
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
      "videoEncoderProfile",
      "connectedControllers",
      "input",
      "model",
      "platform",
      "gaikaiPlayer",
      "protocolVersion",
      "ps3AuthCode",
      "streamServerAuthCode",
      "videoStreamSettings",
      "audioChannels",
      "audioEncoderProfile",
      "audioStreamSettings",
      "capabilities"};
  static constexpr std::array<std::string_view, 7> video_fields{
      "clientHeight",
      "supportedMaxResolution",
      "supportedVideoEncoderProfiles",
      "supportedDynamicRange",
      "preferredMaxResolution",
      "preferredDynamicRange",
      "hqMode"};
  static constexpr std::array<std::string_view, 26> fixed_snippets{
      "\"npEnv\":\"np\"",
      "\"adaptiveStreamMode\":\"resize\"",
      "\"useClientBwLadder\":true",
      "\"audioUploadEnabled\":true",
      "\"audioUploadNumChannels\":1",
      "\"audioUploadSamplingFrequency\":48000",
      "\"acceptButton\":\"X\"",
      "\"encryptionSupported\":true",
      "\"summerTime\":0",
      "\"gkCloudAuthCode\":\"\"",
      "\"partyCapability\":false",
      "\"homesharing\":false",
      "\"isFirstBoot\":false",
      "\"isPlusMember\":true",
      "\"parentalLevel\":0",
      "\"yuvCoefficient\":\"\"",
      "\"videoEncoderProfile\":\"hw5.0\"",
      "\"model\":\"portal\"",
      "\"platform\":\"qlite\"",
      "\"gaikaiPlayer\":\"16.4.0\"",
      "\"ps3AuthCode\":\"\"",
      "\"streamServerAuthCode\":\"\"",
      "\"audioChannels\":\"7.1.4\"",
      "\"audioEncoderProfile\":\"default\"",
      "\"maxAudioChannels\":\"7.1.4\"",
      "\"preferredNumberAudioChannels\":\"7.1.4\""};

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
    return SpecRig::json_of(gk::LaunchSpec::cloud_stream(
        entitlement, gk::Language{"en-US"},
        gk::TimeZone{zone, SpecRig::local_clock}, resolution));
  }

  static auto with_audio(std::uint64_t channels, std::uint64_t ceiling = 0)
      -> libchiaking::Expected<gk::LaunchSpec, Invalid> {
    return gk::LaunchSpec::cloud_stream(
        "synthetic-entitlement", gk::Language{"en-US"},
        gk::TimeZone::parse("UTC+09:00").value(), 1080,
        SpecRig::cloud_stream_version, channels, false, {}, true, ceiling);
  }

  static auto sends_every_fixed_field(std::string_view json)
      -> testing::AssertionResult {
    for (const auto snippet : fixed_snippets)
      if (json.find(snippet) == std::string_view::npos)
        return testing::AssertionFailure() << "missing " << snippet;
    return testing::AssertionSuccess();
  }

  static auto prefers(std::uint64_t channels, std::string_view layout)
      -> testing::AssertionResult {
    const auto choice = with_audio(channels);
    if (!choice) return testing::AssertionFailure() << "the spec was refused";
    const auto decoded = JsonTree::parse(choice.value().json());
    if (!decoded) return testing::AssertionFailure() << "the spec is not JSON";
    const auto* settings =
        JsonTree::member(decoded.value(), "audioStreamSettings");
    if (JsonTree::member(decoded.value(), "audioChannels") == nullptr ||
        settings == nullptr)
      return testing::AssertionFailure() << "the audio fields are missing";
    if (JsonTree::text_at(decoded.value(), "audioChannels") != layout)
      return testing::AssertionFailure()
             << "audioChannels is "
             << JsonTree::text_at(decoded.value(), "audioChannels");
    if (JsonTree::member(*settings, "preferredNumberAudioChannels") ==
            nullptr ||
        JsonTree::text_at(*settings, "preferredNumberAudioChannels") != layout)
      return testing::AssertionFailure() << "the preferred layout differs";
    if (JsonTree::text_at(*settings, "maxAudioChannels") != "7.1.4")
      return testing::AssertionFailure() << "the maximum layout differs";
    if (JsonTree::text_at(*settings, "audioEncoderProfile") != "default")
      return testing::AssertionFailure() << "the encoder profile differs";
    const auto expected_settings =
        std::string{
            R"("audioStreamSettings":{"audioEncoderProfile":"default","maxAudioChannels":"7.1.4","preferredNumberAudioChannels":")"} +
        std::string{layout} + R"("})";
    if (choice.value().json().find(expected_settings) == std::string_view::npos)
      return testing::AssertionFailure() << "the settings are not in order";
    return testing::AssertionSuccess();
  }

  static auto caps_at(std::uint64_t ceiling, std::string_view layout)
      -> testing::AssertionResult {
    const auto capped = with_audio(2, ceiling);
    if (!capped) return testing::AssertionFailure() << "the spec was refused";
    const auto decoded = JsonTree::parse(capped.value().json());
    if (!decoded) return testing::AssertionFailure() << "the spec is not JSON";
    const auto* settings =
        JsonTree::member(decoded.value(), "audioStreamSettings");
    if (settings == nullptr)
      return testing::AssertionFailure() << "the audio settings are missing";
    if (JsonTree::text_at(*settings, "maxAudioChannels") != layout)
      return testing::AssertionFailure()
             << "the maximum is "
             << JsonTree::text_at(*settings, "maxAudioChannels");
    if (JsonTree::text_at(*settings, "preferredNumberAudioChannels") != "2")
      return testing::AssertionFailure() << "the preferred layout differs";
    return testing::AssertionSuccess();
  }

  static auto refuses_channels(std::uint64_t channels)
      -> testing::AssertionResult {
    const auto invalid = with_audio(channels);
    if (invalid)
      return testing::AssertionFailure() << channels << " channels were taken";
    if (invalid.error() != Invalid::audio_channels)
      return testing::AssertionFailure()
             << channels << " channels failed for another reason";
    return testing::AssertionSuccess();
  }

  static auto refuses_every_other_channel_count() -> testing::AssertionResult {
    for (std::uint64_t channels = 0; channels < 256; ++channels) {
      if (channels == 0 || channels == 2 || channels == 6 || channels == 8 ||
          channels == 12)
        continue;
      if (auto refused = refuses_channels(channels); !refused) return refused;
    }
    return testing::AssertionSuccess();
  }

  static auto maps_resolution(int resolution) -> testing::AssertionResult {
    const auto result = make("synthetic-entitlement", "UTC+09:00", resolution);
    if (!result) return testing::AssertionFailure() << "the spec was refused";
    const auto expected = gk::Resolution::nearest(resolution);
    if (result.value().find("\"resolutionSetting\":\"" +
                            std::string(expected.setting()) + "\"") ==
        std::string::npos)
      return testing::AssertionFailure() << "the setting differs";
    if (result.value().find("\"clientWidth\":" +
                            std::to_string(expected.width())) ==
        std::string::npos)
      return testing::AssertionFailure() << "the width differs";
    if (result.value().find("\"clientHeight\":" +
                            std::to_string(expected.height())) ==
        std::string::npos)
      return testing::AssertionFailure() << "the height differs";
    return testing::AssertionSuccess();
  }

  std::string encoded;
  SpecRig::Tree root;
};

TEST_F(GkCloudStreamSpec, KeepsTheLanguageSubtagOfJapanese) {
  EXPECT_EQ(gk::Language{"ja-JP"}.view(), "ja");
}

TEST_F(GkCloudStreamSpec, ListsEveryFieldInOrder) {
  EXPECT_TRUE(SpecRig::has_keys(root, fields));
}

TEST_F(GkCloudStreamSpec, NamesTheEntitlementLanguageAndEndpoints) {
  EXPECT_EQ(JsonTree::text_at(root, "entitlementId"), "synthetic-entitlement");
  EXPECT_EQ(JsonTree::text_at(root, "language"), "en");
  EXPECT_EQ(JsonTree::text_at(root, "cloudEndpoint"),
            "https://cc.prod.gaikai.com");
  EXPECT_EQ(JsonTree::text_at(root, "redirectUri"), "gaikai://local");
  EXPECT_EQ(JsonTree::text_at(root, "httpUserAgent"),
            "PlayStation Portal/6.0.0-rel.444+6a9cea6f5");
}

TEST_F(GkCloudStreamSpec, ListsTheCloudStreamControllers) {
  EXPECT_EQ(SpecRig::texts_of(JsonTree::member(root, "connectedControllers")),
            (Texts{"ds4", "ds5", "xinput"}));
  const auto* input = JsonTree::member(root, "input");
  ASSERT_NE(input, nullptr);
  ASSERT_TRUE(input->is_object());
  EXPECT_NE(JsonTree::member(*input, "controllers"), nullptr);
}

TEST_F(GkCloudStreamSpec, DescribesTheVideoStream) {
  const auto* video = JsonTree::member(root, "videoStreamSettings");
  ASSERT_NE(video, nullptr);
  ASSERT_TRUE(video->is_object());
  EXPECT_TRUE(SpecRig::has_keys(*video, video_fields));
  EXPECT_EQ(JsonTree::integer_at(*video, "supportedMaxResolution"), 1080);
  EXPECT_EQ(JsonTree::integer_at(*video, "preferredMaxResolution"), 1080);
  EXPECT_EQ(JsonTree::text_at(*video, "supportedDynamicRange"), "sdr");
  EXPECT_EQ(JsonTree::text_at(*video, "preferredDynamicRange"), "sdr");
  EXPECT_EQ(JsonTree::integer_at(*video, "hqMode"), 1);
  EXPECT_EQ(SpecRig::texts_of(
                JsonTree::member(*video, "supportedVideoEncoderProfiles")),
            Texts{"hevc_hw4"});
}

TEST_F(GkCloudStreamSpec, DescribesTheAudioStream) {
  const auto* audio = JsonTree::member(root, "audioStreamSettings");
  ASSERT_NE(audio, nullptr);
  ASSERT_TRUE(audio->is_object());
  constexpr std::array<std::string_view, 3> audio_fields{
      "audioEncoderProfile", "maxAudioChannels",
      "preferredNumberAudioChannels"};
  EXPECT_TRUE(SpecRig::has_keys(*audio, audio_fields));
  EXPECT_EQ(JsonTree::text_at(*audio, "audioEncoderProfile"), "default");
  EXPECT_EQ(JsonTree::text_at(*audio, "maxAudioChannels"), "7.1.4");
  EXPECT_EQ(JsonTree::text_at(*audio, "preferredNumberAudioChannels"), "7.1.4");
  EXPECT_EQ(JsonTree::text_at(root, "audioChannels"), "7.1.4");
}

TEST_F(GkCloudStreamSpec, ListsItsCapabilities) {
  EXPECT_EQ(SpecRig::texts_of(JsonTree::member(root, "capabilities")),
            (Texts{"cloudDrivenSenkushaTest", "cronos"}));
}

TEST_F(GkCloudStreamSpec, SendsTheFixedCloudStreamFields) {
  EXPECT_TRUE(sends_every_fixed_field(encoded));
}

TEST_F(GkCloudStreamSpec, PinsTheCloudStreamProtocolVersion) {
  const auto pinned =
      "\"protocolVersion\":" + std::to_string(SpecRig::cloud_stream_version) +
      ",";
  EXPECT_NE(encoded.find(pinned), std::string::npos);
}

TEST_F(GkCloudStreamSpec, SendsAChosenProtocolVersion) {
  const auto pinned_20 = SpecRig::json_of(gk::LaunchSpec::cloud_stream(
      "synthetic-entitlement", gk::Language{"en-US"},
      gk::TimeZone::parse("UTC+09:00").value(), 1080, 20));
  ASSERT_TRUE(pinned_20);
  EXPECT_NE(pinned_20.value().find("\"protocolVersion\":20,"),
            std::string::npos);
}

TEST_F(GkCloudStreamSpec, PrefersTheRequestedAudioLayout) {
  EXPECT_TRUE(prefers(0, "7.1.4"));
  EXPECT_TRUE(prefers(2, "2"));
  EXPECT_TRUE(prefers(6, "5.1"));
  EXPECT_TRUE(prefers(8, "7.1"));
  EXPECT_TRUE(prefers(12, "7.1.4"));
}

TEST_F(GkCloudStreamSpec, CapsTheAudioLayoutAtTheCeiling) {
  EXPECT_TRUE(caps_at(0, "7.1.4"));
  EXPECT_TRUE(caps_at(2, "2"));
  EXPECT_TRUE(caps_at(6, "5.1"));
  EXPECT_TRUE(caps_at(8, "7.1"));
  EXPECT_TRUE(caps_at(12, "7.1.4"));
}

TEST_F(GkCloudStreamSpec, RefusesAnUnknownAudioCeiling) {
  const auto bad_ceiling = with_audio(2, 7);
  ASSERT_FALSE(bad_ceiling);
  EXPECT_EQ(bad_ceiling.error(), Invalid::audio_channels);
}

TEST_F(GkCloudStreamSpec, RefusesEveryOtherChannelCount) {
  EXPECT_TRUE(refuses_every_other_channel_count());
  EXPECT_TRUE(refuses_channels(262));
  EXPECT_TRUE(refuses_channels(0x10000000c));
  EXPECT_TRUE(refuses_channels(std::numeric_limits<std::uint64_t>::max()));
}

TEST_F(GkCloudStreamSpec, SendsTheNearestResolution) {
  EXPECT_TRUE(maps_resolution(720));
  EXPECT_TRUE(maps_resolution(1080));
  EXPECT_TRUE(maps_resolution(1440));
  EXPECT_TRUE(maps_resolution(2160));
  EXPECT_TRUE(maps_resolution(0));
}

TEST_F(GkCloudStreamSpec, EscapesTheEntitlement) {
  const auto quoted = make("e\"\\\n");
  ASSERT_TRUE(quoted);
  EXPECT_NE(quoted.value().find("\"entitlementId\":\"e\\\"\\\\\\n\""),
            std::string::npos);
  const auto control = make(std::string(1, '\x01'));
  ASSERT_TRUE(control);
  EXPECT_NE(control.value().find("\"entitlementId\":\"\\u0001\""),
            std::string::npos);
}

TEST_F(GkCloudStreamSpec, RefusesAnEmptyOversizedOrNulEntitlement) {
  EXPECT_FALSE(make(""));
  EXPECT_FALSE(make(std::string(4097, 'x')));
  EXPECT_FALSE(make(std::string_view{"a\0b", 3}));
}

TEST_F(GkCloudStreamSpec, GrowsPastSixteenKilobytesWhenEscaped) {
  const auto escaped = make(std::string(4096, '\x01'));
  ASSERT_TRUE(escaped);
  EXPECT_GT(escaped.value().size(), std::size_t{16 * 1024});
}

TEST_F(GkCloudStreamSpec, LeavesAccessibilityOffAndQualityHighByDefault) {
  EXPECT_EQ(JsonTree::integer_at(root, "accessibilityTtsEnable"), 0);
  EXPECT_EQ(JsonTree::integer_at(root, "accessibilityContrast"), 0);
  const auto* video = JsonTree::member(root, "videoStreamSettings");
  ASSERT_NE(video, nullptr);
  EXPECT_EQ(JsonTree::integer_at(*video, "hqMode"), 1);
}

TEST_F(GkCloudStreamSpec, TurnsOffHighQualityWhenAsked) {
  const auto steady = SpecRig::json_of(gk::LaunchSpec::cloud_stream(
      "synthetic-entitlement", gk::Language{"en-US"},
      gk::TimeZone{"UTC+09:00", SpecRig::local_clock}, 1080,
      SpecRig::cloud_stream_version, 0, false, {}, false));
  ASSERT_TRUE(steady);
  const auto steady_root = JsonTree::parse(steady.value());
  ASSERT_TRUE(steady_root);
  const auto* video =
      JsonTree::member(steady_root.value(), "videoStreamSettings");
  ASSERT_NE(video, nullptr);
  EXPECT_EQ(JsonTree::integer_at(*video, "hqMode"), 0);
}

TEST(GkCloudStreamHdrSpec, AsksForHevcHdrAtTheChosenResolution) {
  const auto encoded = SpecRig::json_of(gk::LaunchSpec::cloud_stream(
      "synthetic-entitlement", gk::Language{"en-US"},
      gk::TimeZone::parse("UTC").value(), 2160, SpecRig::cloud_stream_version,
      0, true));
  ASSERT_TRUE(encoded);
  const auto root = SpecRig::object(encoded.value());
  ASSERT_TRUE(root);
  EXPECT_EQ(JsonTree::text_at(*root, "videoCodec"), "hevc");
  EXPECT_EQ(JsonTree::text_at(*root, "dynamicRange"), "HDR");
  EXPECT_EQ(JsonTree::text_at(*root, "model"), "portal");
  EXPECT_EQ(JsonTree::text_at(*root, "httpUserAgent"),
            "PlayStation Portal/6.0.0-rel.444+6a9cea6f5");
  const auto* video = JsonTree::member(*root, "videoStreamSettings");
  ASSERT_NE(video, nullptr);
  ASSERT_TRUE(video->is_object());
  EXPECT_EQ(JsonTree::integer_at(*video, "preferredMaxResolution"), 2160);
  EXPECT_EQ(JsonTree::text_at(*video, "supportedDynamicRange"), "HDR");
  EXPECT_EQ(JsonTree::text_at(*video, "preferredDynamicRange"), "HDR");
}

class GkSpecTimeZone : public ::testing::Test {
 protected:
  static auto zone_of(std::string_view zone) -> std::optional<std::string> {
    const auto encoded = SpecRig::json_of(gk::LaunchSpec::cloud_stream(
        "synthetic-entitlement", gk::Language{"en-US"},
        gk::TimeZone{zone, SpecRig::local_clock}, 1080));
    if (!encoded) return std::nullopt;
    const auto parsed = JsonTree::parse(encoded.value());
    if (!parsed) return std::nullopt;
    const auto* value = JsonTree::member(parsed.value(), "timeZone");
    if (value == nullptr || !value->is_string()) return std::nullopt;
    return std::string{JsonTree::text(*value)};
  }
};

TEST_F(GkSpecTimeZone, WritesAnOffsetInItsCanonicalForm) {
  EXPECT_EQ(zone_of("UTC-05:30"), "UTC-05:30");
  EXPECT_EQ(zone_of("utc+09:00"), "UTC+09:00");
  EXPECT_EQ(zone_of("UTC+0900"), "UTC+09:00");
  EXPECT_EQ(zone_of("GMT-3"), "UTC-03:00");
  EXPECT_EQ(zone_of("+5:45"), "UTC+05:45");
  EXPECT_EQ(zone_of("UTC"), "UTC+00:00");
}

TEST_F(GkSpecTimeZone, FallsBackToTheLocalOffsetForAnyOtherName) {
  EXPECT_EQ(zone_of(""), "UTC-03:30");
  EXPECT_EQ(zone_of("UTC+24:00"), "UTC-03:30");
  EXPECT_EQ(zone_of("UTC+09:60"), "UTC-03:30");
  EXPECT_EQ(zone_of("Europe/London"), "UTC-03:30");
  EXPECT_EQ(zone_of("UTC+1:2"), "UTC-03:30");
}

TEST_F(GkSpecTimeZone, UsesUtcForAnUnknownOrImpossibleLocalOffset) {
  const test_support::FixedUtcOffset unknown{std::nullopt};
  const test_support::FixedUtcOffset beyond_a_day{
      std::chrono::minutes{24 * 60}};
  EXPECT_EQ(gk::TimeZone::local(unknown).view(), "UTC+00:00");
  EXPECT_EQ(gk::TimeZone::local(beyond_a_day).view(), "UTC+00:00");
  EXPECT_EQ(gk::TimeZone::local(SpecRig::local_clock).view(), "UTC-03:30");
}

class GkSpecAccessibility : public ::testing::Test {
 protected:
  static auto reads_aloud_with_contrast(
      const libchiaking::Expected<gk::LaunchSpec, Invalid>& spec)
      -> testing::AssertionResult {
    const auto accessible = SpecRig::json_of(spec);
    if (!accessible) return testing::AssertionFailure() << "the spec failed";
    const auto read = JsonTree::parse(accessible.value());
    if (!read) return testing::AssertionFailure() << "the spec is not JSON";
    const std::array<std::pair<std::string_view, std::int64_t>, 7> expected{{
        {"accessibilityTtsEnable", 1},
        {"accessibilityContrast", 1},
        {"accessibilityTtsSpeed", 3},
        {"accessibilityTtsVolume", 12},
        {"accessibilityMarqueeSpeed", 0},
        {"accessibilityLargeText", 0},
        {"accessibilityBoldText", 0},
    }};
    for (const auto& [key, value] : expected)
      if (JsonTree::integer_at(read.value(), key) != value)
        return testing::AssertionFailure() << key << " is not " << value;
    return testing::AssertionSuccess();
  }

  const gk::Accessibility asked{.screen_reader = true, .higher_contrast = true};
  const gk::Language english{"en-US"};
  const gk::TimeZone tokyo{"UTC+09:00", SpecRig::local_clock};
};

TEST_F(GkSpecAccessibility, ReadsAloudWithContrastForCloudStream) {
  EXPECT_TRUE(reads_aloud_with_contrast(gk::LaunchSpec::cloud_stream(
      "synthetic-entitlement", english, tokyo, 1080,
      SpecRig::cloud_stream_version, 0, false, asked)));
}

TEST_F(GkSpecAccessibility, ReadsAloudWithContrastForLegacyCloudStream) {
  EXPECT_TRUE(reads_aloud_with_contrast(gk::LaunchSpec::legacy_cloud_stream(
      "synthetic-entitlement", english, tokyo, 1080, SpecRig::legacy_version,
      asked)));
}

TEST_F(GkSpecAccessibility, SendsCustomizedSettings) {
  const gk::Accessibility customized{.screen_reader = false,
                                     .screen_reader_speed = 7,
                                     .screen_reader_volume = 20,
                                     .text_size = 2,
                                     .bold_text = true,
                                     .higher_contrast = true};
  const auto custom_spec = SpecRig::json_of(gk::LaunchSpec::cloud_stream(
      "synthetic-entitlement", english, tokyo, 1080,
      SpecRig::cloud_stream_version, 0, false, customized));
  ASSERT_TRUE(custom_spec);
  const auto custom_json = JsonTree::parse(custom_spec.value());
  ASSERT_TRUE(custom_json);
  EXPECT_EQ(JsonTree::integer_at(custom_json.value(), "accessibilityTtsEnable"),
            0);
  EXPECT_EQ(JsonTree::integer_at(custom_json.value(), "accessibilityTtsSpeed"),
            7);
  EXPECT_EQ(JsonTree::integer_at(custom_json.value(), "accessibilityTtsVolume"),
            20);
  EXPECT_EQ(JsonTree::integer_at(custom_json.value(), "accessibilityLargeText"),
            2);
  EXPECT_EQ(JsonTree::integer_at(custom_json.value(), "accessibilityBoldText"),
            1);
}

}  // namespace
}  // namespace cloud_test
