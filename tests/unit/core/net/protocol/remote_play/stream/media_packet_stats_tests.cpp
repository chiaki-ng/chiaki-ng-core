#include "tests/unit/core/net/protocol/remote_play/stream/cloud_stream_media_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace remote_play_test {
namespace {

using Rig = CloudStreamMediaRig;

class MediaPacketStatistics : public ::testing::Test {
 protected:
  static auto congestion_after_a_restored_frame(double packet_loss_max)
      -> takion::CongestionValues {
    remote_play::CloudStreamMediaAssembly assembly;
    static_cast<void>(assembly.consume(
        Rig::video_frame_unit(1, 10, 0, 3, 1, Rig::bytes({0, 0, 1, 2}))));
    static_cast<void>(assembly.consume(
        Rig::video_frame_unit(2, 10, 2, 3, 1, Rig::bytes({0, 0, 0x8e, 0}))));
    static_cast<void>(assembly.consume(
        Rig::video_frame_unit(3, 10, 1, 3, 1, Rig::bytes({0, 0, 3, 4}))));
    static_cast<void>(assembly.consume(
        Rig::video_frame_unit(5, 11, 0, 3, 1, Rig::bytes({0, 0, 1, 2}))));
    return assembly.take_congestion(packet_loss_max);
  }

