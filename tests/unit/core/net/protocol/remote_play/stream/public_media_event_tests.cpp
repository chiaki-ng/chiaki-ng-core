#include "tests/unit/core/net/protocol/remote_play/stream/cloud_stream_media_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <utility>
#include <variant>

namespace remote_play_test {
namespace {

using Rig = CloudStreamMediaRig;

TEST(PublicMediaEvent, CarriesAnAssembledVideoFrameAndItsFlags) {
  remote_play::CloudStreamMediaAssembly assembly;
  EXPECT_TRUE(assembly.consume(Rig::video_unit(0, Rig::bytes({0, 1, 0x11}))));
  auto flagged = Rig::video_unit(1, Rig::bytes({0, 2, 0x22, 0x33}));
  flagged.suppress_presentation = true;
  flagged.dynamic_range_flag = true;
  auto completed_video = assembly.consume(flagged);
  ASSERT_TRUE(completed_video);
  ASSERT_EQ(completed_video.value().size(), 1U);
  auto encoded =
      std::get<takion::client_event::EncodedVideo>(completed_video.value()[0]);
  EXPECT_EQ(encoded.frames_lost, 0U);
  EXPECT_FALSE(encoded.frame_recovered);
  EXPECT_TRUE(encoded.suppress_presentation);
  EXPECT_TRUE(encoded.dynamic_range_flag);
  encoded.frames_lost = 4;
  encoded.frame_recovered = true;
  const auto video = remote_play::PublicMediaEvent::from(
      remote_play::ActiveMediaEvent{std::move(encoded)});
  EXPECT_EQ(video.kind, remote_play::EncodedMediaKind::video);
  EXPECT_EQ(video.bytes, Rig::bytes({0x11, 0x22, 0x33}));
  EXPECT_EQ(video.frame_index, 7U);
  EXPECT_EQ(video.codec, 1U);
  EXPECT_EQ(video.adaptive_stream_index, 3U);
  EXPECT_EQ(video.frames_lost, 4U);
  EXPECT_TRUE(video.frame_recovered);
  EXPECT_TRUE(video.suppress_presentation);
  EXPECT_TRUE(video.dynamic_range_flag);
}

TEST(PublicMediaEvent, CarriesControllerSpeakerAudioWithItsPad) {
  const auto media =
      remote_play::PublicMediaEvent::from(remote_play::ActiveMediaEvent{
          takion::client_event::ControllerSpeaker{Rig::bytes({0x5a}), 3}});
  EXPECT_EQ(media.kind, remote_play::EncodedMediaKind::controller_speaker);
  EXPECT_EQ(media.pad_index, 3U);
  EXPECT_EQ(media.bytes, Rig::bytes({0x5a}));
}

TEST(PublicMediaEvent, CarriesAnAssembledAudioFrameWithoutVideoFields) {
  remote_play::CloudStreamMediaAssembly assembly;
  EXPECT_TRUE(assembly.consume(Rig::audio_unit(10, 0, Rig::bytes({0xaa}))));
  EXPECT_TRUE(
      assembly.consume(Rig::audio_unit(11, 1, Rig::bytes({0xbb, 0xcc}))));
  EXPECT_TRUE(assembly.consume(Rig::audio_unit(12, 0, Rig::bytes({0xdd}))));
  auto completed_audio =
      assembly.consume(Rig::audio_unit(13, 1, Rig::bytes({0xee})));
  ASSERT_TRUE(completed_audio);
  ASSERT_EQ(completed_audio.value().size(), 4U);
  const auto audio =
      remote_play::PublicMediaEvent::from(remote_play::ActiveMediaEvent{
          std::move(std::get<takion::client_event::EncodedAudio>(
              completed_audio.value()[1]))});
  EXPECT_EQ(audio.kind, remote_play::EncodedMediaKind::audio);
  EXPECT_EQ(audio.bytes, Rig::bytes({0xbb, 0xcc}));
  EXPECT_EQ(audio.frame_index, 0U);
  EXPECT_EQ(audio.codec, 0U);
  EXPECT_EQ(audio.adaptive_stream_index, 0U);
  EXPECT_FALSE(audio.suppress_presentation);
  EXPECT_FALSE(audio.dynamic_range_flag);
}

TEST(PublicMediaEvent, NamesVoiceAudioAndItsProfileHeader) {
  takion::client_event::EncodedAudio spoken{{std::byte{0x51}}};
  spoken.voice = true;
  const auto voice = remote_play::PublicMediaEvent::from(
      remote_play::ActiveMediaEvent{std::move(spoken)});
  EXPECT_EQ(voice.kind, remote_play::EncodedMediaKind::voice_audio);
  EXPECT_EQ(voice.bytes, Rig::bytes({0x51}));
  takion::client_event::EncodedAudio header{{std::byte{1}}};
  header.voice = true;
  header.profile_header = true;
  EXPECT_EQ(remote_play::PublicMediaEvent::from(
                remote_play::ActiveMediaEvent{std::move(header)})
                .kind,
            remote_play::EncodedMediaKind::voice_profile);
}

TEST(PublicMediaEvent, CarriesHapticsWithoutVideoFields) {
  const auto haptics =
      remote_play::PublicMediaEvent::from(remote_play::ActiveMediaEvent{
          takion::client_event::Haptics{{std::byte{0x7f}}}});
  EXPECT_EQ(haptics.kind, remote_play::EncodedMediaKind::haptics);
  EXPECT_EQ(haptics.bytes, Rig::bytes({0x7f}));
  EXPECT_EQ(haptics.frame_index, 0U);
  EXPECT_EQ(haptics.codec, 0U);
  EXPECT_EQ(haptics.adaptive_stream_index, 0U);
}

}  // namespace
}  // namespace remote_play_test
