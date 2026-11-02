#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionSenkushaProbePackets : public ::testing::Test {
 protected:
  TakionSenkushaProbePackets() {
    values.frame_index = 0x1234;
    values.unit_index = 0x56;
    values.units_in_frame_total = 0x800;
    values.codec = 0xff;
    values.payload =
        TakionCodecRig::bytes({0x00, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44});
  }

  void make_empty_video() {
    values.video = true;
    values.payload.clear();
  }

  auto encode(const takion::PacketEnvelope& envelope =
                  takion::PacketEnvelope{takion::PlainEnvelope{}},
              std::uint32_t version = 7) const {
    return takion::PacketCodec::encode_senkusha_probe(
        values, TakionCodecRig::profile(version), envelope);
  }

  static auto without_type(const std::vector<std::byte>& wire)
      -> std::vector<std::byte> {
    return std::vector<std::byte>(std::span{wire}.begin() + 1,
                                  std::span{wire}.end());
  }

  takion::SenkushaProbeValues values;
};

TEST_F(TakionSenkushaProbePackets, EncodeTheAudioReferenceVector) {
  const auto audio_wire = TakionCodecRig::bytes({
      0x03, 0x00, 0x00, 0x12, 0x34, 0x0a, 0xdf, 0xfc, 0x00,
      0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44,
  });
  const auto encoded = encode();
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().family, takion::PacketFamily::audio_and_haptics);
  EXPECT_EQ(encoded.value().encoded_av_payload, without_type(audio_wire));
  EXPECT_EQ(encoded.value().datagram_size, 26U);
}

TEST_F(TakionSenkushaProbePackets, EncodeTheVideoReferenceVector) {
  make_empty_video();
  const auto video_wire = TakionCodecRig::bytes({
      0x02, 0x00, 0x00, 0x12, 0x34, 0x0a, 0xdf, 0xfc, 0x00, 0xff, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  });
  const auto encoded = encode();
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().family, takion::PacketFamily::video);
  EXPECT_EQ(encoded.value().encoded_av_payload, without_type(video_wire));
  EXPECT_EQ(encoded.value().datagram_size, 21U);
}

TEST_F(TakionSenkushaProbePackets, ZeroTheNaluInfoStructsWhenTheyAreUsed) {
  make_empty_video();
  values.uses_nalu_info_structs = true;
  const auto encoded = encode();
  ASSERT_TRUE(encoded.has_value());
  ASSERT_EQ(encoded.value().encoded_av_payload.size(), 23U);
  EXPECT_EQ(encoded.value().encoded_av_payload[20], std::byte{0});
  EXPECT_EQ(encoded.value().encoded_av_payload[21], std::byte{0});
  EXPECT_EQ(encoded.value().encoded_av_payload[22], std::byte{0});
}

TEST_F(TakionSenkushaProbePackets, CountThePsnEnvelopeInTheDatagram) {
  make_empty_video();
  const auto encoded =
      encode(takion::PacketEnvelope{takion::PsnEnvelope{0x42}});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().datagram_size, 25U);
}

TEST_F(TakionSenkushaProbePackets, RefuseUnitFieldsOutsideTheirBitWidths) {
  make_empty_video();
  values.units_in_frame_total = 0;
  EXPECT_FALSE(encode());
  values.units_in_frame_total = 0x801;
  EXPECT_FALSE(encode());
  values.units_in_frame_total = 1;
  values.units_in_frame_fec = 0x400;
  EXPECT_FALSE(encode());
  values.units_in_frame_fec = 0;
  values.unit_index = 0x800;
  EXPECT_FALSE(encode());
  values.unit_index = 0;
  values.adaptive_stream_index = 8;
  EXPECT_FALSE(encode());
}

TEST_F(TakionSenkushaProbePackets, AreOnlyDefinedForVersion7) {
  make_empty_video();
  values.units_in_frame_total = 1;
  values.unit_index = 0;
  for (const auto version : {9U, 20U}) {
    const auto result =
        encode(takion::PacketEnvelope{takion::PlainEnvelope{}}, version);
    ASSERT_FALSE(result) << "version " << version;
    EXPECT_EQ(result.error().reason,
              takion::ParseFailureReason::unsupported_layout);
  }
}

}  // namespace
}  // namespace takion_test
