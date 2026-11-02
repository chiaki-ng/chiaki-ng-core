#include "tests/unit/core/net/protocol/remote_play/stream/cloud_stream_media_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace remote_play_test {
namespace {

using Rig = CloudStreamMediaRig;

TEST(ConsoleAudioAssembly, ConcealsAWholeDiscontinuityInOnePacket) {
  remote_play::ConsoleAudioAssembler audio;
  auto packet = Rig::audio_unit(1, 0, Rig::bytes({1, 2, 3}));
  packet.units_in_frame_total = 3;
  packet.units_in_frame_fec = 3;
  const auto opening = audio.consume(packet, true);
  ASSERT_TRUE(opening);
  EXPECT_EQ(opening.value().size(), 3U);
  packet.frame_index = 30000;
  packet.units_in_frame_total = 256;
  packet.units_in_frame_fec = 256;
  packet.data.assign(256, std::byte{7});
  auto frames = audio.consume(packet, true);
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 29996U + 256U);
  for (std::size_t index = 0; index < 29996; ++index) {
    EXPECT_EQ(frames.value()[index].index, 4 + index) << "index " << index;
    EXPECT_TRUE(frames.value()[index].bytes.empty()) << "index " << index;
  }
  for (std::size_t index = 29996; index < frames.value().size(); ++index) {
    EXPECT_EQ(frames.value()[index].index, 4 + index) << "index " << index;
    EXPECT_EQ(frames.value()[index].bytes, Rig::bytes({7}))
        << "index " << index;
  }
  packet.frame_index = 30256;
  packet.units_in_frame_total = 1;
  packet.units_in_frame_fec = 1;
  packet.data.resize(1);
  frames = audio.consume(packet, true);
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 1U);
  EXPECT_EQ(frames.value().front().index, 30256U);
}

TEST(ConsoleAudioAssembly, ConcealsEveryFrameOfALongGap) {
  remote_play::ConsoleAudioAssembler audio;
  auto packet = Rig::audio_unit(1, 0, Rig::bytes({1, 2, 3}));
  packet.units_in_frame_total = 3;
  packet.units_in_frame_fec = 3;
  const auto opening = audio.consume(packet, true);
  ASSERT_TRUE(opening);
  EXPECT_EQ(opening.value().size(), 3U);
  packet.frame_index = 7;
  packet.data = Rig::bytes({4, 5, 6});
  auto frames = audio.consume(packet, true);
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 6U);
  for (std::size_t index = 0; index < 3; ++index) {
    EXPECT_EQ(frames.value()[index].index, 4 + index) << "index " << index;
    EXPECT_TRUE(frames.value()[index].bytes.empty()) << "index " << index;
  }
  EXPECT_EQ(frames.value()[3].index, 7U);
  EXPECT_EQ(frames.value()[5].index, 9U);
  packet.frame_index = 40;
  packet.data = Rig::bytes({7, 8, 9});
  frames = audio.consume(packet, true);
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 33U);
  for (std::size_t index = 0; index < 30; ++index) {
    EXPECT_EQ(frames.value()[index].index, 10 + index) << "index " << index;
    EXPECT_TRUE(frames.value()[index].bytes.empty()) << "index " << index;
  }
  EXPECT_EQ(frames.value()[30].index, 40U);
  EXPECT_EQ(frames.value()[30].bytes, Rig::bytes({7}));
  EXPECT_EQ(frames.value()[32].index, 42U);
}

TEST(ConsoleAudioAssembly, PlaysALegacyPacketWithoutSourceUnits) {
  remote_play::ConsoleAudioAssembler audio;
  auto packet = Rig::audio_unit(10, 0, Rig::bytes({8, 9}));
  packet.units_in_frame_total = 2;
  packet.units_in_frame_fec = 0x0120;
  auto frames = audio.consume(packet, false);
  ASSERT_TRUE(frames);
  EXPECT_TRUE(frames.value().empty());
  packet.units_in_frame_fec = 0x2000;
  EXPECT_FALSE(audio.consume(packet, true));
  packet.data = Rig::bytes({10});
  packet.units_in_frame_total = 1;
  packet.units_in_frame_fec = 0x0101;
  frames = audio.consume(packet, false);
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 3U);
  for (std::size_t index = 0; index < 3; ++index) {
    EXPECT_EQ(frames.value()[index].index, 8 + index) << "index " << index;
    EXPECT_EQ(frames.value()[index].bytes,
              std::vector<std::byte>{static_cast<std::byte>(8 + index)})
        << "index " << index;
  }
}

TEST(ConsoleAudioAssembly, HapticsPreserveCountOnlyPacketGeometry) {
  remote_play::ConsoleAudioAssembler audio;
  auto packet =
      Rig::audio_unit(1, 0, std::vector<std::byte>(300, std::byte{7}));
  packet.is_haptics = true;
  packet.units_in_frame_total = 300;
  packet.units_in_frame_fec = 300;
  auto frames = audio.consume(packet, true);
  ASSERT_TRUE(frames);
  EXPECT_EQ(frames.value().size(), 300U);
  packet.frame_index = 301;
  packet.data.assign(5000, std::byte{9});
  packet.units_in_frame_total = 1;
  packet.units_in_frame_fec = 1;
  frames = audio.consume(packet, true);
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 1U);
  EXPECT_EQ(frames.value().front().bytes.size(), 5000U);
}

}  // namespace
}  // namespace remote_play_test
