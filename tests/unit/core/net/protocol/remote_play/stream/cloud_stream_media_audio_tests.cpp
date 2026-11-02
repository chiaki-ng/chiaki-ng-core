#include "tests/unit/core/net/protocol/remote_play/stream/cloud_stream_media_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

using Rig = CloudStreamMediaRig;

class CloudStreamMediaAudio : public ::testing::Test {
 protected:
  static constexpr std::array policies{
      remote_play::AudioAssemblyPolicy::console_units,
      remote_play::AudioAssemblyPolicy::cloud_generation};

  static auto single(remote_play::AudioAssemblyPolicy policy,
                     std::uint16_t frame, std::vector<std::byte> data)
      -> takion::AvPacketValues {
    auto packet = Rig::audio_unit(frame, 0, std::move(data));
    packet.units_in_frame_total = 1;
    packet.units_in_frame_fec =
        policy == remote_play::AudioAssemblyPolicy::console_units ? 1 : 0;
    return packet;
  }

  static auto emitted(remote_play::CloudStreamMediaAssembly& assembly,
                      takion::AvPacketValues packet) -> std::size_t {
    const auto events = assembly.consume(std::move(packet));
    EXPECT_TRUE(events);
    return events ? events.value().size() : 0;
  }
};

TEST_F(CloudStreamMediaAudio, EmitsIndividualWholeAudioUnits) {
  remote_play::CloudStreamMediaAssembly assembly;
  auto first = assembly.consume(Rig::audio_unit(10, 0, Rig::bytes({0, 0xaa})));
  ASSERT_TRUE(first);
  EXPECT_TRUE(first.value().empty());
  auto second =
      assembly.consume(Rig::audio_unit(11, 1, Rig::bytes({0xbb, 0xcc})));
  ASSERT_TRUE(second);
  EXPECT_TRUE(second.value().empty());
  EXPECT_EQ(emitted(assembly, Rig::audio_unit(12, 0, Rig::bytes({0xdd}))), 0U);
  second = assembly.consume(Rig::audio_unit(13, 1, Rig::bytes({0xee})));
  ASSERT_TRUE(second);
  ASSERT_EQ(second.value().size(), 4U);
  const auto* a0 =
      std::get_if<takion::client_event::EncodedAudio>(&second.value()[0]);
  const auto* a1 =
      std::get_if<takion::client_event::EncodedAudio>(&second.value()[1]);
  const auto* a3 =
      std::get_if<takion::client_event::EncodedAudio>(&second.value()[3]);
  ASSERT_TRUE(a3);
  EXPECT_EQ(a3->bytes, Rig::bytes({0xee}));
  ASSERT_TRUE(a0);
  ASSERT_TRUE(a1);
  EXPECT_EQ(a0->bytes, Rig::bytes({0, 0xaa}));
  EXPECT_EQ(a1->bytes, Rig::bytes({0xbb, 0xcc}));
}

TEST_F(CloudStreamMediaAudio, PassesThroughTheJitterStage) {
  remote_play::CloudStreamMediaAssembly assembly;
  const auto cloud = remote_play::AudioAssemblyPolicy::cloud_generation;
  EXPECT_EQ(emitted(assembly, single(cloud, 1, Rig::bytes({1}))), 0U);
  EXPECT_EQ(emitted(assembly, single(cloud, 2, Rig::bytes({2}))), 0U);
  auto frames = assembly.consume(single(cloud, 3, Rig::bytes({3})));
  ASSERT_TRUE(frames);
  EXPECT_EQ(frames.value().size(), 3U);
  frames = assembly.consume(single(cloud, 5, Rig::bytes({5})));
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 2U);
  EXPECT_TRUE(Rig::audio_of(frames.value()[0]).bytes.empty());
  EXPECT_EQ(Rig::audio_of(frames.value()[1]).bytes, Rig::bytes({5}));
  EXPECT_EQ(emitted(assembly, single(cloud, 4, Rig::bytes({4}))), 0U);
}

