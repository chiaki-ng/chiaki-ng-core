#include "src/core/net/protocol/remote_play/stream/stream_info.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

static_assert(remote_play::requested_audio_channels_supported(
    remote_play::AudioRequestTarget::ps5, 12));
static_assert(remote_play::requested_audio_channels_supported(
    remote_play::AudioRequestTarget::cloud_stream, 6));
static_assert(!remote_play::requested_audio_channels_supported(
    remote_play::AudioRequestTarget::legacy_cloud_stream, 6));

class StreamInfoRig : public ::testing::Test {
 protected:
  static auto bytes(std::initializer_list<std::uint8_t> values)
      -> std::vector<std::byte> {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values)
      result.push_back(static_cast<std::byte>(value));
    return result;
  }

  static auto resolution(std::uint32_t width, std::uint32_t height,
                         std::vector<std::byte> video_header)
      -> takion::StreamInfoResolution {
    return {width, height, std::move(video_header)};
  }

  static auto audio_header(std::uint8_t channels, std::uint32_t rate)
      -> std::vector<std::byte> {
    return bytes({channels, 0x10, static_cast<std::uint8_t>(rate >> 24U),
                  static_cast<std::uint8_t>(rate >> 16U),
                  static_cast<std::uint8_t>(rate >> 8U),
                  static_cast<std::uint8_t>(rate), 0x00, 0x00, 0x01, 0xe0, 0x00,
                  0x00, 0x00, 0x01});
  }

  static auto features(std::uint32_t version) -> takion::FeatureSet {
    return takion::resolve_takion_wire_profile(version)->features();
  }

  static auto header_array(const std::vector<std::byte>& source)
      -> remote_play::AudioHeaderBytes {
    remote_play::AudioHeaderBytes result{};
    std::copy_n(source.begin(), result.size(), result.begin());
    return result;
  }

  static auto audio_channel_info() -> takion::StreamInfo {
    takion::StreamInfo info;
    info.audio_header = audio_header(2, 48000);
    auto first = audio_header(2, 24000);
    first.push_back(std::byte{0x99});
    info.audio_channels.push_back(takion::StreamInfoAudioChannel{
        0,
        first,
        std::nullopt,
        {audio_header(6, 48000), audio_header(8, 48000), audio_header(0, 48000),
         bytes({0x02, 0x10}), audio_header(6, 24000)},
        1});
    info.audio_channels.push_back(takion::StreamInfoAudioChannel{
        1, audio_header(1, 16000), true, {}, std::nullopt});
    info.audio_channels.push_back(takion::StreamInfoAudioChannel{
        0, audio_header(1, 8000), false, {audio_header(2, 8000)}, 3});
    info.audio_channels.push_back(takion::StreamInfoAudioChannel{
        5, bytes({0x02, 0x10, 0x00}), false, {}, std::nullopt});
    return info;
  }

  static auto reference_audio_header() -> std::vector<std::byte> {
    return bytes({0x02, 0x10, 0x00, 0x00, 0xac, 0x44, 0x00, 0x00, 0x03, 0xe8,
                  0xde, 0xad, 0xbe, 0xef});
  }
};

using StreamInfoValidation = StreamInfoRig;
using StreamInfoPublication = StreamInfoRig;
using StreamInfoAudioChannels = StreamInfoRig;
using StreamInfoAudioHeader = StreamInfoRig;
using OpusMultistreamLayout = StreamInfoRig;
using RequestedAudioChannels = StreamInfoRig;

TEST_F(StreamInfoValidation, ParsesTheReferenceAudioHeader) {
  takion::StreamInfo info;
  info.audio_header = reference_audio_header();
  info.resolutions.push_back(resolution(1280, 720, bytes({0x01})));
  auto validated = remote_play::StreamInfoCodec::validate(std::move(info));
  ASSERT_TRUE(validated);
  EXPECT_EQ(validated.value().audio,
            (remote_play::AudioHeader{0x02, 0x10, 44100, 1000, 0xdeadbeef}));
  EXPECT_EQ(validated.value().resolutions.size(), 1U);
  const auto original = reference_audio_header();
  EXPECT_TRUE(std::equal(original.begin(), original.end(),
                         validated.value().audio_header.begin(),
                         validated.value().audio_header.end()));
  const auto published =
      remote_play::StreamInfoCodec::to_public_stream_info(validated.value());
  EXPECT_EQ(published.audio_header, validated.value().audio_header);
  EXPECT_EQ(published.audio.channels, 2U);
  EXPECT_EQ(published.audio.rate, 44100U);
}

