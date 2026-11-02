#include "tests/unit/core/net/protocol/remote_play/stream/cloud_stream_media_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

using Rig = CloudStreamMediaRig;

class TemporalAudio : public ::testing::Test {
 protected:
  static auto redundant_payload(std::uint16_t frame, bool count_only)
      -> std::vector<std::byte> {
    std::vector<std::byte> payload(19);
    payload[2] = static_cast<std::byte>(frame >> 8U);
    payload[3] = static_cast<std::byte>(frame & 0xffU);
    payload[5] = std::byte{2};
    payload[6] = count_only ? std::byte{0x20} : std::byte{0x50};
    payload[7] = count_only ? std::byte{2} : std::byte{0x21};
    payload[8] = std::byte{5};
    for (const auto origin : {frame, static_cast<std::uint16_t>(frame - 2),
                              static_cast<std::uint16_t>(frame - 1)}) {
      const auto offset = payload.size();
      payload.resize(offset + 80);
      payload[offset] = static_cast<std::byte>(origin >> 8U);
      payload[offset + 1] = static_cast<std::byte>(origin & 0xffU);
    }
    return payload;
  }
};

TEST_F(TemporalAudio, PreservesOriginsThroughLossAndReordering) {
  for (const std::uint32_t version : {14U, 15U, 20U, 23U, 25U}) {
    const auto profile = takion::resolve_takion_wire_profile(version);
    ASSERT_TRUE(profile) << "version " << version;
    const bool count_only = profile.value().features().contains(
        takion::Feature::av_unit_count_only_encoding);
    for (const std::uint16_t start :
         {std::uint16_t{1}, std::uint16_t{100}, std::uint16_t{65533}}) {
      for (unsigned lost = 0; lost <= 4; ++lost) {
        SCOPED_TRACE("version " + std::to_string(version) + " start " +
                     std::to_string(start) + " lost " + std::to_string(lost));
        remote_play::CloudStreamMediaAssembly assembly{
            remote_play::AudioAssemblyPolicy::console_units, count_only};
        std::vector<takion::client_event::EncodedAudio> received;
        for (unsigned step = 0; step < 12; ++step) {
          if (lost <= 3 && step >= 4 && step < 4 + lost) continue;
          const auto delivered =
              lost == 4 && step >= 4 && step <= 6 ? 10 - step : step;
          const auto frame = static_cast<std::uint16_t>(start + delivered);
          const auto payload = redundant_payload(frame, count_only);
          const takion::PlainPacket wire{takion::PacketType{0x03}, std::nullopt,
                                         payload};
          const auto parsed =
              takion::PacketCodec::decode_av_packet(wire, profile.value());
          ASSERT_TRUE(parsed);
          EXPECT_EQ(parsed.value().units_in_frame_total, 3U);
          EXPECT_EQ(parsed.value().audio_mode, count_only ? 2U : 0U);
          const auto source = takion::PacketCodec::describe_source_datagram(
              parsed.value(), profile.value(), 0, wire.payload);
          EXPECT_EQ(source.unit_total, 3U);
          EXPECT_EQ(source.unit_redundant, 2U);
          EXPECT_EQ(source.unit_size, 80U);
          const auto events = assembly.consume(parsed.value());
          ASSERT_TRUE(events);
          for (const auto& event : events.value())
            received.push_back(Rig::audio_of(event));
          const auto repeated = assembly.consume(parsed.value());
          ASSERT_TRUE(repeated);
          EXPECT_TRUE(repeated.value().empty());
        }
        const auto first =
            static_cast<std::uint16_t>(start == 1 ? 1 : start - 2);
        const std::size_t count = start == 1 ? 12 : 14;
        ASSERT_EQ(received.size(), count);
        for (std::size_t index = 0; index < count; ++index) {
          const auto frame = static_cast<std::uint16_t>(first + index);
          const auto& bytes = received[index].bytes;
          if (lost == 3 && frame == static_cast<std::uint16_t>(start + 4)) {
            EXPECT_TRUE(bytes.empty()) << "frame " << frame;
            continue;
          }
          ASSERT_EQ(bytes.size(), 80U) << "frame " << frame;
          EXPECT_EQ(std::to_integer<unsigned>(bytes[0]),
                    static_cast<unsigned>(frame >> 8U))
              << "frame " << frame;
          EXPECT_EQ(std::to_integer<unsigned>(bytes[1]),
                    static_cast<unsigned>(frame & 0xffU))
              << "frame " << frame;
        }
      }
    }
  }
}

TEST_F(TemporalAudio, DoesNotChangeHapticsOrOtherModeCounts) {
  const auto profile = takion::resolve_takion_wire_profile(20);
  ASSERT_TRUE(profile);
  auto packet = Rig::audio_unit(100, 0, Rig::bytes({100, 101, 99}));
  packet.units_in_frame_total = 3;
  packet.units_in_frame_fec = 2;
  for (const std::uint8_t mode :
       {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{3}}) {
    packet.audio_mode = mode;
    const auto source = takion::PacketCodec::describe_source_datagram(
        packet, profile.value(), 0, packet.data);
    EXPECT_EQ(source.unit_total, 3U) << "mode " << +mode;
    EXPECT_EQ(source.unit_redundant, 1U) << "mode " << +mode;
  }
  packet.audio_mode = remote_play::audio_mode_temporal_redundancy;
  packet.is_haptics = true;
  const auto source = takion::PacketCodec::describe_source_datagram(
      packet, profile.value(), 0, packet.data);
  EXPECT_EQ(source.unit_total, 3U);
  EXPECT_EQ(source.unit_redundant, 1U);
  remote_play::ConsoleAudioAssembler assembler;
  const auto frames = assembler.consume(packet, true);
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 2U);
  EXPECT_EQ(frames.value()[0].bytes, Rig::bytes({100}));
  EXPECT_EQ(frames.value()[1].bytes, Rig::bytes({101}));
}

TEST_F(TemporalAudio, RejectsInvalidHistoryCounts) {
  auto packet = Rig::audio_unit(100, 0, std::vector<std::byte>(240));
  packet.units_in_frame_total = 3;
  packet.audio_mode = remote_play::audio_mode_temporal_redundancy;
  for (const std::uint16_t count :
       {std::uint16_t{3}, std::uint16_t{4}, std::uint16_t{4095}}) {
    packet.units_in_frame_fec = count;
    remote_play::ConsoleAudioAssembler assembler;
    EXPECT_FALSE(assembler.consume(packet, true)) << "count " << count;
  }
}

}  // namespace
}  // namespace remote_play_test
