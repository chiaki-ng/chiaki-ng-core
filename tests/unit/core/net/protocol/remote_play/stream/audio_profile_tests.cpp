#include "src/core/net/protocol/remote_play/stream/audio_negotiation.hpp"
#include "src/core/net/protocol/remote_play/stream/public_media_event.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/net/protocol/remote_play/stream/stream_info.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

class AudioProfileRig : public ::testing::Test {
 protected:
  static auto bytes(std::initializer_list<std::uint8_t> values)
      -> std::vector<std::byte> {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(std::byte{value});
    return result;
  }

  static auto header(std::uint8_t channels, std::uint8_t rate_low)
      -> remote_play::AudioHeaderBytes {
    return remote_play::AudioHeaderBytes{
        std::byte{channels}, std::byte{0x10},     std::byte{0}, std::byte{0},
        std::byte{0xbb},     std::byte{rate_low}, std::byte{0}, std::byte{0},
        std::byte{0x01},     std::byte{0xe0},     std::byte{0}, std::byte{0},
        std::byte{0},        std::byte{1}};
  }

  static auto features(std::uint32_t version) -> takion::FeatureSet {
    return takion::resolve_takion_wire_profile(version)->features();
  }

  static auto configured_info() -> remote_play::ValidatedStreamInfo {
    remote_play::ValidatedStreamInfo info;
    info.audio_header = header(2, 0x80);
    remote_play::AudioChannelSetup main;
    main.type = 0;
    main.header = header(2, 0x80);
    main.profiles[6] = header(6, 0x80);
    main.profiles[8] = header(8, 0x80);
    remote_play::AudioChannelSetup raw;
    raw.type = 1;
    raw.header = header(1, 0x40);
    raw.raw_pcm = true;
    raw.profiles[2] = header(2, 0x40);
    info.audio_channels = {main, raw};
    return info;
  }

  static auto as_vector(const auto& range) -> std::vector<std::byte> {
    return {range.begin(), range.end()};
  }
};

using AudioFeatures = AudioProfileRig;
using AudioProfileSelection = AudioProfileRig;
using AudioStateBuild = AudioProfileRig;
using AudioProfileMedia = AudioProfileRig;

TEST_F(AudioFeatures, FollowTheTakionFeatureCatalogue) {
  using remote_play::AudioFeature;
  for (std::uint32_t version :
       {9U, 10U, 11U, 12U, 14U, 15U, 17U, 18U, 19U, 20U}) {
    const auto set = features(version);
    EXPECT_EQ(remote_play::AudioProfileSelector::has_feature(
                  set, AudioFeature::channel_setup),
              version >= 11)
        << "version " << version;
    EXPECT_EQ(remote_play::AudioProfileSelector::has_feature(
                  set, AudioFeature::channel_profiles),
              version >= 15)
        << "version " << version;
    EXPECT_EQ(remote_play::AudioProfileSelector::has_feature(
                  set, AudioFeature::mode_channel_profiles),
              version >= 18)
        << "version " << version;
    EXPECT_EQ(remote_play::AudioProfileSelector::has_feature(
                  set, AudioFeature::wide_available_bits),
              version >= 19)
        << "version " << version;
  }
}

TEST_F(AudioFeatures, EnableSelectionPerModeFromVersionFifteen) {
  const auto v15 = features(15);
  const auto v18 = features(18);
  for (std::uint32_t mode : {0U, 2U, 4U, 5U})
    EXPECT_TRUE(remote_play::AudioProfileSelector::selection_enabled(v15, mode))
        << "mode " << mode;
  for (std::uint32_t mode : {1U, 3U}) {
    EXPECT_FALSE(
        remote_play::AudioProfileSelector::selection_enabled(v15, mode))
        << "mode " << mode;
    EXPECT_TRUE(remote_play::AudioProfileSelector::selection_enabled(v18, mode))
        << "mode " << mode;
  }
  EXPECT_FALSE(
      remote_play::AudioProfileSelector::selection_enabled(features(12), 0));
}