TEST_F(CloudStreamMediaAudio, PlaysAReorderedOlderGeneration) {
  remote_play::CloudStreamMediaAssembly assembly;
  const auto generation = [](std::uint16_t frame, int value) {
    auto packet = Rig::audio_unit(frame, 0, Rig::bytes({value}));
    packet.units_in_frame_total = 2;
    packet.units_in_frame_fec = 1;
    return packet;
  };
  EXPECT_EQ(emitted(assembly, generation(0, 1)), 0U);
  EXPECT_EQ(emitted(assembly, generation(4, 2)), 0U);
  const auto frames = assembly.consume(generation(2, 3));
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 5U);
  const auto& played = frames.value();
  EXPECT_EQ(Rig::audio_of(played[0]).bytes, Rig::bytes({1}));
  EXPECT_TRUE(Rig::audio_of(played[1]).bytes.empty());
  EXPECT_EQ(Rig::audio_of(played[2]).bytes, Rig::bytes({3}));
  EXPECT_TRUE(Rig::audio_of(played[3]).bytes.empty());
  EXPECT_EQ(Rig::audio_of(played[4]).bytes, Rig::bytes({2}));
}

TEST_F(CloudStreamMediaAudio, StreamInfoRestartsAudioPlaybackButNotHaptics) {
  for (const auto policy : policies) {
    SCOPED_TRACE(Rig::policy_name(policy));
    remote_play::CloudStreamMediaAssembly assembly{policy, true};
    const auto unit = [policy](std::uint16_t frame, bool haptics) {
      auto packet = single(policy, frame, Rig::bytes({frame}));
      packet.is_haptics = haptics;
      return packet;
    };
    EXPECT_EQ(emitted(assembly, unit(1, false)), 0U);
    EXPECT_EQ(emitted(assembly, unit(2, false)), 0U);
    EXPECT_EQ(emitted(assembly, unit(3, false)), 3U);
    EXPECT_EQ(emitted(assembly, unit(9, true)), 1U);
    assembly.on_stream_info();
    EXPECT_EQ(emitted(assembly, unit(100, false)), 0U);
    EXPECT_EQ(emitted(assembly, unit(101, false)), 0U);
    const auto frames = assembly.consume(unit(102, false));
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames.value().size(), 3U);
    EXPECT_EQ(Rig::audio_of(frames.value()[0]).bytes, Rig::bytes({100}));
    const auto repeated = assembly.consume(unit(9, true));
    EXPECT_TRUE(!repeated || repeated.value().empty());
    EXPECT_EQ(emitted(assembly, unit(10, true)), 1U);
  }
}

TEST_F(CloudStreamMediaAudio, FramesCarryTheirChannelTagAndProfile) {
  for (const auto policy : policies) {
    SCOPED_TRACE(Rig::policy_name(policy));
    remote_play::CloudStreamMediaAssembly assembly{policy, true};
    const auto unit = [policy](std::uint16_t frame, std::uint8_t profile) {
      auto packet = single(policy, frame, Rig::bytes({frame}));
      packet.av_tag = 1;
      packet.audio_profile = profile;
      return packet;
    };
    EXPECT_EQ(emitted(assembly, unit(1, 2)), 0U);
    EXPECT_EQ(emitted(assembly, unit(2, 6)), 0U);
    auto frames = assembly.consume(unit(3, 6));
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames.value().size(), 3U);
    const std::array<std::uint8_t, 3> expected{2, 6, 6};
    for (std::size_t index = 0; index < expected.size(); ++index) {
      const auto& audio = Rig::audio_of(frames.value()[index]);
      EXPECT_EQ(audio.av_tag, std::optional<std::uint8_t>{1})
          << "index " << index;
      EXPECT_EQ(audio.audio_profile, expected[index]) << "index " << index;
      EXPECT_FALSE(audio.profile_header) << "index " << index;
    }
    frames = assembly.consume(unit(5, 8));
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames.value().size(), 2U);
    const auto& concealed = Rig::audio_of(frames.value()[0]);
    EXPECT_TRUE(concealed.bytes.empty());
    EXPECT_FALSE(concealed.av_tag);
    EXPECT_EQ(concealed.audio_profile, 0U);
    EXPECT_EQ(Rig::audio_of(frames.value()[1]).audio_profile, 8U);
  }
}

