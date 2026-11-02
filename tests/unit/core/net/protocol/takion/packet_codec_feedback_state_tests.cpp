#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionFeedbackStatePayloads : public ::testing::Test {
 protected:
  TakionFeedbackStatePayloads() {
    values.controller_id = 2;
    values.orient_w = 1.0F;
    values.left_x = -32768;
    values.left_y = -1;
    values.right_y = 32767;
  }

  auto encodes_at_version_7() const -> bool {
    return takion::PacketCodec::encode_feedback_state(
               values, TakionCodecRig::profile(7), takion::PlainEnvelope{})
        .has_value();
  }

  takion::FeedbackStateValues values;
};

TEST_F(TakionFeedbackStatePayloads, MatchTheReferenceBytesForEachLayout) {
  const auto expected_v9_payload = TakionCodecRig::bytes({
      0xa2, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f,
      0xff, 0x7f, 0xff, 0x7f, 0xfe, 0xf7, 0xef, 0x1f, 0x80,
      0x00, 0xff, 0xff, 0x00, 0x00, 0x7f, 0xff,
  });
  const auto expected_v12_payload = TakionCodecRig::bytes({
      0xa2, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff, 0x7f, 0xff,
      0x7f, 0xff, 0x7f, 0xfe, 0xf7, 0xef, 0x1f, 0x80, 0x00, 0xff,
      0xff, 0x00, 0x00, 0x7f, 0xff, 0x00, 0x00, 0x01,
  });
  auto encoded = takion::PacketCodec::encode_feedback_state(
      values, TakionCodecRig::profile(7),
      takion::PacketEnvelope{takion::PlainEnvelope{}});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().encoded_payload, expected_v9_payload);
  EXPECT_EQ(encoded.value().datagram_size, 26U);
  encoded = takion::PacketCodec::encode_feedback_state(
      values, TakionCodecRig::profile(9),
      takion::PacketEnvelope{takion::PlainEnvelope{}});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().encoded_payload, expected_v9_payload);
  EXPECT_EQ(encoded.value().datagram_size, 26U);
  encoded = takion::PacketCodec::encode_feedback_state(
      values, TakionCodecRig::profile(20),
      takion::PacketEnvelope{takion::PsnEnvelope{0x42}});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(encoded.value().encoded_payload, expected_v12_payload);
  EXPECT_EQ(encoded.value().datagram_size, 41U);
}

TEST_F(TakionFeedbackStatePayloads, AcceptMotionAtItsLimits) {
  for (const auto gyro : {-30.0F, 30.0F}) {
    values.gyro_x = gyro;
    values.gyro_y = gyro;
    values.gyro_z = gyro;
    EXPECT_TRUE(encodes_at_version_7()) << "gyro " << gyro;
  }
  values.gyro_x = 0.0F;
  values.gyro_y = 0.0F;
  values.gyro_z = 0.0F;
  for (const auto accel : {-5.0F, 5.0F}) {
    values.accel_x = accel;
    values.accel_y = accel;
    values.accel_z = accel;
    EXPECT_TRUE(encodes_at_version_7()) << "accel " << accel;
  }
}

TEST_F(TakionFeedbackStatePayloads, RefuseAControllerBeyondTheFourth) {
  values.controller_id = 4;
  EXPECT_FALSE(encodes_at_version_7());
}

TEST_F(TakionFeedbackStatePayloads, ReplaceNonFiniteMotionWithTheNeutralPose) {
  auto neutral = values;
  neutral.gyro_y = 0.0F;
  neutral.accel_x = 0.0F;
  neutral.accel_y = 1.0F;
  neutral.accel_z = 0.0F;
  neutral.orient_x = 0.0F;
  neutral.orient_y = 0.0F;
  neutral.orient_z = 0.0F;
  neutral.orient_w = 1.0F;
  auto broken = neutral;
  broken.gyro_y = std::numeric_limits<float>::quiet_NaN();
  broken.accel_x = std::numeric_limits<float>::infinity();
  broken.accel_y = 3.0F;
  broken.orient_x = std::numeric_limits<float>::quiet_NaN();
  broken.orient_w = 0.5F;
  const auto expected = takion::PacketCodec::encode_feedback_state(
      neutral, TakionCodecRig::profile(7), takion::PlainEnvelope{});
  const auto sanitized = takion::PacketCodec::encode_feedback_state(
      broken, TakionCodecRig::profile(7), takion::PlainEnvelope{});
  ASSERT_TRUE(expected);
  ASSERT_TRUE(sanitized);
  EXPECT_EQ(expected.value().encoded_payload,
            sanitized.value().encoded_payload);
}

