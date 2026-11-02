#include "src/core/net/protocol/remote_play/stream/microphone_sender.hpp"
#include "tests/unit/core/net/protocol/remote_play/stream/active_session_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

class ActiveSessionMicrophoneSender : public ::testing::Test {
 protected:
  auto frame_of(std::uint8_t value) -> std::span<const std::byte> {
    frame.fill(std::byte{value});
    return std::span<const std::byte>{frame};
  }

  static auto expected_packet(std::vector<std::byte> header,
                              std::array<std::uint8_t, 3> frames)
      -> std::vector<std::byte> {
    for (const auto value : frames)
      header.insert(header.end(), 40, std::byte{value});
    return header;
  }

  remote_play::MicrophoneSender sender;
  std::array<std::byte, 40> frame{};
};

TEST_F(ActiveSessionMicrophoneSender, PacketsCarryTheExactReferenceHeaders) {
  const auto ps4 = sender.push(frame_of(1), false, false);
  ASSERT_TRUE(ps4);
  EXPECT_EQ(ps4->payload_offset, 19U);
  EXPECT_EQ(
      ps4->packet,
      expected_packet(
          {std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
           std::byte{0x01}, std::byte{0x00}, std::byte{0x02}, std::byte{0x28},
           std::byte{0x21}, std::byte{0x05}, std::byte{0}, std::byte{0},
           std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
           std::byte{0}, std::byte{0}},
          {1, 1, 1}));
  const auto ps5 = sender.push(frame_of(2), true, true);
  ASSERT_TRUE(ps5);
  EXPECT_EQ(ps5->payload_offset, 20U);
  EXPECT_EQ(
      ps5->packet,
      expected_packet(
          {std::byte{0x03}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
           std::byte{0x02}, std::byte{0x00}, std::byte{0x02}, std::byte{0x20},
           std::byte{0x01}, std::byte{0x05}, std::byte{0},    std::byte{0},
           std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0},
           std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0}},
          {2, 1, 1}));
  const auto third = sender.push(frame_of(3), true, false);
  ASSERT_TRUE(third);
  EXPECT_EQ(
      std::vector<std::byte>(third->packet.begin() + 20, third->packet.end()),
      expected_packet({}, {3, 1, 2}));
  const auto fourth = sender.push(frame_of(4), true, false);
  ASSERT_TRUE(fourth);
  EXPECT_EQ(
      std::vector<std::byte>(fourth->packet.begin() + 20, fourth->packet.end()),
      expected_packet({}, {4, 2, 3}));
}

TEST(ActiveSessionMicrophoneHeader, IsTaggedFromVersion11Onwards) {
  using takion::AvLayout;
  EXPECT_FALSE(remote_play::microphone_header_tagged(AvLayout::version_7));
  EXPECT_FALSE(
      remote_play::microphone_header_tagged(AvLayout::versions_9_to_10));
  for (const auto layout :
       {AvLayout::version_11, AvLayout::versions_12_to_14,
        AvLayout::versions_15_to_19, AvLayout::versions_20_to_21,
        AvLayout::versions_22_to_23, AvLayout::versions_24_to_25})
    EXPECT_TRUE(remote_play::microphone_header_tagged(layout))
        << ::testing::PrintToString(layout);
}

TEST(ActiveSessionMicrophone, FramesGoOutAsReferenceAudioPackets) {
  ActiveSessionRig rig{9};
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  auto& session = *rig.owner;
  std::array<std::byte, 40> frame{};
  const auto start = rig.state->sent.size();
  for (std::uint8_t value = 1; value <= 4; ++value) {
    frame.fill(static_cast<std::byte>(value));
    EXPECT_TRUE(session.post_microphone(frame)) << +value;
    ASSERT_TRUE(session.pump_once()) << +value;
  }
  EXPECT_TRUE(
      session.post_microphone(std::span<const std::byte>{frame}.first(39)));
  ASSERT_TRUE(session.pump_once());
  std::vector<std::vector<std::byte>> packets;
  for (std::size_t index = start; index < rig.state->sent.size(); ++index) {
    const auto& wire = rig.state->sent[index];
    if (!wire.empty() && wire[0] == std::byte{0x03}) packets.push_back(wire);
  }
  ASSERT_EQ(packets.size(), 4U);
  ASSERT_EQ(packets[0].size(), 19U + 120U);
  ASSERT_EQ(packets[1].size(), 19U + 120U);
  ASSERT_GE(packets[2].size(), 100U);
  ASSERT_GE(packets[3].size(), 100U);
  EXPECT_EQ(packets[0][2], std::byte{0});
  EXPECT_EQ(packets[0][4], std::byte{1});
  EXPECT_EQ(packets[3][2], std::byte{3});
  EXPECT_EQ(packets[3][4], std::byte{4});
  EXPECT_EQ(packets[0][19], std::byte{1});
  EXPECT_EQ(packets[0][59], std::byte{1});
  EXPECT_EQ(packets[0][99], std::byte{1});
  EXPECT_EQ(packets[2][19], std::byte{3});
  EXPECT_EQ(packets[2][59], std::byte{1});
  EXPECT_EQ(packets[2][99], std::byte{2});
  EXPECT_EQ(packets[3][19], std::byte{4});
  EXPECT_EQ(packets[3][59], std::byte{2});
  EXPECT_EQ(packets[3][99], std::byte{3});
  EXPECT_TRUE(session.request_stop());
  const auto stopped = session.post_microphone(frame);
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error(), remote_play::ActiveSessionError::not_ready);
}

}  // namespace
}  // namespace remote_play_test
