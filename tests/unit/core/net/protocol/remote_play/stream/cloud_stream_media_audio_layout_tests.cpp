#include "tests/unit/core/net/protocol/remote_play/stream/cloud_stream_media_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

using Rig = CloudStreamMediaRig;

class CloudStreamAudioLayout : public ::testing::Test {
 protected:
  static auto three_units() -> takion::AvPacketValues {
    auto packet = Rig::audio_unit(
        10, 0, Rig::bytes({0x10, 0x11, 0x20, 0x21, 0x90, 0x91}));
    packet.units_in_frame_total = 3;
    return packet;
  }

  static auto repeat_frame(std::uint16_t frame) -> takion::AvPacketValues {
    auto packet = Rig::audio_unit(frame, 1, Rig::bytes({0x42}));
    packet.units_in_frame_total = 3;
    packet.units_in_frame_fec = 0xffff;
    packet.audio_mode = 3;
    packet.av_tag = 0;
    return packet;
  }
};

TEST_F(CloudStreamAudioLayout, ConsolePreservesLegacyAndModeZeroUnitLayouts) {
  for (bool count_only : {false, true}) {
    SCOPED_TRACE(count_only ? "count only" : "legacy");
    remote_play::CloudStreamMediaAssembly assembly{
        remote_play::AudioAssemblyPolicy::console_units, count_only};
    auto packet = three_units();
    packet.units_in_frame_fec = count_only ? 2 : 0x0212;
    auto frames = assembly.consume(packet);
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames.value().size(), 3U);
    EXPECT_EQ(Rig::audio_of(frames.value()[0]).bytes, Rig::bytes({0x90, 0x91}));
    EXPECT_EQ(Rig::audio_of(frames.value()[1]).bytes, Rig::bytes({0x10, 0x11}));
    EXPECT_EQ(Rig::audio_of(frames.value()[2]).bytes, Rig::bytes({0x20, 0x21}));
    frames = assembly.consume(packet);
    ASSERT_TRUE(frames);
    EXPECT_TRUE(frames.value().empty());
    packet.is_haptics = true;
    frames = assembly.consume(packet);
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames.value().size(), 2U);
    EXPECT_TRUE(std::holds_alternative<takion::client_event::Haptics>(
        frames.value()[0]));
    const auto repeated = assembly.consume(packet);
    ASSERT_TRUE(repeated);
    EXPECT_TRUE(repeated.value().empty());
    packet.data.pop_back();
    EXPECT_FALSE(assembly.consume(packet));
  }
}

TEST_F(CloudStreamAudioLayout, CompleteFrameRepeatsPreservePayloadPerLane) {
  for (bool count_only : {false, true}) {
    SCOPED_TRACE(count_only ? "count only" : "legacy");
    remote_play::CloudStreamMediaAssembly assembly{
        remote_play::AudioAssemblyPolicy::cloud_generation, count_only};
    std::size_t main = 0;
    std::size_t voice = 0;
    for (std::uint16_t frame = 100; frame < 110; ++frame) {
      for (std::uint8_t lane : {std::uint8_t{1}, std::uint8_t{0}}) {
        for (std::uint16_t copy :
             {std::uint16_t{1}, std::uint16_t{2}, std::uint16_t{0}}) {
          auto packet =
              Rig::audio_unit(frame, copy, Rig::bytes({lane, 0x42, 0x53}));
          packet.units_in_frame_total = 3;
          packet.units_in_frame_fec = 2;
          packet.audio_mode = 3;
          packet.av_tag = lane;
          const auto events = assembly.consume(packet);
          ASSERT_TRUE(events);
          for (const auto& event : events.value()) {
            const auto& audio = Rig::audio_of(event);
            ASSERT_TRUE(audio.av_tag);
            EXPECT_EQ(audio.bytes, Rig::bytes({*audio.av_tag, 0x42, 0x53}));
            if (*audio.av_tag == 0)
              ++main;
            else
              ++voice;
          }
        }
      }
    }
    EXPECT_EQ(main, 10U);
    EXPECT_EQ(voice, 10U);
  }
}

