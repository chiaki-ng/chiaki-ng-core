#include "tests/support/random.hpp"
#include "tests/unit/core/net/transport/rudp/frame_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rudp_test {
namespace {

using libchiaking::net::transport::rudp::SwitchPacketError;

class RudpSwitchRequest : public FrameCodecRig {
 protected:
  static auto marked_nonce() -> std::array<std::byte, 16> {
    std::array<std::byte, 16> nonce{};
    nonce[0] = std::byte{0xaa};
    nonce[15] = std::byte{0xbb};
    return nonce;
  }

  static auto counting_nonce() -> std::array<std::byte, 16> {
    std::array<std::byte, 16> nonce{};
    for (unsigned index = 0; index < nonce.size(); ++index)
      nonce[index] = static_cast<std::byte>(index);
    return nonce;
  }
};

TEST_F(RudpSwitchRequest, MatchesTheGoldenPacket) {
  const auto result = FrameCodec::encode_switch_to_stream_connection_request(
      0x1234, counting_nonce());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().size(), 34U);
  EXPECT_EQ(result.value(), golden_switch);
}

TEST_F(RudpSwitchRequest, PlacesTheCounterAndNonceAtEitherCounterBoundary) {
  const auto nonce = marked_nonce();
  for (const std::uint16_t counter :
       {std::uint16_t{0x0000}, std::uint16_t{0xFFFF}}) {
    SCOPED_TRACE(counter);
    const auto result =
        FrameCodec::encode_switch_to_stream_connection_request(counter, nonce);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 34U);
    EXPECT_EQ(result.value()[8], static_cast<std::byte>(counter >> 8));
    EXPECT_EQ(result.value()[9], static_cast<std::byte>(counter & 0xFF));
    EXPECT_EQ(result.value()[18], std::byte{0xaa});
    EXPECT_EQ(result.value()[33], std::byte{0xbb});
    EXPECT_EQ(result.value()[1], std::byte{0x22});
    const auto back = FrameCodec::decode(result.value());
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back.value().size(), 1U);
    EXPECT_EQ(back.value()[0].type, 0x0230);
    ASSERT_EQ(back.value()[0].payload.size(), 26U);
    for (std::size_t index = 0; index < 8; ++index)
      EXPECT_EQ(back.value()[0].payload[2 + index], golden_switch[10 + index]);
    EXPECT_EQ(back.value()[0].payload[10], std::byte{0xaa});
    EXPECT_EQ(back.value()[0].payload[25], std::byte{0xbb});
  }
}

TEST_F(RudpSwitchRequest, IsDeterministic) {
  const auto nonce = marked_nonce();
  const auto first =
      FrameCodec::encode_switch_to_stream_connection_request(0x1234, nonce);
  const auto second =
      FrameCodec::encode_switch_to_stream_connection_request(0x1234, nonce);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first.value(), second.value());
}

TEST_F(RudpSwitchRequest, DrawsItsNonceFromTheRandomSource) {
  test_support::CountingRandom random;
  const auto packet = FrameCodec::encode_switch_request_random(0xFFFF, random);
  ASSERT_TRUE(packet.has_value());
  ASSERT_EQ(packet.value().size(), 34U);
  EXPECT_EQ(packet.value()[8], std::byte{0xff});
  EXPECT_EQ(packet.value()[9], std::byte{0xff});
  EXPECT_EQ(random.calls, 1U);
  EXPECT_EQ(random.last_size, 16U);
  const auto expected = FrameCodec::encode_switch_to_stream_connection_request(
      0xFFFF, counting_nonce());
  ASSERT_TRUE(expected.has_value());
  EXPECT_EQ(packet.value(), expected.value());
}

TEST_F(RudpSwitchRequest, FailsWhenTheRandomSourceFails) {
  test_support::CountingRandom random;
  random.fail = true;
  const auto failed = FrameCodec::encode_switch_request_random(0x1234, random);
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error(), SwitchPacketError::random_failed);
  EXPECT_EQ(random.calls, 1U);
}

class RudpCtrlHttpResponse : public FrameCodecRig {
 protected:
  const Bytes response = {std::byte{0x12}, std::byte{0x34}, std::byte{0x48},
                          std::byte{0x54}, std::byte{0x54}, std::byte{0x50},
                          std::byte{0x0D}, std::byte{0x0A}, std::byte{0x0D},
                          std::byte{0x0A}, std::byte{0xDE}, std::byte{0xAD}};
};

