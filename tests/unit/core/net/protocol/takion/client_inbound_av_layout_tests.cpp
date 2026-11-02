#include "tests/unit/core/net/protocol/takion/client_inbound_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

namespace takion_test {
namespace {

class TakionInboundAvLayout : public ::testing::Test {
 protected:
  static auto version9_video() -> std::vector<std::byte> {
    std::vector<std::byte> video(24);
    video[1] = std::byte{0x11};
    video[3] = std::byte{0x22};
    video[5] = std::byte{0x20};
    video[6] = std::byte{0x04};
    video[8] = std::byte{0x3c};
    video[20] = std::byte{0x55};
    return video;
  }

  static auto version9_audio() -> std::vector<std::byte> {
    std::vector<std::byte> audio(19);
    audio[3] = std::byte{0x33};
    audio[4] = std::byte{0x01};
    audio[5] = std::byte{0x02};
    audio[8] = std::byte{0x05};
    audio[18] = std::byte{0x66};
    return audio;
  }

  static void send(InboundHarness& harness, std::uint8_t type,
                   std::vector<std::byte> payload) {
    harness.receive(InboundWire::plain_packet(type, std::move(payload),
                                              harness.config.bootstrap_version),
                    harness.session());
  }
};

TEST_F(TakionInboundAvLayout, Version9TypesVideoAndAudioUnits) {
  InboundHarness harness{InboundWire::fixed_config(9)};
  send(harness, 0x02, version9_video());
  ASSERT_EQ(harness.events.events.size(), 1U);
  const auto* first =
      harness.events.last<takion::client_event::AvUnitReceived>();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->values.frame_index, 0x22U);
  EXPECT_EQ(first->values.unit_index, 1U);
  EXPECT_EQ(first->values.data,
            (std::vector<std::byte>{std::byte{0x55}, std::byte{0}, std::byte{0},
                                    std::byte{0}}));

  send(harness, 0x03, version9_audio());
  ASSERT_EQ(harness.events.events.size(), 2U);
  const auto* second =
      harness.events.last<takion::client_event::AvUnitReceived>();
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->values.frame_index, 0x33U);
  EXPECT_FALSE(second->values.uses_nalu_info_structs);
  EXPECT_EQ(second->values.data, std::vector<std::byte>{std::byte{0x66}});
  EXPECT_FALSE(std::holds_alternative<takion::client_event::Haptics>(
      harness.events.events.back()));
}

TEST_F(TakionInboundAvLayout, Version9RejectsTheAudioExtensionAndShortUnits) {
  InboundHarness harness{InboundWire::fixed_config(9)};
  std::vector<std::byte> extended(22);
  extended[3] = std::byte{0x33};
  extended[8] = std::byte{0x05};
  extended[21] = std::byte{0x66};
  send(harness, 0x13, extended);
  ASSERT_EQ(harness.events.events.size(), 1U);
  EXPECT_EQ(harness.events.last_rejection(),
            takion::ClientError::packet_decode_failure);

  auto audio = version9_audio();
  audio.resize(17);
  send(harness, 0x03, audio);
  ASSERT_EQ(harness.events.events.size(), 2U);
  EXPECT_EQ(harness.events.last_rejection(),
            takion::ClientError::packet_decode_failure);

  auto video = version9_video();
  video.resize(19);
  send(harness, 0x02, video);
  ASSERT_EQ(harness.events.events.size(), 3U);
  EXPECT_EQ(harness.events.last_rejection(),
            takion::ClientError::packet_decode_failure);
}

TEST_F(TakionInboundAvLayout, Version7NaluFlaggedAudioIsAudioNotHaptics) {
  InboundHarness harness{InboundWire::fixed_config(7)};
  std::vector<std::byte> flagged(0x14 + 8);
  flagged[2] = std::byte{0x01};
  flagged[0x14 + 4] = std::byte{0x77};
  send(harness, 0x13, flagged);
  ASSERT_EQ(harness.events.events.size(), 1U);
  const auto* nalu = harness.events.last<takion::client_event::EncodedAudio>();
  ASSERT_NE(nalu, nullptr);
  EXPECT_TRUE(nalu->uses_nalu_info_structs);
  EXPECT_EQ(nalu->bytes, flagged);

  std::vector<std::byte> plain_audio(0x11 + 8);
  plain_audio[0x11 + 4] = std::byte{0x66};
  send(harness, 0x03, plain_audio);
  ASSERT_EQ(harness.events.events.size(), 2U);
  const auto* audio = harness.events.last<takion::client_event::EncodedAudio>();
  ASSERT_NE(audio, nullptr);
  EXPECT_FALSE(audio->uses_nalu_info_structs);
  EXPECT_EQ(audio->bytes, plain_audio);
}

TEST_F(TakionInboundAvLayout, Version7NaluFlaggedVideoCarriesTheFlag) {
  InboundHarness harness{InboundWire::fixed_config(7)};
  std::vector<std::byte> flagged(0x17 + 8);
  send(harness, 0x12, flagged);
  ASSERT_EQ(harness.events.events.size(), 1U);
  const auto* nalu = harness.events.last<takion::client_event::EncodedVideo>();
  ASSERT_NE(nalu, nullptr);
  EXPECT_TRUE(nalu->uses_nalu_info_structs);
  EXPECT_EQ(nalu->bytes, flagged);

  std::vector<std::byte> plain_video(0x14 + 8);
  send(harness, 0x02, plain_video);
  ASSERT_EQ(harness.events.events.size(), 2U);
  const auto* video = harness.events.last<takion::client_event::EncodedVideo>();
  ASSERT_NE(video, nullptr);
  EXPECT_FALSE(video->uses_nalu_info_structs);
  EXPECT_EQ(video->bytes, plain_video);
}

}  // namespace
}  // namespace takion_test