TEST_F(CloudStreamAudioLayout, UnitizedLanesWithEqualIndicesStayApart) {
  for (std::uint8_t first : {std::uint8_t{0}, std::uint8_t{1}}) {
    SCOPED_TRACE("first lane " + std::to_string(first));
    remote_play::CloudStreamMediaAssembly assembly;
    std::size_t main = 0;
    std::size_t voice = 0;
    for (std::uint16_t frame = 100; frame < 110; ++frame) {
      for (std::uint8_t lane : {first, static_cast<std::uint8_t>(1U - first)}) {
        auto packet = Rig::audio_unit(frame, 0, Rig::bytes({lane}));
        packet.units_in_frame_total = 1;
        packet.av_tag = lane;
        const auto events = assembly.consume(packet);
        ASSERT_TRUE(events);
        for (const auto& event : events.value()) {
          const auto& audio = Rig::audio_of(event);
          ASSERT_TRUE(audio.av_tag);
          EXPECT_EQ(audio.bytes, Rig::bytes({*audio.av_tag}));
          if (*audio.av_tag == 0)
            ++main;
          else
            ++voice;
        }
      }
    }
    EXPECT_EQ(main, 10U);
    EXPECT_EQ(voice, 10U);
  }
}

TEST_F(CloudStreamAudioLayout, PackedAudioKeepsEachLaneSeparate) {
  remote_play::CloudStreamMediaAssembly assembly{
      remote_play::AudioAssemblyPolicy::cloud_generation, true};
  for (std::uint8_t lane :
       {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{6}, std::uint8_t{9}}) {
    SCOPED_TRACE("lane " + std::to_string(lane));
    auto packet = Rig::audio_unit(
        10, 0, Rig::bytes({lane, 0x11, lane, 0x21, lane, 0x31}));
    packet.units_in_frame_total = 3;
    packet.units_in_frame_fec = 2;
    packet.audio_mode = remote_play::audio_mode_temporal_redundancy;
    packet.av_tag = lane;
    const auto frames = assembly.consume(packet);
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames.value().size(), 3U);
    for (const auto& event : frames.value()) {
      const auto& audio = Rig::audio_of(event);
      EXPECT_EQ(audio.av_tag, lane);
      ASSERT_EQ(audio.bytes.size(), 2U);
      EXPECT_EQ(audio.bytes[0], std::byte{lane});
    }
    const auto repeated = assembly.consume(packet);
    ASSERT_TRUE(repeated);
    EXPECT_TRUE(repeated.value().empty());
  }
}

TEST_F(CloudStreamAudioLayout, RepeatFramesReorderWrapAndReset) {
  remote_play::CloudStreamMediaAssembly assembly;
  for (unsigned pass = 0; pass < 3; ++pass) {
    SCOPED_TRACE("pass " + std::to_string(pass));
    std::size_t frames = 0;
    for (std::uint16_t index : {std::uint16_t{65534}, std::uint16_t{0},
                                std::uint16_t{65535}, std::uint16_t{1}}) {
      const auto packet = repeat_frame(index);
      const auto events = assembly.consume(packet);
      ASSERT_TRUE(events);
      for (const auto& event : events.value()) {
        EXPECT_EQ(Rig::audio_of(event).bytes, packet.data);
        ++frames;
      }
      const auto repeated = assembly.consume(packet);
      ASSERT_TRUE(repeated);
      EXPECT_TRUE(repeated.value().empty());
    }
    EXPECT_EQ(frames, 4U);
    if (pass == 0)
      assembly.on_stream_info();
    else
      assembly.reset();
  }
}

TEST_F(CloudStreamAudioLayout, StreamInfoResetsAuxiliaryGenerationAndJitter) {
  remote_play::CloudStreamMediaAssembly assembly;
  for (unsigned pass = 0; pass < 3; ++pass) {
    for (std::uint8_t lane :
         {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{6}}) {
      SCOPED_TRACE("pass " + std::to_string(pass) + " lane " +
                   std::to_string(lane));
      std::size_t delivered = 0;
      for (std::uint16_t frame = 100; frame < 103; ++frame) {
        auto packet = Rig::audio_unit(frame, 0, Rig::bytes({lane}));
        packet.units_in_frame_total = 1;
        packet.av_tag = lane;
        const auto events = assembly.consume(packet);
        ASSERT_TRUE(events);
        delivered += events.value().size();
      }
      EXPECT_EQ(delivered, 3U);
    }
    if (pass == 0)
      assembly.on_stream_info();
    else
      assembly.reset();
  }
}

TEST_F(CloudStreamAudioLayout, RepeatFramesRejectInvalidEnvelopes) {
  remote_play::CloudStreamMediaAssembly assembly;
  auto good = Rig::audio_unit(10, 1, Rig::bytes({0x42}));
  good.units_in_frame_total = 3;
  good.audio_mode = 3;
  for (unsigned variant = 0; variant < 7; ++variant) {
    auto packet = good;
    switch (variant) {
      case 0:
        packet.data.clear();
        break;
      case 1:
        packet.codec = 1;
        break;
      case 2:
        packet.units_in_frame_total = 0;
        break;
      case 3:
        packet.units_in_frame_total = 257;
        break;
      case 4:
        packet.unit_index = 3;
        break;
      case 5:
        packet.data.resize(4097);
        break;
      case 6:
        packet.av_tag = 16;
        break;
    }
    EXPECT_FALSE(assembly.consume(packet)) << "variant " << variant;
  }
}