TEST_F(StreamInfoValidation, RefusesATruncatedOrOversizedAudioHeader) {
  takion::StreamInfo base;
  base.resolutions.push_back(resolution(640, 360, bytes({0x00})));
  for (std::size_t size = 0; size < 14; ++size) {
    auto info = base;
    info.audio_header.assign(size, std::byte{0});
    const auto rejected =
        remote_play::StreamInfoCodec::validate(std::move(info));
    ASSERT_FALSE(rejected) << size << " bytes";
    EXPECT_EQ(rejected.error(),
              remote_play::StreamInfoError::invalid_audio_header)
        << size << " bytes";
  }
  auto oversized = base;
  oversized.audio_header.assign(15, std::byte{0});
  const auto rejected =
      remote_play::StreamInfoCodec::validate(std::move(oversized));
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(),
            remote_play::StreamInfoError::invalid_audio_header);
}

TEST_F(StreamInfoValidation,
       KeepsTheFirstEightResolutionsWithHeadersInTheirOrder) {
  takion::StreamInfo info;
  info.audio_header.assign(14, std::byte{0});
  for (std::uint32_t i = 0; i < 10; ++i) {
    auto header = i == 4 ? std::vector<std::byte>{}
                         : bytes({static_cast<std::uint8_t>(0xa0 + i),
                                  static_cast<std::uint8_t>(0xb0 + i)});
    info.resolutions.push_back(resolution(100 + i, 200 + i, std::move(header)));
  }
  auto validated = remote_play::StreamInfoCodec::validate(std::move(info));
  ASSERT_TRUE(validated);
  ASSERT_EQ(validated.value().resolutions.size(), 8U);
  const std::array<std::uint32_t, 8> expected_widths{100, 101, 102, 103,
                                                     105, 106, 107, 108};
  for (std::size_t i = 0; i < expected_widths.size(); ++i) {
    const auto& entry = validated.value().resolutions[i];
    EXPECT_EQ(entry.width, expected_widths[i]) << "entry " << i;
    EXPECT_EQ(entry.height, expected_widths[i] + 100) << "entry " << i;
    ASSERT_EQ(entry.video_header.size(), 2U) << "entry " << i;
    EXPECT_EQ(entry.video_header[0],
              static_cast<std::byte>(0xa0 + expected_widths[i] - 100))
        << "entry " << i;
    EXPECT_EQ(entry.video_header[1],
              static_cast<std::byte>(0xb0 + expected_widths[i] - 100))
        << "entry " << i;
  }
}

TEST_F(StreamInfoValidation, AcceptsNoResolutions) {
  takion::StreamInfo empty;
  empty.audio_header.assign(14, std::byte{0});
  const auto none = remote_play::StreamInfoCodec::validate(std::move(empty));
  ASSERT_TRUE(none);
  EXPECT_TRUE(none.value().resolutions.empty());
}

TEST_F(StreamInfoValidation, CapsAFullResolutionSetAtEight) {
  takion::StreamInfo full;
  full.audio_header.assign(14, std::byte{0});
  for (std::uint32_t i = 0; i < 10; ++i)
    full.resolutions.push_back(resolution(i, i, bytes({0x01})));
  const auto capped = remote_play::StreamInfoCodec::validate(std::move(full));
  ASSERT_TRUE(capped);
  ASSERT_EQ(capped.value().resolutions.size(), 8U);
  for (std::size_t i = 0; i < 8; ++i)
    EXPECT_EQ(capped.value().resolutions[i].width, i) << "entry " << i;
}

TEST_F(StreamInfoPublication, OwnsItsAudioFormatAndVideoProfiles) {
  takion::StreamInfo info;
  info.audio_header = reference_audio_header();
  info.resolutions.push_back(resolution(1920, 1080, bytes({0x01, 0x02})));
  info.resolutions.push_back(resolution(1280, 720, bytes({0x03})));
  auto validated = remote_play::StreamInfoCodec::validate(std::move(info));
  ASSERT_TRUE(validated);

  const auto ready =
      remote_play::StreamInfoCodec::to_public_stream_info(validated.value());
  using Position = remote_play::AudioChannelPosition;
  EXPECT_EQ(ready.audio, (remote_play::event::AudioFormat{
                             2,
                             16,
                             44100,
                             1000,
                             1,
                             1,
                             {0, 1},
                             {Position::front_left, Position::front_right}}));
  ASSERT_EQ(ready.profiles.size(), 2U);
  EXPECT_EQ(ready.profiles[0], (remote_play::event::VideoProfile{
                                   1920, 1080, bytes({0x01, 0x02})}));
  EXPECT_EQ(ready.profiles[1],
            (remote_play::event::VideoProfile{1280, 720, bytes({0x03})}));

  validated.value().resolutions[0].video_header[0] = std::byte{0xff};
  validated.value().resolutions.clear();
  EXPECT_EQ(ready.profiles[0].video_header, bytes({0x01, 0x02}));
}

