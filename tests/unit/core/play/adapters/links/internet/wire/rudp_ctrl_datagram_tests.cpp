#include "src/core/net/protocol/remote_play/rudp_ctrl.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
using rudp_ctrl::CtrlFrames;
using rudp_ctrl::ExchangeChannel;

class RudpCtrlDatagram : public ::testing::Test {
 protected:
  void expect_data_frame(std::uint16_t type, std::size_t offset,
                         std::uint16_t expected_ack) {
    SCOPED_TRACE(type);
    std::vector<std::byte> payload(offset, std::byte{0});
    payload[0] = std::byte{0x12};
    payload[1] = std::byte{0x34};
    if (offset >= 4) {
      payload[2] = std::byte{0x56};
      payload[3] = std::byte{0x78};
    }
    const auto ctrl = CtrlFrames::stream_switch_header();
    payload.insert(payload.end(), ctrl.begin(), ctrl.end());
    const auto decoded = remote_play::RudpCtrl::decode_ongoing(
        ExchangeChannel::packet(type, payload));
    ASSERT_TRUE(decoded);
    ASSERT_FALSE(decoded.value().empty());
    const auto& front = decoded.value().front();
    EXPECT_EQ(front.subtype, rudp::FrameCodec::subtype_of(type));
    EXPECT_EQ(front.remote_counter, 0x1235);
    EXPECT_EQ(front.ctrl_frame, ctrl);
    if (type == 0x0230)
      EXPECT_FALSE(front.ack_counter);
    else
      EXPECT_EQ(front.ack_counter, expected_ack);
  }

  static auto switch_payload() -> std::vector<std::byte> {
    const auto ctrl = CtrlFrames::stream_switch_header();
    std::vector<std::byte> payload{std::byte{0}, std::byte{1}};
    payload.insert(payload.end(), ctrl.begin(), ctrl.end());
    return payload;
  }
};

TEST_F(RudpCtrlDatagram, ReadsTheCountersOfEveryDataSubtype) {
  expect_data_frame(0x0230, 2, 0);
  expect_data_frame(0x1230, 8, 0x5678);
  expect_data_frame(0x2630, 6, 0x5678);
  expect_data_frame(0x3630, 2, 0);
}

TEST_F(RudpCtrlDatagram, ReadsAnAcknowledgementWithoutACtrlFrame) {
  const auto acknowledgement = remote_play::RudpCtrl::decode_ongoing(
      ExchangeChannel::packet(0x2430, {std::byte{0}, std::byte{0},
                                       std::byte{0x12}, std::byte{0x34}}));
  ASSERT_TRUE(acknowledgement);
  ASSERT_FALSE(acknowledgement.value().empty());
  EXPECT_EQ(acknowledgement.value().front().subtype, rudp::Subtype::ack);
  EXPECT_EQ(acknowledgement.value().front().ack_counter, 0x1234);
  EXPECT_TRUE(acknowledgement.value().front().ctrl_frame.empty());
}

TEST_F(RudpCtrlDatagram, RefusesAnAcknowledgementShorterThanFourBytes) {
  for (std::size_t size = 0; size < 4; ++size) {
    SCOPED_TRACE(size);
    EXPECT_FALSE(remote_play::RudpCtrl::decode_ongoing(
        ExchangeChannel::packet(0x2430, std::vector<std::byte>(size))));
  }
}

TEST_F(RudpCtrlDatagram, LeavesTheCtrlFrameEmptyForAnUnknownSubtype) {
  const auto decoded = remote_play::RudpCtrl::decode_ongoing(
      ExchangeChannel::packet(0x9930, std::vector<std::byte>(4)));
  ASSERT_TRUE(decoded);
  ASSERT_FALSE(decoded.value().empty());
  EXPECT_TRUE(decoded.value().front().ctrl_frame.empty());
}

TEST_F(RudpCtrlDatagram, LeavesTheCtrlFrameEmptyWhenItIsShort) {
  const auto decoded =
      remote_play::RudpCtrl::decode_ongoing(ExchangeChannel::packet(
          0x1230, {std::byte{0}, std::byte{1}, std::byte{0}, std::byte{2}}));
  ASSERT_TRUE(decoded);
  ASSERT_FALSE(decoded.value().empty());
  EXPECT_TRUE(decoded.value().front().ctrl_frame.empty());
}

TEST_F(RudpCtrlDatagram, LeavesTheCtrlFrameEmptyWhenBytesTrailIt) {
  auto trailing = CtrlFrames::stream_switch_header();
  trailing.push_back(std::byte{0xFF});
  trailing.insert(trailing.begin(), {std::byte{0}, std::byte{1}});
  const auto decoded = remote_play::RudpCtrl::decode_ongoing(
      ExchangeChannel::packet(0x0230, trailing));
  ASSERT_TRUE(decoded);
  ASSERT_FALSE(decoded.value().empty());
  EXPECT_TRUE(decoded.value().front().ctrl_frame.empty());
}

TEST_F(RudpCtrlDatagram, KeepsAnAcknowledgementAndAFrameSentTogether) {
  const auto payload = switch_payload();
  const std::array<rudp::Frame, 2> combined{
      rudp::Frame{0x2430, std::vector<std::byte>(6)},
      rudp::Frame{0x0230, payload}};
  const auto combined_packet = rudp::FrameCodec::encode(combined);
  ASSERT_TRUE(combined_packet);
  const auto combined_ctrl =
      remote_play::RudpCtrl::decode_ongoing(combined_packet.value());
  ASSERT_TRUE(combined_ctrl);
  ASSERT_EQ(combined_ctrl.value().size(), 2U);
  EXPECT_TRUE(combined_ctrl.value()[0].ctrl_frame.empty());
  EXPECT_EQ(combined_ctrl.value()[1].ctrl_frame,
            CtrlFrames::stream_switch_header());
}

TEST_F(RudpCtrlDatagram, KeepsEveryCtrlFrameOfAPacket) {
  const auto payload = switch_payload();
  const std::array<rudp::Frame, 2> nested_frames{rudp::Frame{0x0230, payload},
                                                 rudp::Frame{0x0230, payload}};
  const auto nested_frames_packet = rudp::FrameCodec::encode(nested_frames);
  ASSERT_TRUE(nested_frames_packet);
  const auto nested =
      remote_play::RudpCtrl::decode_ongoing(nested_frames_packet.value());
  ASSERT_TRUE(nested);
  ASSERT_EQ(nested.value().size(), 2U);
  const auto ctrl = CtrlFrames::stream_switch_header();
  EXPECT_EQ(nested.value()[0].ctrl_frame, ctrl);
  EXPECT_EQ(nested.value()[1].ctrl_frame, ctrl);
}

}  // namespace
}  // namespace play_test
