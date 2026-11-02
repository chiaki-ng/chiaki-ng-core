#include "tests/unit/core/net/protocol/remote_play/stream/active_media_pipeline_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace remote_play_test {
namespace {

using Rig = ActiveMediaRig;

class ActiveMediaAudio : public ActiveMediaHarness<::testing::Test> {
 protected:
  auto voice_pipeline(remote_play::AudioAssemblyPolicy policy, bool cloud,
                      std::uint8_t main_channels, std::uint8_t voice_channels,
                      std::uint32_t version)
      -> remote_play::ActiveMediaPipeline {
    remote_play::ActiveMediaPipeline media{
        sink, remote_play::ActiveMediaOptions{policy, cloud, false, false},
        diagnostics};
    remote_play::ValidatedStreamInfo info;
    info.resolutions.push_back(
        takion::StreamInfoResolution{1920, 1080, {std::byte{1}}});
    info.audio_header = Rig::audio_header(main_channels);
    remote_play::AudioChannelSetup main;
    main.header = Rig::audio_header(main_channels);
    remote_play::AudioChannelSetup voice;
    voice.type = 1;
    voice.header = Rig::audio_header(voice_channels);
    info.audio_channels = {main, voice};
    media.on_stream_info(
        info, takion::resolve_takion_wire_profile(version)->features());
    return media;
  }
};

TEST_F(ActiveMediaAudio, ProfileHeaderPrecedesTheFirstFrameThatRequestsIt) {
  auto media = audio_pipeline(15);
  ASSERT_TRUE(media.consume(Rig::audio_unit(1, 0, 2, std::byte{0x11}), 0));
  ASSERT_TRUE(media.consume(Rig::audio_unit(2, 0, 6, std::byte{0x12}), 0));
  ASSERT_TRUE(media.consume(Rig::audio_unit(3, 0, 6, std::byte{0x13}), 0));
  auto audio = Rig::take_audio(media);
  ASSERT_EQ(audio.size(), 4U);
  EXPECT_FALSE(audio[0].profile_header);
  EXPECT_EQ(audio[0].bytes, std::vector<std::byte>{std::byte{0x11}});
  EXPECT_TRUE(audio[1].profile_header);
  EXPECT_FALSE(audio[1].raw_pcm);
  EXPECT_EQ(audio[1].bytes, Rig::bytes_of(Rig::audio_header(6)));
  EXPECT_EQ(audio[2].bytes, std::vector<std::byte>{std::byte{0x12}});
  EXPECT_EQ(audio[3].bytes, std::vector<std::byte>{std::byte{0x13}});
  ASSERT_TRUE(media.consume(Rig::audio_unit(4, 0, 6, std::byte{0x14}), 0));
  audio = Rig::take_audio(media);
  ASSERT_EQ(audio.size(), 1U);
  EXPECT_FALSE(audio[0].profile_header);
}

TEST_F(ActiveMediaAudio, ConsoleVoiceLaneHasItsOwnHeaderAndDecoder) {
  auto media = voice_pipeline(remote_play::AudioAssemblyPolicy::console_units,
                              false, 2, 1, 15);
  for (std::uint16_t frame = 1; frame <= 3; ++frame) {
    const auto offset = static_cast<std::uint8_t>(frame);
    ASSERT_TRUE(media.consume(
        Rig::audio_unit(frame, 0, 2, std::byte(0x10 + offset)), 0));
    ASSERT_TRUE(media.consume(
        Rig::audio_unit(frame, 1, 1, std::byte(0x20 + offset)), 0));
  }
  std::vector<takion::client_event::EncodedAudio> game;
  std::vector<takion::client_event::EncodedAudio> spoken;
  for (auto& audio : Rig::take_audio(media))
    (audio.voice ? spoken : game).push_back(std::move(audio));
  ASSERT_EQ(game.size(), 3U);
  ASSERT_EQ(spoken.size(), 4U);
  for (std::size_t index = 0; index < game.size(); ++index) {
    EXPECT_FALSE(game[index].profile_header) << "index " << index;
    EXPECT_EQ(game[index].bytes, std::vector<std::byte>{std::byte(
                                     0x11 + static_cast<std::uint8_t>(index))})
        << "index " << index;
  }
  EXPECT_TRUE(spoken[0].profile_header);
  EXPECT_EQ(spoken[0].bytes, Rig::bytes_of(Rig::audio_header(1)));
  for (std::size_t index = 1; index < spoken.size(); ++index) {
    EXPECT_FALSE(spoken[index].profile_header) << "index " << index;
    EXPECT_EQ(spoken[index].bytes,
              std::vector<std::byte>{
                  std::byte(0x20 + static_cast<std::uint8_t>(index))})
        << "index " << index;
  }
  ASSERT_TRUE(media.consume(Rig::audio_unit(4, 1, 1, std::byte{0x24}), 0));
  const auto next = Rig::take_audio(media);
  ASSERT_EQ(next.size(), 1U);
  EXPECT_TRUE(next[0].voice);
  EXPECT_FALSE(next[0].profile_header);
  ASSERT_TRUE(media.consume(Rig::audio_unit(6, 1, 1, std::byte{0x26}), 0));
  const auto after_gap = Rig::take_audio(media);
  ASSERT_EQ(after_gap.size(), 2U);
  for (const auto& audio : after_gap) {
    EXPECT_TRUE(audio.voice);
    EXPECT_FALSE(audio.profile_header);
  }
  EXPECT_TRUE(after_gap[0].bytes.empty());
  EXPECT_EQ(after_gap[1].bytes, std::vector<std::byte>{std::byte{0x26}});
}

TEST_F(ActiveMediaAudio, DropsConsoleVoiceWithoutADescribedLane) {
  auto media = audio_pipeline(15);
  for (std::uint16_t frame = 1; frame <= 3; ++frame)
    ASSERT_TRUE(
        media.consume(Rig::audio_unit(frame, 1, 1, std::byte{0x31}), 0));
  EXPECT_TRUE(Rig::take_audio(media).empty());
}

TEST_F(ActiveMediaAudio, CloudAuxiliaryAudioCannotChangeTheMainDecoder) {
  auto media = voice_pipeline(
      remote_play::AudioAssemblyPolicy::cloud_generation, true, 12, 2, 23);
  for (std::uint8_t lane :
       {std::uint8_t{1}, std::uint8_t{6}, std::uint8_t{9}}) {
    takion::client_event::EncodedAudio audio{{std::byte{0x41}}};
    audio.av_tag = lane;
    EXPECT_TRUE(media.queue_audio(std::move(audio))) << "lane " << +lane;
    EXPECT_TRUE(Rig::take_audio(media).empty()) << "lane " << +lane;
  }
  takion::client_event::EncodedAudio audio{{std::byte{0x42}}};
  audio.av_tag = 0;
  audio.audio_profile = 12;
  EXPECT_TRUE(media.queue_audio(std::move(audio)));
  const auto delivered = Rig::take_audio(media);
  ASSERT_EQ(delivered.size(), 1U);
  EXPECT_FALSE(delivered[0].profile_header);
  EXPECT_EQ(delivered[0].bytes, std::vector<std::byte>{std::byte{0x42}});
}

TEST_F(ActiveMediaAudio, CloudControllerSpeakerLanesLeaveWithTheirPad) {
  remote_play::ActiveMediaPipeline media{
      sink,
      remote_play::ActiveMediaOptions{
          remote_play::AudioAssemblyPolicy::cloud_generation, true, false,
          false, true},
      diagnostics};
  for (const std::uint8_t lane :
       {std::uint8_t{6}, std::uint8_t{9}, std::uint8_t{1}}) {
    takion::client_event::EncodedAudio audio{{std::byte{lane}}};
    audio.av_tag = lane;
    EXPECT_TRUE(media.queue_audio(std::move(audio))) << "lane " << +lane;
  }
  takion::client_event::EncodedAudio lost;
  lost.av_tag = 7;
  EXPECT_TRUE(media.queue_audio(std::move(lost)));
  std::vector<takion::client_event::ControllerSpeaker> speaker;
  while (auto event = media.take()) {
    auto* unit = std::get_if<takion::client_event::ControllerSpeaker>(&*event);
    ASSERT_NE(unit, nullptr);
    speaker.push_back(std::move(*unit));
  }
  ASSERT_EQ(speaker.size(), 3U);
  EXPECT_EQ(speaker[0].pad_index, 0U);
  EXPECT_EQ(speaker[0].bytes, std::vector<std::byte>{std::byte{6}});
  EXPECT_EQ(speaker[1].pad_index, 3U);
  EXPECT_EQ(speaker[1].bytes, std::vector<std::byte>{std::byte{9}});
  EXPECT_EQ(speaker[2].pad_index, 1U);
  EXPECT_TRUE(speaker[2].bytes.empty());
}

TEST_F(ActiveMediaAudio, ProfileSelectionWaitsForFeatureTen) {
  auto media = audio_pipeline(12);
  for (std::uint16_t frame = 1; frame <= 3; ++frame)
    ASSERT_TRUE(
        media.consume(Rig::audio_unit(frame, 0, 6, std::byte{0x20}), 0));
  for (const auto& audio : Rig::take_audio(media))
    EXPECT_FALSE(audio.profile_header);
}

TEST_F(ActiveMediaAudio, LocalStreamsKeepNoCloudAudioDiagnostics) {
  auto local = audio_pipeline(20);
  ASSERT_TRUE(local.consume(Rig::audio_unit(1, 0, 2, std::byte{0x41}), 0));
  EXPECT_FALSE(diagnostics.audio.present);
  EXPECT_FALSE(diagnostics.audio.packet_present);
}

class ActiveMediaCloudAudioDiagnostics
    : public ActiveMediaHarness<::testing::TestWithParam<bool>> {};

TEST_P(ActiveMediaCloudAudioDiagnostics,
       DistinguishTheRootLaneAndTheSelectedProfile) {
  const bool surround_profile = GetParam();
  remote_play::ActiveMediaPipeline media{
      sink,
      remote_play::ActiveMediaOptions{
          remote_play::AudioAssemblyPolicy::cloud_generation, true, false,
          false},
      diagnostics};
  remote_play::ValidatedStreamInfo info;
  info.resolutions.push_back(
      takion::StreamInfoResolution{1920, 1080, {std::byte{1}}});
  info.audio_header = Rig::audio_header(12);
  remote_play::AudioChannelSetup main;
  main.header = Rig::audio_header(2);
  if (surround_profile) main.profiles[8] = Rig::audio_header(8);
  info.audio_channels.push_back(main);
  remote_play::AudioChannelSetup haptics;
  haptics.type = 2;
  haptics.header = Rig::audio_header(2);
  info.audio_channels.push_back(haptics);
  media.on_stream_info(info,
                       takion::resolve_takion_wire_profile(23)->features());
  EXPECT_TRUE(diagnostics.audio.present);
  EXPECT_EQ(diagnostics.audio.channel_type_bits, 0x5U);
  EXPECT_EQ(diagnostics.audio.top_channels, 12U);
  EXPECT_TRUE(diagnostics.audio.main_present);
  EXPECT_EQ(diagnostics.audio.main_channels, 2U);
  EXPECT_EQ(diagnostics.audio.main_profile_bits, surround_profile ? 256U : 0U);
  auto auxiliary = Rig::audio_unit(4, 1, 2, std::byte{0x41});
  auxiliary.units_in_frame_fec = 0;
  ASSERT_TRUE(media.consume(auxiliary, 0));
  EXPECT_FALSE(diagnostics.audio.packet_present);
  for (std::uint16_t frame = 5; frame < 8; ++frame) {
    auto packet = Rig::audio_unit(frame, 0, 8, std::byte{0x42});
    packet.units_in_frame_total = 3;
    packet.unit_index = 1;
    packet.units_in_frame_fec = 0xffff;
    packet.audio_mode = 3;
    ASSERT_TRUE(media.consume(packet, 0)) << "frame " << frame;
  }
  const auto& audio = diagnostics.audio;
  EXPECT_EQ(audio.lane_bits, 0x1U);
  EXPECT_TRUE(audio.packet_present);
  EXPECT_EQ(audio.packet_frame, 5U);
  EXPECT_EQ(audio.packet_unit, 1U);
  EXPECT_EQ(audio.packet_total, 3U);
  EXPECT_EQ(audio.packet_fec, 65535U);
  EXPECT_EQ(audio.packet_bytes, 1U);
  EXPECT_EQ(audio.packet_lane, 0U);
  EXPECT_EQ(audio.packet_profile, 8U);
  EXPECT_EQ(audio.packet_mode, 3U);
  EXPECT_TRUE(audio.selection_present);
  EXPECT_EQ(audio.selection_lane, 0U);
  EXPECT_EQ(audio.selection_profile, 8U);
  EXPECT_EQ(audio.current_channels, surround_profile ? 8U : 2U);
  EXPECT_FALSE(Rig::take_audio(media).empty());
  takion::client_event::EncodedAudio speaker{{std::byte{0x43}}};
  speaker.av_tag = 6;
  EXPECT_TRUE(media.queue_audio(std::move(speaker)));
  EXPECT_EQ(audio.lane_bits, 0x41U);
}

INSTANTIATE_TEST_SUITE_P(
    MainProfiles, ActiveMediaCloudAudioDiagnostics, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& tested) -> std::string {
      return tested.param ? "Surround" : "Stereo";
    });

}  // namespace
}  // namespace remote_play_test