TEST_F(CloudStreamAudioLayout, CloudRoutesOnlyRedundancyModeToConsoleUnits) {
  auto packet = three_units();
  packet.units_in_frame_fec = 2;
  packet.audio_mode = remote_play::audio_mode_temporal_redundancy;
  remote_play::CloudStreamMediaAssembly counted{
      remote_play::AudioAssemblyPolicy::cloud_generation, true};
  auto frames = counted.consume(packet);
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 3U);
  EXPECT_EQ(Rig::audio_of(frames.value()[0]).bytes, Rig::bytes({0x20, 0x21}));
  EXPECT_EQ(Rig::audio_of(frames.value()[1]).bytes, Rig::bytes({0x90, 0x91}));
  EXPECT_EQ(Rig::audio_of(frames.value()[2]).bytes, Rig::bytes({0x10, 0x11}));

  remote_play::CloudStreamMediaAssembly legacy{
      remote_play::AudioAssemblyPolicy::cloud_generation, false};
  frames = legacy.consume(packet);
  EXPECT_TRUE(!frames || frames.value().size() != 3);

  packet.audio_mode = 1;
  remote_play::CloudStreamMediaAssembly padded{
      remote_play::AudioAssemblyPolicy::cloud_generation, true};
  frames = padded.consume(packet);
  EXPECT_TRUE(!frames || frames.value().size() != 3);
}

TEST_F(CloudStreamAudioLayout, ConsoleRoutesOnlyPaddedModeToGenerations) {
  auto packet = three_units();
  packet.units_in_frame_fec = 1;
  for (const std::uint8_t mode :
       {std::uint8_t{0}, remote_play::audio_mode_temporal_redundancy}) {
    packet.audio_mode = mode;
    remote_play::CloudStreamMediaAssembly console{
        remote_play::AudioAssemblyPolicy::console_by_mode, true};
    const auto frames = console.consume(packet);
    ASSERT_TRUE(frames) << "mode " << +mode;
    EXPECT_EQ(frames.value().size(), 3U) << "mode " << +mode;
  }
  packet.audio_mode = 1;
  remote_play::CloudStreamMediaAssembly legacy{
      remote_play::AudioAssemblyPolicy::console_by_mode, false};
  remote_play::CloudStreamMediaAssembly units{
      remote_play::AudioAssemblyPolicy::console_units, false};
  auto frames = legacy.consume(packet);
  const auto expected = units.consume(packet);
  ASSERT_EQ(frames.has_value(), expected.has_value());
  if (frames) {
    EXPECT_EQ(frames.value().size(), expected.value().size());
  }
  remote_play::CloudStreamMediaAssembly padded{
      remote_play::AudioAssemblyPolicy::console_by_mode, true};
  frames = padded.consume(packet);
  EXPECT_TRUE(!frames || frames.value().size() != 3);
}

TEST_F(CloudStreamAudioLayout, ConsoleModeOneKeepsTheWholeFrameOncePerGroup) {
  remote_play::CloudStreamMediaAssembly assembly{
      remote_play::AudioAssemblyPolicy::console_by_mode, true};
  std::vector<std::vector<std::byte>> decoded;
  for (std::uint16_t frame = 1; frame <= 6; ++frame) {
    for (std::uint16_t unit = 0; unit < 3; ++unit) {
      auto packet = Rig::audio_unit(
          frame, unit,
          unit == 0 ? Rig::bytes({0x00, 0x02, 0xf4, frame, 0x44, 0x32})
                    : Rig::bytes({0x55, 0x66, 0x77, 0x88, 0x99, 0xaa}));
      packet.units_in_frame_total = 3;
      packet.units_in_frame_fec = 2;
      packet.audio_mode = 1;
      const auto events = assembly.consume(packet);
      ASSERT_TRUE(events) << "frame " << frame << " unit " << unit;
      for (const auto& event : events.value())
        decoded.push_back(Rig::audio_of(event).bytes);
    }
  }
  ASSERT_GE(decoded.size(), 3U);
  for (std::size_t index = 0; index < decoded.size(); ++index)
    EXPECT_EQ(decoded[index],
              Rig::bytes({0xf4, static_cast<int>(index + 1), 0x44, 0x32}))
        << "index " << index;
}

}  // namespace
}  // namespace remote_play_test