TEST_F(TakionFeedbackStatePayloads, ClampMotionBeyondItsLimits) {
  auto limit = values;
  limit.gyro_x = 30.0F;
  limit.gyro_y = -30.0F;
  limit.accel_x = 5.0F;
  limit.accel_z = -5.0F;
  auto beyond = values;
  beyond.gyro_x = 300.0F;
  beyond.gyro_y = -31.0F;
  beyond.accel_x = 9.81F;
  beyond.accel_z = -5.001F;
  const auto limit_encoded = takion::PacketCodec::encode_feedback_state(
      limit, TakionCodecRig::profile(7), takion::PlainEnvelope{});
  const auto beyond_encoded = takion::PacketCodec::encode_feedback_state(
      beyond, TakionCodecRig::profile(7), takion::PlainEnvelope{});
  ASSERT_TRUE(limit_encoded.has_value());
  ASSERT_TRUE(beyond_encoded.has_value());
  EXPECT_EQ(limit_encoded.value().encoded_payload,
            beyond_encoded.value().encoded_payload);
}

TEST(TakionFeedbackStateOrientation, PacksTheQuaternionLikeTheReference) {
  const std::pair<std::array<float, 4>, std::vector<std::byte>> cases[]{
      {{0.1F, -0.7F, 0.2F, 0.5F},
       TakionCodecRig::bytes({0x83, 0x1b, 0x79, 0x94, 0x36})},
      {{0.5F, 0.5F, 0.5F, 0.5F},
       TakionCodecRig::bytes({0x83, 0xa0, 0x4d, 0x9b, 0x36})},
      {{-0.5F, 0.5F, -0.5F, -0.5F},
       TakionCodecRig::bytes({0x83, 0xa1, 0xad, 0x44, 0x09})},
      {{0.9F, -0.9F, 0.0F, 0.1F},
       TakionCodecRig::bytes({0x83, 0x00, 0xf0, 0x6f, 0x24})},
      {{0.0F, 0.0F, -1.0F, 0.0F},
       TakionCodecRig::bytes({0x83, 0xfd, 0xf7, 0xef, 0x1f})},
  };
  const auto motion = TakionCodecRig::bytes(
      {0xa0, 0x65, 0x86, 0xbb, 0x4b, 0xba, 0xfb, 0xcc, 0x6c, 0x32, 0xf3, 0x32});
  for (const auto& [orientation, tail] : cases) {
    takion::FeedbackStateValues values;
    values.controller_id = 0;
    values.gyro_x = 1.5F;
    values.gyro_y = -12.25F;
    values.gyro_z = 29.0F;
    values.accel_x = -0.75F;
    values.accel_y = 4.5F;
    values.accel_z = 0.125F;
    values.orient_x = orientation[0];
    values.orient_y = orientation[1];
    values.orient_z = orientation[2];
    values.orient_w = orientation[3];
    const auto encoded = takion::PacketCodec::encode_feedback_state(
        values, TakionCodecRig::profile(7), takion::PlainEnvelope{});
    ASSERT_TRUE(encoded.has_value());
    auto expected = motion;
    expected.insert(expected.end(), tail.begin(), tail.end());
    expected.resize(takion::feedback_state_v9_payload_size);
    EXPECT_EQ(encoded.value().encoded_payload, expected);
  }
}

class TakionFeedbackStatePackets : public ::testing::Test {
 protected:
  void SetUp() override {
    takion::FeedbackStateValues values;
    values.controller_id = 2;
    values.orient_w = 1.0F;
    auto v9 = takion::PacketCodec::encode_feedback_state(
        values, TakionCodecRig::profile(7), plain_envelope);
    auto v12 = takion::PacketCodec::encode_feedback_state(
        values, takion::resolve_takion_wire_profile(12).value(),
        plain_envelope);
    ASSERT_TRUE(v9.has_value());
    ASSERT_TRUE(v12.has_value());
    v9_state = std::move(v9).value();
    v12_state = std::move(v12).value();
  }

  static auto state_bytes(const std::vector<std::byte>& packet,
                          std::ptrdiff_t offset) -> std::vector<std::byte> {
    return std::vector<std::byte>(packet.begin() + offset, packet.end());
  }

  const takion::PacketEnvelope plain_envelope{takion::PlainEnvelope{}};
  const takion::PacketEnvelope psn_envelope{takion::PsnEnvelope{0x42}};
  const takion::FeedbackStatePacketHeaderInput header{0x1234, std::nullopt};
  const takion::FeedbackStatePacketHeaderInput extended_input{
      0x1234, takion::ExtendedPacketHeader{0x11223344, 0x55667788}};
  std::optional<takion::EncodedFeedbackState> v9_state;
  std::optional<takion::EncodedFeedbackState> v12_state;
};