TEST_F(AudioProfileSelection, NeverSwitchesALegacyStream) {
  remote_play::AudioProfileSelector selector;
  auto info = configured_info();
  info.audio_channels.clear();
  selector.configure(info, features(20));
  EXPECT_FALSE(selector.select(std::uint8_t{0}, 6));
  EXPECT_FALSE(selector.select(std::nullopt, 8));
}

TEST_F(AudioProfileSelection, PicksTheProfileMappedToTheChannelCount) {
  remote_play::AudioProfileSelector selector;
  selector.configure(configured_info(), features(15));
  EXPECT_FALSE(selector.select(std::nullopt, 6));
  EXPECT_FALSE(selector.select(std::uint8_t{0}, 0));
  EXPECT_FALSE(selector.select(std::uint8_t{0}, 2));
  EXPECT_FALSE(selector.select(std::uint8_t{0}, 3));
  auto change = selector.select(std::uint8_t{0}, 6);
  ASSERT_TRUE(change);
  EXPECT_EQ(*change, (remote_play::AudioProfileChange{header(6, 0x80), false}));
  EXPECT_FALSE(selector.select(std::uint8_t{0}, 6));
  EXPECT_FALSE(selector.select(std::uint8_t{0}, 0));
  EXPECT_FALSE(selector.select(std::uint8_t{9}, 0));
  change = selector.select(std::uint8_t{0}, 8);
  ASSERT_TRUE(change);
  EXPECT_EQ(change->header, header(8, 0x80));
  EXPECT_FALSE(selector.select(std::uint8_t{0}, 2));
  EXPECT_FALSE(selector.select(std::nullopt, 0));
}

TEST_F(AudioProfileSelection, FollowsTheSetupOfEachChannelTag) {
  remote_play::AudioProfileSelector selector;
  selector.configure(configured_info(), features(15));
  auto change = selector.select(std::uint8_t{1}, 0);
  ASSERT_TRUE(change);
  EXPECT_EQ(*change, (remote_play::AudioProfileChange{header(1, 0x40), true}));
  change = selector.select(std::uint8_t{1}, 2);
  ASSERT_TRUE(change);
  EXPECT_EQ(*change, (remote_play::AudioProfileChange{header(2, 0x40), true}));
  change = selector.select(std::uint8_t{0}, 0);
  ASSERT_TRUE(change);
  EXPECT_EQ(*change, (remote_play::AudioProfileChange{header(2, 0x80), false}));
  change = selector.select(std::uint8_t{1}, 0);
  ASSERT_TRUE(change);
  EXPECT_EQ(*change, (remote_play::AudioProfileChange{header(2, 0x40), true}));
}

TEST_F(AudioProfileSelection, IsGatedByVersionAndMode) {
  remote_play::AudioProfileSelector selector;
  selector.configure(configured_info(), features(12));
  EXPECT_FALSE(selector.select(std::uint8_t{0}, 6));
  selector.configure(configured_info(), features(15), 1);
  EXPECT_FALSE(selector.select(std::uint8_t{0}, 6));
  selector.configure(configured_info(), features(18), 3);
  EXPECT_TRUE(selector.select(std::uint8_t{0}, 6));
  selector.reset();
  EXPECT_FALSE(selector.select(std::uint8_t{0}, 8));
}

TEST_F(AudioStateBuild, LaysOutFlagsAsLittleEndianWords) {
  const auto flags = remote_play::audio_state_flags(3, 0xff, 1);
  EXPECT_EQ(flags.size(), 9U);
  EXPECT_EQ(as_vector(flags),
            bytes({0x03, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}));
  const auto wide =
      remote_play::audio_state_flags(0x81, 0x12345678, 0x9abcdef0);
  EXPECT_EQ(as_vector(wide),
            bytes({0x81, 0x78, 0x56, 0x34, 0x12, 0xf0, 0xde, 0xbc, 0x9a}));
}