TEST_F(RudpCtrlHttpResponse, ReadsTheCounterHeaderAndBody) {
  const auto decoded = FrameCodec::decode_ctrl_http_response(response);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().remote_counter, 0x1235);
  EXPECT_EQ(decoded.value().header_size, 8U);
  EXPECT_EQ(
      decoded.value().received,
      (Bytes{std::byte{0x48}, std::byte{0x54}, std::byte{0x54}, std::byte{0x50},
             std::byte{0x0D}, std::byte{0x0A}, std::byte{0x0D}, std::byte{0x0A},
             std::byte{0xDE}, std::byte{0xAD}}));
}

TEST_F(RudpCtrlHttpResponse, WrapsTheCounter) {
  const Bytes wrap = {std::byte{0xFF}, std::byte{0xFF}, std::byte{0x0D},
                      std::byte{0x0A}, std::byte{0x0D}, std::byte{0x0A}};
  const auto decoded = FrameCodec::decode_ctrl_http_response(wrap);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().remote_counter, 0);
  EXPECT_EQ(decoded.value().header_size, 4U);
  EXPECT_EQ(decoded.value().received.size(), 4U);
}

TEST_F(RudpCtrlHttpResponse, RefusesAnEmptyOrShortPayload) {
  EXPECT_FALSE(FrameCodec::decode_ctrl_http_response({}));
  const Bytes short_payload = {std::byte{0x00}, std::byte{0x01}};
  const auto decoded = FrameCodec::decode_ctrl_http_response(short_payload);
  ASSERT_FALSE(decoded);
  EXPECT_EQ(decoded.error(), FrameError::malformed);
}

TEST_F(RudpCtrlHttpResponse, RefusesAnUnfinishedHeader) {
  const Bytes partial = {std::byte{0x00}, std::byte{0x01}, std::byte{0x0D},
                         std::byte{0x0A}, std::byte{0x0D}};
  const auto decoded = FrameCodec::decode_ctrl_http_response(partial);
  ASSERT_FALSE(decoded);
  EXPECT_EQ(decoded.error(), FrameError::malformed);
}

TEST_F(RudpCtrlHttpResponse, AcceptsTheLongestPayloadAndNoLonger) {
  Bytes longest(4087, std::byte{0x00});
  longest[2] = std::byte{0x0D};
  longest[3] = std::byte{0x0A};
  longest[4] = std::byte{0x0D};
  longest[5] = std::byte{0x0A};
  const auto accepted = FrameCodec::decode_ctrl_http_response(longest);
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted.value().received.size(), 4085U);
  longest.push_back(std::byte{0x00});
  const auto refused = FrameCodec::decode_ctrl_http_response(longest);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), FrameError::too_large);
}

TEST_F(RudpCtrlHttpResponse, FindsTheResponseFrameAmongOthers) {
  Bytes packet;
  append_frame(
      packet, 0x2030,
      {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}});
  append_frame(packet, 0x0230, response);
  append_frame(packet, 0x2430,
               {std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                std::byte{0x92}, std::byte{0x00}, std::byte{0x00}});
  const auto decoded = FrameCodec::decode_ctrl_http_response_packet(packet);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().remote_counter, 0x1235);
  EXPECT_EQ(decoded.value().header_size, 8U);
  ASSERT_EQ(decoded.value().received.size(), 10U);
  EXPECT_EQ(decoded.value().received.back(), std::byte{0xAD});
  packet.pop_back();
  EXPECT_TRUE(FrameCodec::decode_ctrl_http_response_packet(packet));
}

TEST_F(RudpCtrlHttpResponse, ReadsAStandaloneResponseFrame) {
  const auto decoded =
      FrameCodec::decode_ctrl_http_response_packet(packet_of(0x0630, response));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().remote_counter, 0x1235);
}

TEST_F(RudpCtrlHttpResponse, RefusesAPacketWithoutAResponseFrame) {
  EXPECT_FALSE(FrameCodec::decode_ctrl_http_response_packet(packet_of(
      0x2030,
      {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}})));
}

TEST_F(RudpCtrlHttpResponse, RefusesAMalformedResponseFrame) {
  EXPECT_FALSE(FrameCodec::decode_ctrl_http_response_packet(
      packet_of(0x0230, {std::byte{0x12}, std::byte{0x34}, std::byte{0x41}})));
}