TEST_F(StreamInfoPublication, OfAnEmptyStreamHasNoProfiles) {
  const remote_play::ValidatedStreamInfo empty{};
  const auto empty_ready =
      remote_play::StreamInfoCodec::to_public_stream_info(empty);
  EXPECT_TRUE(empty_ready.profiles.empty());
}

TEST_F(StreamInfoAudioChannels, NeedTheChannelSetupFeature) {
  for (const auto version : {9U, 10U}) {
    auto validated = remote_play::StreamInfoCodec::validate(
        audio_channel_info(), features(version));
    ASSERT_TRUE(validated) << "version " << version;
    EXPECT_TRUE(validated.value().audio_channels.empty())
        << "version " << version;
    EXPECT_EQ(validated.value().audio_header,
              header_array(audio_header(2, 48000)))
        << "version " << version;
  }
  auto defaulted = remote_play::StreamInfoCodec::validate(audio_channel_info());
  ASSERT_TRUE(defaulted);
  EXPECT_TRUE(defaulted.value().audio_channels.empty());
}

TEST_F(StreamInfoAudioChannels, KeepAHeaderPerChannel) {
  for (const auto version : {11U, 12U, 14U}) {
    SCOPED_TRACE(testing::Message() << "version " << version);
    auto validated = remote_play::StreamInfoCodec::validate(
        audio_channel_info(), features(version));
    ASSERT_TRUE(validated);
    const auto& channels = validated.value().audio_channels;
    ASSERT_EQ(channels.size(), 2U);
    EXPECT_EQ(channels[0].type, 0U);
    EXPECT_EQ(channels[0].header, header_array(audio_header(2, 24000)));
    EXPECT_FALSE(channels[0].raw_pcm);
    EXPECT_TRUE(channels[0].profiles.empty());
    EXPECT_FALSE(channels[0].fec_mode);
    EXPECT_EQ(channels[1].type, 1U);
    EXPECT_EQ(channels[1].header, header_array(audio_header(1, 16000)));
    EXPECT_TRUE(channels[1].raw_pcm);
    EXPECT_EQ(validated.value().audio_header,
              header_array(audio_header(2, 48000)));
    EXPECT_EQ(validated.value().audio,
              (remote_play::AudioHeader{2, 16, 48000, 480, 1}));
  }
}

TEST_F(StreamInfoAudioChannels, KeyTheirProfilesByChannelCount) {
  for (const auto version : {15U, 18U, 20U}) {
    SCOPED_TRACE(testing::Message() << "version " << version);
    auto validated = remote_play::StreamInfoCodec::validate(
        audio_channel_info(), features(version));
    ASSERT_TRUE(validated);
    const auto& channels = validated.value().audio_channels;
    ASSERT_EQ(channels.size(), 2U);
    EXPECT_EQ(channels[0].fec_mode, std::optional<std::uint32_t>{1});
    EXPECT_EQ(channels[0].profiles.size(), 2U);
    ASSERT_TRUE(channels[0].profiles.contains(6));
    ASSERT_TRUE(channels[0].profiles.contains(8));
    EXPECT_EQ(channels[0].profiles.at(6), header_array(audio_header(6, 24000)));
    EXPECT_EQ(channels[0].profiles.at(8), header_array(audio_header(8, 48000)));
    EXPECT_FALSE(channels[0].profiles.contains(0));
    EXPECT_FALSE(channels[0].profiles.contains(2));
    EXPECT_TRUE(channels[1].profiles.empty());
    EXPECT_FALSE(channels[1].fec_mode);
  }
}

TEST_F(StreamInfoAudioHeader, IgnoresTrailingBytes) {
  auto header = audio_header(6, 44100);
  header.push_back(std::byte{0xff});
  const auto parsed = remote_play::StreamInfoCodec::parse_audio_header(header);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, (remote_play::AudioHeader{6, 16, 44100, 480, 1}));
  header.resize(13);
  EXPECT_FALSE(remote_play::StreamInfoCodec::parse_audio_header(header));
  EXPECT_EQ(remote_play::StreamInfoCodec::to_public_audio_format(
                remote_play::AudioHeader{6, 16, 44100, 480, 1}),
            (remote_play::event::AudioFormat{
                6, 16, 44100, 480, 4, 2, {0, 1, 2, 3, 4, 5}}));
}

