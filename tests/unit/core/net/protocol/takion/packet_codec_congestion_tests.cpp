#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionCongestionPackets : public ::testing::Test {
 protected:
  const takion::CongestionValues values{0x1122, 0x3344, 0x5566};
  const std::vector<std::byte> expected_payload =
      TakionCodecRig::bytes({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x00, 0x00,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
};

TEST_F(TakionCongestionPackets, EncodeTheReferencePayloadAtVersion7) {
  const auto encoded = takion::PacketCodec::encode_congestion(
      values, TakionCodecRig::profile(7),
      takion::PacketEnvelope{takion::PlainEnvelope{}});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().encoded_payload, expected_payload);
  EXPECT_EQ(encoded.value().datagram_size, 15U);
  const auto plain_packet = takion::PacketCodec::encode_plain_packet(
      takion::PlainPacket{takion::PacketType{0x05}, std::nullopt,
                          encoded.value().encoded_payload},
      TakionCodecRig::profile(7));
  ASSERT_TRUE(plain_packet.has_value());
  EXPECT_TRUE(std::ranges::equal(
      plain_packet.value().bytes(),
      TakionCodecRig::bytes({0x05, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x00,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})));
}

TEST_F(TakionCongestionPackets, TravelBehindAnExtendedHeaderInAPsnEnvelope) {
  const auto encoded = takion::PacketCodec::encode_congestion(
      values, TakionCodecRig::profile(20),
      takion::PacketEnvelope{takion::PsnEnvelope{0x42}});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().encoded_payload, expected_payload);
  EXPECT_EQ(encoded.value().datagram_size, 27U);
  const takion::ExtendedPacketHeader congestion_header{0x11223344, 0x55667788};
  const auto plain_packet = takion::PacketCodec::encode_plain_packet(
      takion::PlainPacket{takion::PacketType{0x05}, congestion_header,
                          encoded.value().encoded_payload},
      TakionCodecRig::profile(20));
  ASSERT_TRUE(plain_packet.has_value());
  const auto wrapped = takion::EnvelopeCodec::encode(
      plain_packet.value().bytes(),
      takion::PacketEnvelope{takion::PsnEnvelope{0x42}});
  ASSERT_TRUE(wrapped.has_value());
  EXPECT_EQ(wrapped.value().size(), 27U);
  EXPECT_TRUE(std::ranges::equal(
      wrapped.value().bytes(),
      TakionCodecRig::bytes({0x00, 0x00, 0x00, 0x42, 0x05, 0x11, 0x22,
                             0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x11,
                             0x22, 0x33, 0x44, 0x55, 0x66, 0x00, 0x00,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00})));
  const auto unwrapped = takion::EnvelopeCodec::decode(
      wrapped.value(), takion::PacketEnvelope{takion::PsnEnvelope{0x42}});
  ASSERT_TRUE(unwrapped.has_value());
  const auto decoded = takion::PacketCodec::decode_plain_packet(
      unwrapped.value(), TakionCodecRig::profile(20));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().type.family(), takion::PacketFamily::congestion);
  ASSERT_TRUE(decoded.value().extended_header.has_value());
  EXPECT_EQ(decoded.value().extended_header->timestamp,
            congestion_header.timestamp);
  EXPECT_EQ(decoded.value().extended_header->counter,
            congestion_header.counter);
  EXPECT_TRUE(std::ranges::equal(decoded.value().payload, expected_payload));
}

}  // namespace
}  // namespace takion_test
