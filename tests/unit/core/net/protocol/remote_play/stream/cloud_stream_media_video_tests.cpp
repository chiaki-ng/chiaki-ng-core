#include "tests/unit/core/net/protocol/remote_play/stream/cloud_stream_media_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <variant>

namespace remote_play_test {
namespace {

using Rig = CloudStreamMediaRig;

TEST(CloudStreamMediaVideo, EmitsAnAssembledFrameOnce) {
  remote_play::CloudStreamMediaAssembly assembly;
  const auto first =
      assembly.consume(Rig::video_unit(0, Rig::bytes({0, 1, 0x11})));
  ASSERT_TRUE(first);
  EXPECT_TRUE(first.value().empty());
  const auto second =
      assembly.consume(Rig::video_unit(1, Rig::bytes({0, 2, 0x22, 0x33})));
  ASSERT_TRUE(second);
  ASSERT_EQ(second.value().size(), 1U);
  const auto* video =
      std::get_if<takion::client_event::EncodedVideo>(&second.value()[0]);
  ASSERT_TRUE(video);
  EXPECT_EQ(video->bytes, Rig::bytes({0x11, 0x22, 0x33}));
  EXPECT_EQ(video->frame_index, 7U);
  EXPECT_EQ(video->codec, 1U);
  EXPECT_EQ(video->adaptive_stream_index, 3U);
}

TEST(CloudStreamMediaVideo, RejectsBadUnitsWithTheirReason) {
  remote_play::CloudStreamMediaAssembly assembly;
  auto bad_audio = Rig::audio_unit(20, 0, Rig::bytes({0}));
  bad_audio.codec = 9;
  const auto audio = assembly.consume(bad_audio);
  ASSERT_FALSE(audio);
  EXPECT_EQ(audio.error(),
            remote_play::CloudStreamMediaAssemblyError::audio_unit_rejected);
  const auto source = assembly.consume(Rig::audio_unit(21, 1, Rig::bytes({0})));
  ASSERT_TRUE(source);
  EXPECT_TRUE(source.value().empty());
  const auto duplicate =
      assembly.consume(Rig::audio_unit(21, 1, Rig::bytes({1})));
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error(),
            remote_play::CloudStreamMediaAssemblyError::audio_rejected);
  const auto video = assembly.consume(Rig::video_unit(0, Rig::bytes({0})));
  ASSERT_FALSE(video);
  EXPECT_EQ(video.error(),
            remote_play::CloudStreamMediaAssemblyError::video_rejected);
  EXPECT_EQ(assembly.last_video_error(),
            remote_play::VideoAssemblerError::short_source);
  auto other = Rig::video_unit(0, Rig::bytes({0, 0}));
  other.type = takion::PacketType{0x05};
  const auto unsupported = assembly.consume(other);
  EXPECT_FALSE(assembly.last_video_error());
  ASSERT_FALSE(unsupported);
  EXPECT_EQ(unsupported.error(),
            remote_play::CloudStreamMediaAssemblyError::unsupported_family);
}

TEST(CloudStreamMediaVideo, ALateUnitForAFecRestoredSlotCountsAsReceived) {
  remote_play::CloudStreamMediaAssembly assembly;
  const auto first = assembly.consume(
      Rig::video_frame_unit(1, 10, 0, 3, 1, Rig::bytes({0, 0, 1, 2})));
  ASSERT_TRUE(first);
  EXPECT_TRUE(first.value().empty());
  const auto restored = assembly.consume(
      Rig::video_frame_unit(2, 10, 2, 3, 1, Rig::bytes({0, 0, 0x8e, 0})));
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored.value().size(), 1U);
  EXPECT_FALSE(assembly.consume(
      Rig::video_frame_unit(3, 10, 1, 3, 1, Rig::bytes({0, 0, 3, 4}))));
  static_cast<void>(assembly.consume(
      Rig::video_frame_unit(4, 11, 0, 3, 1, Rig::bytes({0, 0, 1, 2}))));
  const auto measured = assembly.take_congestion(1.0);
  EXPECT_EQ(measured.server_sent, 4U);
  EXPECT_EQ(measured.lost, 0U);
}

TEST(CloudStreamMediaVideo, AFecRestoreFromAnotherGeometryDoesNotHideAUnit) {
  remote_play::CloudStreamMediaAssembly assembly;
  EXPECT_FALSE(assembly.consume(
      Rig::video_frame_unit(1, 10, 5, 1, 0, Rig::bytes({0, 0, 9, 9}))));
  const auto first = assembly.consume(
      Rig::video_frame_unit(2, 10, 0, 3, 1, Rig::bytes({0, 0, 1, 2})));
  ASSERT_TRUE(first);
  EXPECT_TRUE(first.value().empty());
  const auto restored = assembly.consume(
      Rig::video_frame_unit(3, 10, 2, 3, 1, Rig::bytes({0, 0, 0x8e, 0})));
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored.value().size(), 1U);
  static_cast<void>(assembly.consume(
      Rig::video_frame_unit(4, 10, 1, 3, 1, Rig::bytes({0, 0, 3, 4}))));
  static_cast<void>(assembly.consume(
      Rig::video_frame_unit(5, 11, 0, 3, 1, Rig::bytes({0, 0, 1, 2}))));
  const auto measured = assembly.take_congestion(1.0);
  EXPECT_EQ(measured.server_sent, 5U);
  EXPECT_EQ(measured.lost, 0U);
}

}  // namespace
}  // namespace remote_play_test