TEST_F(StreamInfoAudioHeader, PublishesTheDecoderLayoutOfGrantedSurround) {
  const auto twelve = bytes({0x0c, 0x10, 0x00, 0x00, 0xbb, 0x80, 0x00, 0x00,
                             0x01, 0xe0, 0x00, 0x00, 0x00, 0x01});
  const auto parsed = remote_play::StreamInfoCodec::parse_audio_header(twelve);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, (remote_play::AudioHeader{12, 16, 48000, 480, 1}));
  EXPECT_EQ(
      remote_play::StreamInfoCodec::to_public_audio_format(*parsed),
      (remote_play::event::AudioFormat{
          12, 16, 48000, 480, 7, 5, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}}));

  takion::StreamInfo info;
  info.audio_header = bytes({0x08, 0x10, 0x00, 0x00, 0xbb, 0x80, 0x00, 0x00,
                             0x01, 0xe0, 0x00, 0x00, 0x00, 0x01});
  const auto validated =
      remote_play::StreamInfoCodec::validate(std::move(info));
  ASSERT_TRUE(validated);
  EXPECT_EQ(
      remote_play::StreamInfoCodec::to_public_stream_info(validated.value())
          .audio,
      (remote_play::event::AudioFormat{
          8, 16, 48000, 480, 5, 3, {0, 1, 2, 3, 4, 5, 6, 7}}));

  const auto three = remote_play::StreamInfoCodec::to_public_audio_format(
      remote_play::AudioHeader{3, 16, 48000, 480, 1});
  EXPECT_EQ(three, (remote_play::event::AudioFormat{3, 16, 48000, 480}));
}

TEST_F(OpusMultistreamLayout, FollowsTheNativeTable) {
  using Layout = remote_play::OpusMultistreamLayout;
  using Position = remote_play::AudioChannelPosition;
  EXPECT_EQ(remote_play::opus_multistream_layout(1),
            (Layout{1, 0, {0}, {Position::mono}}));
  EXPECT_EQ(
      remote_play::opus_multistream_layout(2),
      (Layout{1, 1, {0, 1}, {Position::front_left, Position::front_right}}));
  EXPECT_EQ(remote_play::opus_multistream_layout(6),
            (Layout{4, 2, {0, 1, 2, 3, 4, 5}}));
  EXPECT_EQ(remote_play::opus_multistream_layout(8),
            (Layout{5, 3, {0, 1, 2, 3, 4, 5, 6, 7}}));
  EXPECT_EQ(remote_play::opus_multistream_layout(12),
            (Layout{7, 5, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}}));
}

TEST_F(OpusMultistreamLayout, HasNoLayoutForOtherChannelCounts) {
  for (const auto channels : std::initializer_list<std::uint8_t>{
           0, 3, 4, 5, 7, 9, 10, 11, 13, 16, 255})
    EXPECT_FALSE(remote_play::opus_multistream_layout(channels))
        << unsigned{channels} << " channels";
}

TEST_F(OpusMultistreamLayout, LeavesSurroundPositionsUnknown) {
  for (const auto surround : std::initializer_list<std::uint8_t>{6, 8, 12}) {
    const auto layout = remote_play::opus_multistream_layout(surround);
    ASSERT_TRUE(layout) << unsigned{surround} << " channels";
    for (const auto position : layout->positions)
      EXPECT_EQ(position, remote_play::AudioChannelPosition::unknown)
          << unsigned{surround} << " channels";
  }
}

TEST_F(OpusMultistreamLayout, CodesEveryChannelWithinTheMaximum) {
  for (unsigned channels = 0; channels < 256; ++channels) {
    const auto layout = remote_play::opus_multistream_layout(
        static_cast<std::uint8_t>(channels));
    if (!layout) continue;
    EXPECT_EQ(static_cast<unsigned>(layout->streams + layout->coupled_streams),
              channels);
    EXPECT_LE(channels, remote_play::audio_max_channels);
  }
}

TEST_F(RequestedAudioChannels, AllowSurroundOnlyForAPs5OrCloudStream) {
  using Target = remote_play::AudioRequestTarget;
  for (const auto target :
       {Target::ps4, Target::ps5, Target::legacy_cloud_stream,
        Target::cloud_stream}) {
    for (std::uint32_t channels = 0; channels < 600; ++channels) {
      const bool stereo = channels == 0 || channels == 2;
      const bool surround = channels == 6 || channels == 8 || channels == 12;
      EXPECT_EQ(
          remote_play::requested_audio_channels_supported(target, channels),
          stereo || (surround &&
                     (target == Target::ps5 || target == Target::cloud_stream)))
          << testing::PrintToString(target) << " with " << channels
          << " channels";
    }
    EXPECT_FALSE(
        remote_play::requested_audio_channels_supported(target, 0x1000000cU))
        << testing::PrintToString(target);
    EXPECT_FALSE(
        remote_play::requested_audio_channels_supported(target, 0xffffffffU))
        << testing::PrintToString(target);
  }
}

}  // namespace
}  // namespace remote_play_test