TEST_F(AudioStateBuild, LaysOutTheSpeakerLayoutAndPortStates) {
  const auto layout = remote_play::audio_state_full_layout();
  ASSERT_EQ(layout.size(), 73U);
  const auto encoded = as_vector(layout);
  EXPECT_EQ(
      std::vector<std::byte>(encoded.begin(), encoded.begin() + 21),
      bytes({0x03, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1e, 0x00,
             0x00, 0x00, 0xe2, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
  EXPECT_EQ(std::vector<std::byte>(encoded.begin() + 21, encoded.begin() + 25),
            bytes({0x98, 0xfe, 0x98, 0xfe}));
  EXPECT_EQ(std::vector<std::byte>(encoded.begin() + 37, encoded.begin() + 41),
            bytes({0x79, 0xff, 0x00, 0x00}));
  EXPECT_EQ(std::vector<std::byte>(encoded.begin() + 41, encoded.end()),
            std::vector<std::byte>(32));

  const auto speaker = remote_play::audio_state_port_states(
      remote_play::AudioPortGroup::controller, 2,
      remote_play::AudioPortOutput::controller_speaker,
      remote_play::audio_port_full_volume);
  std::vector<std::byte> expected(33);
  expected[5] = std::byte{0x04};
  expected[13] = std::byte{127};
  EXPECT_EQ(as_vector(speaker), expected);
  const auto unused = remote_play::audio_state_port_states(
      remote_play::AudioPortGroup::main, 4, remote_play::AudioPortOutput::main,
      remote_play::audio_port_full_volume);
  std::vector<std::byte> group_only(33);
  group_only[0] = std::byte{1};
  EXPECT_EQ(as_vector(unused), group_only);

  const auto built = remote_play::AudioStateCodec::build(
      remote_play::AudioStateKind::full, layout, features(15));
  ASSERT_TRUE(built);
  ASSERT_TRUE(built.value().data);
  EXPECT_EQ(built.value().data->size(), 73U);
}

TEST_F(AudioStateBuild, NeedsFeatureTen) {
  const auto flags = remote_play::audio_state_flags(3, 0xff, 1);
  for (const auto version : {9U, 12U, 14U}) {
    const auto rejected = remote_play::AudioStateCodec::build(
        remote_play::AudioStateKind::flags, flags, features(version));
    ASSERT_FALSE(rejected) << "version " << version;
    EXPECT_EQ(rejected.error(),
              remote_play::AudioStateError::unsupported_by_version)
        << "version " << version;
  }
  const auto built = remote_play::AudioStateCodec::build(
      remote_play::AudioStateKind::flags, flags, features(15));
  ASSERT_TRUE(built);
  EXPECT_EQ(built.value().type, takion::AudioStateType::flags);
  EXPECT_EQ(built.value().data, as_vector(flags));
}

TEST_F(AudioStateBuild, CarriesAnEmptyBody) {
  const auto empty = remote_play::AudioStateCodec::build(
      remote_play::AudioStateKind::hrtf, {}, features(15));
  ASSERT_TRUE(empty);
  ASSERT_TRUE(empty.value().data);
  EXPECT_TRUE(empty.value().data->empty());
}

TEST_F(AudioStateBuild, RefusesAnUnknownKind) {
  const auto flags = remote_play::audio_state_flags(3, 0xff, 1);
  const auto unknown = remote_play::AudioStateCodec::build(
      static_cast<remote_play::AudioStateKind>(7), flags, features(20));
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error(), remote_play::AudioStateError::unsupported_kind);
}

TEST_F(AudioStateBuild, SendsTheChannelCountOnlyFromVersionEighteen) {
  const auto body = bytes({0x06});
  for (const auto version : {15U, 16U, 17U}) {
    const auto rejected = remote_play::AudioStateCodec::build(
        remote_play::AudioStateKind::channel_count, body, features(version));
    ASSERT_FALSE(rejected) << "version " << version;
    EXPECT_EQ(rejected.error(),
              remote_play::AudioStateError::unsupported_by_version)
        << "version " << version;
  }
  for (const auto version : {18U, 19U, 20U}) {
    const auto built = remote_play::AudioStateCodec::build(
        remote_play::AudioStateKind::channel_count, body, features(version));
    ASSERT_TRUE(built) << "version " << version;
    EXPECT_EQ(built.value().type, takion::AudioStateType::channel_count)
        << "version " << version;
    EXPECT_EQ(built.value().data, body) << "version " << version;
  }
}

TEST_F(AudioStateBuild, CapsAvailableBitsAtFfWithoutWideBits) {
  const auto wide = remote_play::audio_state_flags(3, 0x3ff, 1);
  for (const auto kind :
       {remote_play::AudioStateKind::full, remote_play::AudioStateKind::flags,
        remote_play::AudioStateKind::angle}) {
    const auto built =
        remote_play::AudioStateCodec::build(kind, wide, features(18));
    ASSERT_TRUE(built);
    ASSERT_TRUE(built.value().data);
    EXPECT_EQ(*built.value().data,
              bytes({0x03, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}));
  }
  const auto high = remote_play::audio_state_flags(3, 0x100, 0);
  const auto capped = remote_play::AudioStateCodec::build(
      remote_play::AudioStateKind::flags, high, features(15));
  ASSERT_TRUE(capped);
  ASSERT_TRUE(capped.value().data);
  ASSERT_GE(capped.value().data->size(), 3U);
  EXPECT_EQ((*capped.value().data)[1], std::byte{0xff});
  EXPECT_EQ((*capped.value().data)[2], std::byte{0});
}

TEST_F(AudioStateBuild, KeepsBodiesThatNeedNoCap) {
  const auto narrow = remote_play::audio_state_flags(3, 0xff, 1);
  auto built = remote_play::AudioStateCodec::build(
      remote_play::AudioStateKind::flags, narrow, features(18));
  ASSERT_TRUE(built);
  ASSERT_TRUE(built.value().data);
  EXPECT_EQ(*built.value().data, as_vector(narrow));
  const auto wide = remote_play::audio_state_flags(3, 0x3ff, 1);
  built = remote_play::AudioStateCodec::build(remote_play::AudioStateKind::hrtf,
                                              wide, features(18));
  ASSERT_TRUE(built);
  ASSERT_TRUE(built.value().data);
  EXPECT_EQ(*built.value().data, as_vector(wide));
  const auto short_body = bytes({0x03, 0x00, 0x01, 0x00});
  built = remote_play::AudioStateCodec::build(
      remote_play::AudioStateKind::flags, short_body, features(18));
  ASSERT_TRUE(built);
  ASSERT_TRUE(built.value().data);
  EXPECT_EQ(*built.value().data, short_body);
}

TEST_F(AudioStateBuild, KeepsWideAvailableBitsOnceTheVersionAllowsThem) {
  const auto wide = remote_play::audio_state_flags(3, 0x3ff, 1);
  const auto built = remote_play::AudioStateCodec::build(
      remote_play::AudioStateKind::flags, wide, features(19));
  ASSERT_TRUE(built);
  ASSERT_TRUE(built.value().data);
  EXPECT_EQ(*built.value().data, as_vector(wide));
}

TEST_F(AudioProfileMedia, MakesAProfileHeaderAPublicAudioProfile) {
  takion::client_event::EncodedAudio audio;
  const auto bytes_of = header(6, 0x80);
  audio.bytes.assign(bytes_of.begin(), bytes_of.end());
  audio.profile_header = true;
  audio.raw_pcm = true;
  const auto media =
      remote_play::PublicMediaEvent::from(remote_play::ActiveMediaEvent{audio});
  EXPECT_EQ(media.kind, remote_play::EncodedMediaKind::audio_profile);
  EXPECT_EQ(media.audio, (remote_play::event::AudioFormat{
                             6, 16, 48000, 480, 4, 2, {0, 1, 2, 3, 4, 5}}));
  EXPECT_TRUE(media.audio_raw_pcm);
  EXPECT_EQ(media.bytes, as_vector(bytes_of));
}

TEST_F(AudioProfileMedia, KeepsAPlainFrameAsAudio) {
  takion::client_event::EncodedAudio plain;
  plain.bytes = bytes({1, 2});
  plain.av_tag = 0;
  plain.audio_profile = 6;
  const auto frame =
      remote_play::PublicMediaEvent::from(remote_play::ActiveMediaEvent{plain});
  EXPECT_EQ(frame.kind, remote_play::EncodedMediaKind::audio);
  EXPECT_FALSE(frame.audio_raw_pcm);
}

}  // namespace
}  // namespace remote_play_test
