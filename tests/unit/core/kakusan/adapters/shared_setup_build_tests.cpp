#include "src/core/net/kakusan/control_message.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_setup_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

namespace kakusan_test {
namespace {

using libchiaking::net::kakusan::Error;
using libchiaking::net::kakusan::control::SourceCodec;
using test_support::Bytes;

class SharedSetupBuild : public SharedSetupTest {
 protected:
  static auto refused(const libchiaking::media::StreamDescription& changed)
      -> ::testing::AssertionResult {
    return fails_with(build(12, changed, 60), Error::codec);
  }
};

class SharedSetupBuildHevc : public SharedSetupTest {};

TEST_F(SharedSetupBuild, CarriesTheKeysProfilesAndAudio) {
  const auto keys = source_keys();
  const auto built = build(12, ready_info(2), 60);
  ASSERT_TRUE(built);
  const auto& setup = built.value();
  EXPECT_EQ(setup.source_protocol, 12U);
  EXPECT_EQ(setup.video_fps, 60U);
  EXPECT_EQ(setup.source_session_id, session_id());
  EXPECT_TRUE(std::ranges::equal(setup.receive_key, keys.key()));
  EXPECT_TRUE(std::ranges::equal(setup.receive_iv, keys.iv()));
  ASSERT_EQ(setup.video_profiles.size(), 2U);
  EXPECT_EQ(setup.video_profiles[0].width, 1280U);
  EXPECT_EQ(setup.video_profiles[0].height, 720U);
  EXPECT_EQ(setup.video_profiles[1].header, fixture_header());
  EXPECT_EQ(Bytes::hex(setup.audio_header), fixture_audio_header);
  EXPECT_TRUE(setup.validate());
  EXPECT_TRUE(build(9, ready_info(2), 1));
}

TEST_F(SharedSetupBuild, MatchesTheFixtureMessage) {
  const auto same = fixture_setup();
  const auto rebuilt = build(12, ready_info(1), 60, same.source_session_id);
  ASSERT_TRUE(rebuilt);
  const auto left = encoded(rebuilt.value());
  const auto right = encoded(same);
  ASSERT_TRUE(left);
  ASSERT_TRUE(right);
  EXPECT_EQ(left.value(), right.value());
}

TEST_F(SharedSetupBuild, CarriesNewerProtocolVersions) {
  for (const std::uint32_t version : {15U, 19U, 20U}) {
    SCOPED_TRACE(version);
    const auto newer = build(version, ready_info(2), 60);
    ASSERT_TRUE(newer);
    EXPECT_EQ(newer.value().source_protocol, version);
    EXPECT_TRUE(newer.value().validate());
    EXPECT_TRUE(encoded(newer.value()));
  }
}

TEST_F(SharedSetupBuild, RefusesUnsupportedVersionsAndFrameRates) {
  for (const std::uint32_t version : {0U, 7U, 8U, 21U, 256U}) {
    SCOPED_TRACE(version);
    EXPECT_TRUE(fails_with(build(version, ready_info(2), 60), Error::version));
  }
  for (const std::uint32_t fps : {0U, 61U, 1000U, 0xffffffffU}) {
    SCOPED_TRACE(fps);
    EXPECT_TRUE(fails_with(build(12, ready_info(2), fps), Error::codec));
  }
}

TEST_F(SharedSetupBuild, NeedsOneToEightProfiles) {
  EXPECT_FALSE(build(12, ready_info(0), 60));
  EXPECT_FALSE(build(12, ready_info(9), 60));
  EXPECT_TRUE(build(12, ready_info(8), 60));
}

TEST_F(SharedSetupBuild, RefusesPicturesOutside1080p) {
  auto changed = ready_info();
  changed.profiles[0].width = 0;
  EXPECT_TRUE(refused(changed));
  changed.profiles[0].width = 1921;
  EXPECT_TRUE(refused(changed));
  changed.profiles[0].width = 0x10001U;
  EXPECT_TRUE(refused(changed));
  changed = ready_info();
  changed.profiles[0].height = 0;
  EXPECT_TRUE(refused(changed));
  changed.profiles[0].height = 1081;
  EXPECT_TRUE(refused(changed));
  changed.profiles[0].height = 0xffffffffU;
  EXPECT_TRUE(refused(changed));
}

TEST_F(SharedSetupBuild, RefusesProfileHeadersItCannotForward) {
  auto changed = ready_info();
  changed.profiles[0].header.clear();
  EXPECT_TRUE(refused(changed));
  changed.profiles[0].header.assign(513, std::byte{1});
  EXPECT_TRUE(refused(changed));
  changed.profiles[0].header.assign(512, std::byte{1});
  EXPECT_TRUE(refused(changed));
  changed.profiles[0].header = fixture_header();
  changed.profiles[0].header.resize(512);
  EXPECT_TRUE(build(12, changed, 60));
  changed.profiles[0].header = sps(100, 50);
  EXPECT_TRUE(refused(changed));
}

TEST_F(SharedSetupBuild, RefusesAudioThatIsNotOpusStereoAt48k) {
  auto changed = ready_info();
  changed.audio_header = audio_header_with(2, 16, 44100, 960);
  EXPECT_TRUE(refused(changed));
  changed.audio_header = audio_header_with(2, 8, 48000, 960);
  EXPECT_TRUE(refused(changed));
  changed.audio_header = audio_header_with(2, 16, 48000, 1000);
  EXPECT_TRUE(refused(changed));
}

TEST_F(SharedSetupBuild, KeepsTheOpaqueAudioTail) {
  auto changed = ready_info();
  changed.audio_header = audio_header_with(1, 16, 48000, 2880, 0xdeadbeefU);
  const auto opaque = build(12, changed, 60);
  ASSERT_TRUE(opaque);
  EXPECT_EQ(opaque.value().audio_header, changed.audio_header);
}

TEST_F(SharedSetupBuildHevc, LabelsTheSetupAndItsStreamHevc) {
  const auto built = build(12, h265_info(), 60);
  ASSERT_TRUE(built);
  EXPECT_EQ(built.value().source_codec, SourceCodec::h265);
  const auto text = encoded(built.value());
  ASSERT_TRUE(text);
  EXPECT_NE(text.value().find("\"source_codec\":\"h265\""), std::string::npos);
  const auto decoded =
      libchiaking::net::kakusan::control::MessageCodec::decode(text.value());
  ASSERT_TRUE(decoded);
  const auto& setup = std::get<SourceSetup>(decoded.value());
  EXPECT_EQ(setup.source_codec, SourceCodec::h265);
  ASSERT_EQ(setup.video_profiles.size(), 2U);
  EXPECT_EQ(setup.video_profiles[1].header, Bytes::unhex(h265_main_header));
  const auto forwarded = forward(setup);
  ASSERT_TRUE(forwarded);
  EXPECT_TRUE(forwarded.value().stream.hevc);
  ASSERT_FALSE(forwarded.value().stream.description.profiles.empty());
  EXPECT_EQ(forwarded.value().stream.description.profiles[0].header,
            Bytes::unhex(h265_main_header));
}

TEST_F(SharedSetupBuildHevc, LabelsAPlainSetupH264) {
  const auto plain = build(12, ready_info(), 60);
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain.value().source_codec, SourceCodec::h264);
  const auto plain_text = encoded(plain.value());
  ASSERT_TRUE(plain_text);
  EXPECT_NE(plain_text.value().find("\"source_codec\":\"h264\""),
            std::string::npos);
  const auto forwarded = forward(plain.value());
  ASSERT_TRUE(forwarded);
  EXPECT_FALSE(forwarded.value().stream.hevc);
}

TEST_F(SharedSetupBuildHevc, RefusesProfilesOfMixedCodecs) {
  auto mixed = h265_info();
  mixed.profiles[1].header = Bytes::unhex(h264_high_header);
  EXPECT_TRUE(fails_with(build(12, mixed, 60), Error::codec));
}

TEST_F(SharedSetupBuildHevc, RefusesToForwardAMislabelledSetup) {
  const auto built = build(12, h265_info(), 60);
  ASSERT_TRUE(built);
  auto mislabelled = built.value().clone();
  mislabelled.source_codec = SourceCodec::h264;
  EXPECT_FALSE(forward(mislabelled));
  const auto plain = build(12, ready_info(), 60);
  ASSERT_TRUE(plain);
  auto swapped = plain.value().clone();
  swapped.source_codec = SourceCodec::h265;
  EXPECT_FALSE(forward(swapped));
}

TEST_F(SharedSetupBuildHevc, RefusesAnHdrPicture) {
  auto info = h265_info();
  info.picture = libchiaking::media::GrantedPicture{
      libchiaking::media::VideoCodec::h265, 1920, 1080, 60, 15000};
  EXPECT_TRUE(build(12, info, 60));
  info.picture->codec = libchiaking::media::VideoCodec::h265_hdr;
  EXPECT_TRUE(fails_with(build(12, info, 60), Error::codec));
}

TEST_F(SharedSetupBuildHevc, RefusesToDecodeAnUnknownCodec) {
  const auto built = build(12, h265_info(), 60);
  ASSERT_TRUE(built);
  const auto text = encoded(built.value());
  ASSERT_TRUE(text);
  auto unknown = text.value();
  const auto at = unknown.find("h265");
  ASSERT_NE(at, std::string::npos);
  unknown.replace(at, 4, "av01");
  EXPECT_TRUE(fails_with(
      libchiaking::net::kakusan::control::MessageCodec::decode(unknown),
      Error::codec));
}

}  // namespace
}  // namespace kakusan_test