TEST_F(RudpCtrlHttpResponse, AcceptsTwoResponseFrames) {
  Bytes ambiguous;
  append_frame(ambiguous, 0x0230, response);
  append_frame(ambiguous, 0x0630, response);
  EXPECT_TRUE(FrameCodec::decode_ctrl_http_response_packet(ambiguous));
}

class RudpSessionMessage : public FrameCodecRig {};

TEST_F(RudpSessionMessage, MatchesTheGoldenPacketAndRoundTrips) {
  const Bytes request = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                         std::byte{0xEF}};
  Bytes expected;
  append_frame(
      expected, 0x2030,
      {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}});
  append_frame(expected, 0x0230,
               {std::byte{0x12}, std::byte{0x34}, std::byte{0xDE},
                std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}});
  const auto encoded =
      FrameCodec::encode_session_message(0x1234, 0x5678, request);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded.value(), expected);
  const auto decoded = FrameCodec::decode(encoded.value());
  ASSERT_TRUE(decoded);
  ASSERT_EQ(decoded.value().size(), 2U);
  EXPECT_EQ(decoded.value()[0].type, 0x2030);
  EXPECT_EQ(decoded.value()[0].payload,
            (Bytes{std::byte{0x12}, std::byte{0x34}, std::byte{0x56},
                   std::byte{0x78}}));
  EXPECT_EQ(decoded.value()[1].type, 0x0230);
  EXPECT_EQ(decoded.value()[1].payload,
            (Bytes{std::byte{0x12}, std::byte{0x34}, std::byte{0xDE},
                   std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}}));
}

TEST_F(RudpSessionMessage, WritesTheHighestCounters) {
  const auto encoded = FrameCodec::encode_session_message(0xFFFF, 0xFFFF, {});
  ASSERT_TRUE(encoded);
  ASSERT_GE(encoded.value().size(), 12U);
  EXPECT_EQ(encoded.value()[8], std::byte{0xFF});
  EXPECT_EQ(encoded.value()[9], std::byte{0xFF});
  EXPECT_EQ(encoded.value()[10], std::byte{0xFF});
  EXPECT_EQ(encoded.value()[11], std::byte{0xFF});
}

TEST_F(RudpSessionMessage, AcceptsTheLongestRequestAndNoLonger) {
  const auto encoded =
      FrameCodec::encode_session_message(1, 2, Bytes(4085, std::byte{0x41}));
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded.value().size(), 4107U);
  const auto rejected =
      FrameCodec::encode_session_message(1, 2, Bytes(4086, std::byte{0x41}));
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), FrameError::too_large);
}

class RudpAckMessage : public FrameCodecRig {};

TEST_F(RudpAckMessage, MatchesTheGoldenPacketAndRoundTrips) {
  const Bytes expected = {std::byte{0xC0}, std::byte{0x0E}, std::byte{0x24},
                          std::byte{0x4F}, std::byte{0x24}, std::byte{0x4F},
                          std::byte{0x24}, std::byte{0x30}, std::byte{0x12},
                          std::byte{0x34}, std::byte{0x56}, std::byte{0x78},
                          std::byte{0x00}, std::byte{0x92}};
  const auto encoded = FrameCodec::encode_ack_message(0x1234, 0x5678);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded.value(), expected);
  const auto decoded = FrameCodec::decode(encoded.value());
  ASSERT_TRUE(decoded);
  ASSERT_EQ(decoded.value().size(), 1U);
  EXPECT_EQ(decoded.value()[0].type, 0x2430);
  EXPECT_EQ(decoded.value()[0].payload,
            (Bytes{std::byte{0x12}, std::byte{0x34}, std::byte{0x56},
                   std::byte{0x78}, std::byte{0x00}, std::byte{0x92}}));
}

TEST_F(RudpAckMessage, WritesTheHighestCounters) {
  const auto encoded = FrameCodec::encode_ack_message(0xFFFF, 0xFFFF);
  ASSERT_TRUE(encoded);
  ASSERT_GE(encoded.value().size(), 12U);
  EXPECT_EQ(encoded.value()[8], std::byte{0xFF});
  EXPECT_EQ(encoded.value()[9], std::byte{0xFF});
  EXPECT_EQ(encoded.value()[10], std::byte{0xFF});
  EXPECT_EQ(encoded.value()[11], std::byte{0xFF});
}

}  // namespace
}  // namespace rudp_test