TEST_F(TakionFeedbackStatePackets, PlaceAVersion9StateBehindTheHeader) {
  const auto assembled = takion::PacketCodec::assemble_feedback_state_packet(
      *v9_state, header, TakionCodecRig::profile(7), plain_envelope);
  ASSERT_TRUE(assembled.has_value());
  EXPECT_EQ(assembled.value().encoded_packet.size(), 37U);
  EXPECT_EQ(assembled.value().datagram_size, 37U);
  EXPECT_EQ(assembled.value().state_payload_offset, 12U);
  EXPECT_EQ(assembled.value().state_payload_size, 25U);
  EXPECT_TRUE(std::equal(
      assembled.value().encoded_packet.begin(),
      assembled.value().encoded_packet.begin() + 12,
      TakionCodecRig::bytes({0x06, 0x12, 0x34, 0x00, 0, 0, 0, 0, 0, 0, 0, 0})
          .begin()));
  EXPECT_EQ(state_bytes(assembled.value().encoded_packet, 12),
            v9_state->encoded_payload);
}

TEST_F(TakionFeedbackStatePackets, PlaceAVersion12StateBehindTheHeader) {
  const auto assembled = takion::PacketCodec::assemble_feedback_state_packet(
      *v12_state, header, takion::resolve_takion_wire_profile(12).value(),
      plain_envelope);
  ASSERT_TRUE(assembled.has_value());
  EXPECT_EQ(assembled.value().encoded_packet.size(), 40U);
  EXPECT_EQ(assembled.value().datagram_size, 40U);
  EXPECT_EQ(assembled.value().state_payload_offset, 12U);
  EXPECT_EQ(assembled.value().state_payload_size, 28U);
  EXPECT_EQ(state_bytes(assembled.value().encoded_packet, 12),
            v12_state->encoded_payload);
}

TEST_F(TakionFeedbackStatePackets, PlaceTheStateBehindAnExtendedHeader) {
  const auto assembled = takion::PacketCodec::assemble_feedback_state_packet(
      *v12_state, extended_input, TakionCodecRig::profile(20), psn_envelope);
  ASSERT_TRUE(assembled.has_value());
  EXPECT_EQ(assembled.value().encoded_packet.size(), 48U);
  EXPECT_EQ(assembled.value().datagram_size, 52U);
  EXPECT_EQ(assembled.value().state_payload_offset, 20U);
  EXPECT_EQ(assembled.value().state_payload_size, 28U);
  EXPECT_TRUE(std::equal(
      assembled.value().encoded_packet.begin(),
      assembled.value().encoded_packet.begin() + 20,
      TakionCodecRig::bytes({0x06, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                             0x77, 0x88, 0x12, 0x34, 0x00, 0,    0,
                             0,    0,    0,    0,    0,    0})
          .begin()));
  EXPECT_EQ(state_bytes(assembled.value().encoded_packet, 20),
            v12_state->encoded_payload);
}

TEST_F(TakionFeedbackStatePackets, RefuseAHeaderThatDisagreesWithTheVersion) {
  const auto missing_header =
      takion::PacketCodec::assemble_feedback_state_packet(
          *v12_state, header, TakionCodecRig::profile(20), psn_envelope);
  ASSERT_FALSE(missing_header.has_value());
  EXPECT_EQ(missing_header.error().reason,
            takion::ParseFailureReason::invalid_field);
  const auto unexpected_header =
      takion::PacketCodec::assemble_feedback_state_packet(
          *v9_state, extended_input, TakionCodecRig::profile(7),
          plain_envelope);
  ASSERT_FALSE(unexpected_header.has_value());
  EXPECT_EQ(unexpected_header.error().reason,
            takion::ParseFailureReason::invalid_field);
}

TEST_F(TakionFeedbackStatePackets, RefuseAStateOfTheWrongSize) {
  for (const auto size : {std::size_t{24}, std::size_t{26}}) {
    auto malformed_state = *v9_state;
    malformed_state.encoded_payload.resize(size);
    const auto malformed = takion::PacketCodec::assemble_feedback_state_packet(
        malformed_state, header, TakionCodecRig::profile(7), plain_envelope);
    ASSERT_FALSE(malformed.has_value()) << "size " << size;
    EXPECT_EQ(malformed.error().reason,
              takion::ParseFailureReason::invalid_length);
  }
}

TEST_F(TakionFeedbackStatePackets, RefuseAStateWithAnInvalidLeadByte) {
  auto invalid_field_state = *v9_state;
  invalid_field_state.encoded_payload[0] = std::byte{0xb2};
  const auto invalid_field =
      takion::PacketCodec::assemble_feedback_state_packet(
          invalid_field_state, header, TakionCodecRig::profile(7),
          plain_envelope);
  ASSERT_FALSE(invalid_field.has_value());
  EXPECT_EQ(invalid_field.error().reason,
            takion::ParseFailureReason::invalid_field);
}

}  // namespace
}  // namespace takion_test