TEST_F(CloudStreamMediaAudio, ConsoleVoiceLaneIsAssembledBesideTheGameAudio) {
  remote_play::CloudStreamMediaAssembly assembly{
      remote_play::AudioAssemblyPolicy::console_units, true};
  const auto unit = [](std::uint16_t frame, std::uint8_t lane) {
    auto packet = Rig::audio_unit(frame, 0, Rig::bytes({frame * 16 + lane}));
    packet.units_in_frame_total = 1;
    packet.units_in_frame_fec = 1;
    packet.av_tag = lane;
    return packet;
  };
  std::vector<takion::client_event::EncodedAudio> game;
  std::vector<takion::client_event::EncodedAudio> voice;
  for (std::uint16_t frame = 1; frame <= 4; ++frame) {
    for (const std::uint8_t lane : {std::uint8_t{0}, std::uint8_t{1}}) {
      auto events = assembly.consume(unit(frame, lane));
      ASSERT_TRUE(events);
      for (auto& event : events.value()) {
        auto& audio = std::get<takion::client_event::EncodedAudio>(event);
        (audio.voice ? voice : game).push_back(std::move(audio));
      }
    }
  }
  ASSERT_EQ(game.size(), 4U);
  ASSERT_EQ(voice.size(), 4U);
  for (std::size_t index = 0; index < game.size(); ++index) {
    const auto frame = static_cast<int>((index + 1) * 16);
    EXPECT_EQ(game[index].bytes, Rig::bytes({frame})) << "index " << index;
    EXPECT_EQ(voice[index].bytes, Rig::bytes({frame + 1})) << "index " << index;
  }

  const auto gap = assembly.consume(unit(6, 1));
  ASSERT_TRUE(gap);
  ASSERT_EQ(gap.value().size(), 2U);
  const auto& concealed = Rig::audio_of(gap.value()[0]);
  EXPECT_TRUE(concealed.voice);
  EXPECT_TRUE(concealed.bytes.empty());
  EXPECT_TRUE(Rig::audio_of(gap.value()[1]).voice);

  remote_play::CloudStreamMediaAssembly cloud{
      remote_play::AudioAssemblyPolicy::cloud_generation, true};
  std::size_t emitted_count = 0;
  for (std::uint16_t frame = 1; frame <= 4; ++frame) {
    auto packet = unit(frame, 1);
    packet.units_in_frame_fec = 0;
    const auto events = cloud.consume(packet);
    ASSERT_TRUE(events);
    for (const auto& event : events.value()) {
      EXPECT_FALSE(Rig::audio_of(event).voice);
      ++emitted_count;
    }
  }
  EXPECT_GT(emitted_count, 0U);
}

TEST_F(CloudStreamMediaAudio, HapticsCarryThePadAndShareOneFrameCounter) {
  for (const auto policy : policies) {
    SCOPED_TRACE(Rig::policy_name(policy));
    remote_play::CloudStreamMediaAssembly assembly{policy, true};
    const auto haptic = [policy](std::uint16_t frame, std::uint8_t tag) {
      auto packet = single(policy, frame, Rig::bytes({tag}));
      packet.is_haptics = true;
      packet.av_tag = tag;
      return packet;
    };
    auto frames = assembly.consume(haptic(9, 3));
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames.value().size(), 1U);
    EXPECT_EQ(
        std::get<takion::client_event::Haptics>(frames.value()[0]).pad_index,
        1U);
    frames = assembly.consume(haptic(9, 4));
    EXPECT_TRUE(!frames || frames.value().empty());
    frames = assembly.consume(haptic(10, 5));
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames.value().size(), 1U);
    const auto& last =
        std::get<takion::client_event::Haptics>(frames.value()[0]);
    EXPECT_EQ(last.pad_index, 3U);
    EXPECT_EQ(last.bytes, Rig::bytes({5}));
    const auto media = remote_play::PublicMediaEvent::from(
        remote_play::ActiveMediaEvent{last});
    EXPECT_EQ(media.kind, remote_play::EncodedMediaKind::haptics);
    EXPECT_EQ(media.pad_index, 3U);
  }
}

}  // namespace
}  // namespace remote_play_test