  takion::AvPacketValues packet = Rig::video_unit(0, Rig::bytes({0, 0, 1}));
  remote_play::MediaPacketStats stats;
};

TEST_F(MediaPacketStatistics, CountSkippedVideoPacketIndexes) {
  for (const auto index : std::array<std::uint16_t, 4>{10, 11, 13, 14}) {
    packet.packet_index = index;
    stats.video(packet);
  }
  auto measured = stats.take(1.0);
  EXPECT_EQ(measured.server_sent, 5U);
  EXPECT_EQ(measured.lost, 1U);
  packet.packet_index = 17;
  stats.video(packet);
  measured = stats.take(1.0);
  EXPECT_EQ(measured.server_sent, 3U);
  EXPECT_EQ(measured.lost, 2U);
  measured = stats.take(1.0);
  EXPECT_EQ(measured.server_sent, 0U);
  EXPECT_EQ(measured.lost, 0U);
}

TEST_F(MediaPacketStatistics, SaturateAReportAfterALongGapInsteadOfWrapping) {
  stats.audio(0, 4);
  stats.audio(30000, 4);
  const auto measured = stats.take(1.0);
  EXPECT_EQ(measured.server_sent, 0xffffU);
  EXPECT_GT(measured.lost, 0xfff0U);
  EXPECT_LE(measured.lost, measured.server_sent);
}

TEST_F(MediaPacketStatistics, CountLateAndRepeatedVideoAsReceived) {
  for (const auto index : std::array<std::uint16_t, 4>{10, 12, 11, 12}) {
    packet.packet_index = index;
    stats.video(packet);
  }
  const auto measured = stats.take(1.0);
  EXPECT_EQ(measured.server_sent, 4U);
  EXPECT_EQ(measured.lost, 0U);
}

TEST_F(MediaPacketStatistics, CountVideoAcrossTheIndexWrap) {
  for (const auto index : std::array<std::uint16_t, 3>{65534, 65535, 1}) {
    packet.packet_index = index;
    stats.video(packet);
  }
  const auto measured = stats.take(1.0);
  EXPECT_EQ(measured.server_sent, 4U);
  EXPECT_EQ(measured.lost, 1U);
}

TEST_F(MediaPacketStatistics, IgnoreUnitCountsThatDoNotAddUp) {
  packet.units_in_frame_total = 0;
  packet.units_in_frame_fec = 700;
  for (const auto index : std::array<std::uint16_t, 3>{1, 2, 3}) {
    packet.packet_index = index;
    packet.unit_index = index;
    stats.video(packet);
  }
  const auto measured = stats.take(1.0);
  EXPECT_EQ(measured.server_sent, 3U);
  EXPECT_EQ(measured.lost, 0U);
}

TEST_F(MediaPacketStatistics, CountMissingAudioFramesByUnit) {
  stats.audio(10, 3);
  static_cast<void>(stats.take(1.0));
  for (int unit = 0; unit < 3; ++unit) {
    stats.audio(11, 3);
    stats.audio(13, 3);
  }
  const auto measured = stats.take(1.0);
  EXPECT_EQ(measured.server_sent, 9U);
  EXPECT_EQ(measured.lost, 3U);
}

TEST_F(MediaPacketStatistics, CapLossAtPacketLossMax) {
  remote_play::MediaPacketStats capped;
  capped.audio(1);
  capped.audio(10);
  auto values = capped.take(0.2);
  EXPECT_EQ(values.server_sent, 10U);
  EXPECT_EQ(values.lost, 2U);
  remote_play::MediaPacketStats standard;
  standard.audio(1);
  standard.audio(10);
  values = standard.take();
  EXPECT_EQ(values.server_sent, 10U);
  EXPECT_EQ(values.lost, 0U);
}

TEST_F(MediaPacketStatistics, CongestionLossCapFollowsPacketLossMax) {
  const auto uncapped = congestion_after_a_restored_frame(1.0);
  EXPECT_EQ(uncapped.server_sent, 5U);
  EXPECT_EQ(uncapped.lost, 1U);
  const auto loose = congestion_after_a_restored_frame(0.5);
  EXPECT_EQ(loose.server_sent, 5U);
  EXPECT_EQ(loose.lost, 1U);
  const auto tight = congestion_after_a_restored_frame(0.05);
  EXPECT_EQ(tight.server_sent, 5U);
  EXPECT_EQ(tight.lost, 0U);
  const auto fifth = congestion_after_a_restored_frame(0.2);
  EXPECT_EQ(fifth.server_sent, 5U);
  EXPECT_EQ(fifth.lost, 1U);
  const auto tenth = congestion_after_a_restored_frame(0.1);
  EXPECT_EQ(tenth.server_sent, 5U);
  EXPECT_EQ(tenth.lost, 0U);
}

TEST_F(MediaPacketStatistics, HapticsDoNotEnterCongestion) {
  for (auto policy : {remote_play::AudioAssemblyPolicy::console_units,
                      remote_play::AudioAssemblyPolicy::cloud_generation}) {
    SCOPED_TRACE(Rig::policy_name(policy));
    remote_play::CloudStreamMediaAssembly audio_only{policy};
    remote_play::CloudStreamMediaAssembly mixed{policy};
    auto first = Rig::audio_unit(10, 0, Rig::bytes({0x10, 0x11}));
    first.units_in_frame_total = 1;
    auto second = Rig::audio_unit(11, 0, Rig::bytes({0x20, 0x21}));
    second.units_in_frame_total = 1;
    auto haptic = Rig::audio_unit(500, 0, Rig::bytes({0x30, 0x31}));
    haptic.units_in_frame_total = 1;
    haptic.is_haptics = true;
    static_cast<void>(audio_only.consume(first));
    static_cast<void>(audio_only.consume(second));
    static_cast<void>(mixed.consume(first));
    static_cast<void>(mixed.consume(haptic));
    static_cast<void>(mixed.consume(second));
    const auto expected = audio_only.take_congestion();
    const auto measured = mixed.take_congestion();
    EXPECT_EQ(measured.server_sent, expected.server_sent);
    EXPECT_EQ(measured.lost, expected.lost);
  }
}

TEST_F(MediaPacketStatistics, AuxiliaryLanesDoNotInflateMainAudio) {
  remote_play::CloudStreamMediaAssembly main;
  remote_play::CloudStreamMediaAssembly mixed;
  for (std::uint16_t frame = 100; frame < 110; ++frame) {
    auto unit = Rig::audio_unit(frame, 1, Rig::bytes({0x41}));
    unit.units_in_frame_total = 3;
    unit.audio_mode = 3;
    unit.av_tag = 0;
    EXPECT_TRUE(main.consume(unit)) << "frame " << frame;
    EXPECT_TRUE(mixed.consume(unit)) << "frame " << frame;
    EXPECT_TRUE(mixed.consume(unit)) << "frame " << frame;
    unit.av_tag = 1;
    EXPECT_TRUE(mixed.consume(unit)) << "frame " << frame;
  }
  const auto expected = main.take_congestion();
  const auto actual = mixed.take_congestion();
  EXPECT_EQ(actual.server_sent, expected.server_sent);
  EXPECT_EQ(actual.lost, expected.lost);
}

}  // namespace
}  // namespace remote_play_test
