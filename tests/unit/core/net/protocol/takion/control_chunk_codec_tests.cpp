#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"
#include "tests/unit/core/net/protocol/takion/control_chunk_vectors.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionHandshakeChunks : public ::testing::Test {
 protected:
  using Vectors = TakionControlChunkVectors;
};

TEST_F(TakionHandshakeChunks, HaveTheReferenceSizes) {
  EXPECT_EQ(Vectors::init().size(), 33U);
  EXPECT_EQ(Vectors::init_acknowledgement().size(), 65U);
  EXPECT_EQ(Vectors::cookie().size(), 49U);
  EXPECT_EQ(Vectors::cookie_acknowledgement().size(), 17U);
}

TEST_F(TakionHandshakeChunks, RoundTripTheReferenceVectorsExactly) {
  EXPECT_TRUE(Vectors::round_trips_exactly<takion::InitChunk>(Vectors::init()));
  EXPECT_TRUE(Vectors::round_trips_exactly<takion::InitAcknowledgementChunk>(
      Vectors::init_acknowledgement()));
  EXPECT_TRUE(
      Vectors::round_trips_exactly<takion::CookieChunk>(Vectors::cookie()));
  EXPECT_TRUE(Vectors::round_trips_exactly<takion::CookieAcknowledgementChunk>(
      Vectors::cookie_acknowledgement()));
}

TEST_F(TakionHandshakeChunks, DecodeTheInitValues) {
  const auto decoded = Vectors::decode_handshake(Vectors::init());
  ASSERT_TRUE(decoded);
  ASSERT_TRUE(std::holds_alternative<takion::InitChunk>(decoded.value()));
  const auto& values = std::get<takion::InitChunk>(decoded.value()).values;
  EXPECT_EQ(values.tag.value, 0x11223344U);
  EXPECT_EQ(values.advertised_receive_window.value, 0x19000U);
  EXPECT_EQ(values.outbound_streams.value, 100U);
  EXPECT_EQ(values.inbound_streams.value, 100U);
  EXPECT_EQ(values.initial_sequence_number.value, 0x55667788U);
}

TEST_F(TakionHandshakeChunks, DecodeTheInitAcknowledgementAndItsCookie) {
  const auto decoded =
      Vectors::decode_handshake(Vectors::init_acknowledgement());
  ASSERT_TRUE(decoded);
  ASSERT_TRUE(std::holds_alternative<takion::InitAcknowledgementChunk>(
      decoded.value()));
  const auto& ack = std::get<takion::InitAcknowledgementChunk>(decoded.value());
  EXPECT_EQ(ack.header.verification_tag.value, 0x11223344U);
  EXPECT_EQ(ack.values.init.tag.value, 0xa1b2c3d4U);
  EXPECT_EQ(ack.values.init.initial_sequence_number.value, 0x89abcdefU);
  for (std::uint8_t value = 0; value < 32; ++value)
    EXPECT_EQ(ack.values.cookie[value], static_cast<std::byte>(value))
        << "index " << +value;
}

TEST_F(TakionHandshakeChunks, RefuseEveryStrictPrefix) {
  const std::vector<std::vector<std::byte>> vectors{
      Vectors::init(),
      Vectors::init_acknowledgement(),
      Vectors::cookie(),
      Vectors::cookie_acknowledgement(),
  };
  for (const auto& vector : vectors) {
    for (std::size_t size = 0; size < vector.size(); ++size) {
      const auto decoded =
          Vectors::decode_handshake(std::span{vector}.first(size));
      ASSERT_FALSE(decoded.has_value()) << "size " << size;
      EXPECT_TRUE(decoded.error().needed > decoded.error().available ||
                  decoded.error().reason ==
                      takion::ParseFailureReason::invalid_length)
          << "size " << size;
    }
  }
}

TEST_F(TakionHandshakeChunks, RefuseALengthBelowTheChunkHeader) {
  auto too_small = Vectors::init();
  too_small[15] = std::byte{0};
  too_small[16] = std::byte{3};
  const auto result = Vectors::decode_handshake(too_small);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, takion::ParseFailureReason::invalid_length);
  EXPECT_EQ(result.error().offset.value, 14U);
}

TEST_F(TakionHandshakeChunks, RefuseALengthThatDisagreesWithThePacket) {
  auto mismatch = Vectors::init();
  mismatch[16] = std::byte{0x15};
  const auto result = Vectors::decode_handshake(mismatch);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, takion::ParseFailureReason::invalid_length);
}

TEST_F(TakionHandshakeChunks, RefuseAnUnknownChunkType) {
  auto unknown = Vectors::init();
  unknown[13] = std::byte{0x7f};
  const auto result = Vectors::decode_handshake(unknown);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason,
            takion::ParseFailureReason::unknown_control_chunk);
  EXPECT_EQ(result.error().offset.value, 12U);
}

class TakionDataChunks : public ::testing::Test {
 protected:
  using Vectors = TakionControlChunkVectors;

  static auto reencoded(const takion::ControlChunkPacket& packet)
      -> std::optional<std::vector<std::byte>> {
    const auto chunk = takion::ControlChunkCodec::encode_packet(packet);
    EXPECT_TRUE(chunk.has_value());
    if (!chunk) return std::nullopt;
    const auto encoded = takion::PacketCodec::encode_plain_packet(
        takion::PlainPacket{
            takion::PacketType{0}, std::nullopt,
            std::vector<std::byte>(chunk.value().begin(), chunk.value().end())},
        TakionCodecRig::profile(7));
    if (!encoded) return std::nullopt;
    return std::vector<std::byte>(encoded.value().bytes().begin(),
                                  encoded.value().bytes().end());
  }

  static auto extended_acknowledgement() -> std::vector<std::byte> {
    auto extended_ack = Vectors::data_acknowledgement();
    extended_ack[15] = std::byte{0x00};
    extended_ack[16] = std::byte{0x20};
    extended_ack[25] = std::byte{0x00};
    extended_ack[26] = std::byte{0x02};
    extended_ack[27] = std::byte{0x00};
    extended_ack[28] = std::byte{0x02};
    const auto extension =
        TakionCodecRig::bytes({0x00, 0x01, 0x00, 0x02, 0x00, 0x04, 0x00, 0x04,
                               0x01, 0x02, 0x03, 0x04, 0xaa, 0xbb, 0xcc, 0xdd});
    extended_ack.insert(extended_ack.end(), extension.begin(), extension.end());
    return extended_ack;
  }
};

TEST_F(TakionDataChunks, DecodeAndReencodeTheDataVector) {
  const auto data_wire = Vectors::data();
  const auto decoded = Vectors::decode_control(data_wire);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_TRUE(std::holds_alternative<takion::DataChunk>(decoded.value()));
  const auto& data = std::get<takion::DataChunk>(decoded.value());
  EXPECT_EQ(data.header.verification_tag.value, 0x11223344U);
  EXPECT_EQ(data.header.key_position, 0x01020304U);
  EXPECT_EQ(data.header.flags, 0x03U);
  EXPECT_EQ(data.payload, TakionCodecRig::bytes({0xde, 0xad, 0xbe, 0xef}));
  EXPECT_EQ(reencoded(decoded.value()), data_wire);
}

TEST_F(TakionDataChunks, DecodeAndReencodeTheAcknowledgementVector) {
  const auto ack_wire = Vectors::data_acknowledgement();
  const auto decoded = Vectors::decode_control(ack_wire);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_TRUE(std::holds_alternative<takion::DataAcknowledgementChunk>(
      decoded.value()));
  const auto& ack = std::get<takion::DataAcknowledgementChunk>(decoded.value());
  EXPECT_EQ(ack.header.flags, 0xa5U);
  EXPECT_EQ(ack.values.cumulative_sequence, 0x55667788U);
  EXPECT_EQ(ack.values.advertised_receive_window, 0x19000U);
  EXPECT_TRUE(ack.values.gap_blocks.empty());
  EXPECT_TRUE(ack.values.duplicate_sequences.empty());
  EXPECT_EQ(reencoded(decoded.value()), ack_wire);
}

TEST_F(TakionDataChunks, CarryGapBlocksAndDuplicateSequences) {
  const auto extended_ack = extended_acknowledgement();
  const auto decoded = Vectors::decode_control(extended_ack);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_TRUE(std::holds_alternative<takion::DataAcknowledgementChunk>(
      decoded.value()));
  const auto& extended =
      std::get<takion::DataAcknowledgementChunk>(decoded.value());
  EXPECT_EQ(extended.values.gap_blocks,
            (std::vector<takion::GapAcknowledgementBlock>{{1, 2}, {4, 4}}));
  EXPECT_EQ(extended.values.duplicate_sequences,
            (std::vector<std::uint32_t>{0x01020304, 0xaabbccdd}));
  const auto extended_chunk =
      takion::ControlChunkCodec::encode_packet(decoded.value());
  ASSERT_TRUE(extended_chunk.has_value());
  EXPECT_TRUE(std::ranges::equal(
      extended_chunk.value().bytes(),
      std::vector<std::byte>(extended_ack.begin() + 1, extended_ack.end())));
  EXPECT_EQ(reencoded(decoded.value()), extended_ack);
}

TEST_F(TakionDataChunks, RefuseEveryStrictPrefix) {
  for (const auto& vector : {Vectors::data(), Vectors::data_acknowledgement(),
                             extended_acknowledgement()}) {
    for (std::size_t size = 0; size < vector.size(); ++size)
      EXPECT_FALSE(Vectors::decode_control(std::span{vector}.first(size)))
          << "size " << size << " of " << vector.size();
  }
}

TEST_F(TakionDataChunks, RefuseAnUnknownChunkType) {
  auto unknown = Vectors::data();
  unknown[13] = std::byte{0x7f};
  const auto result = Vectors::decode_control(unknown);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason,
            takion::ParseFailureReason::unknown_control_chunk);
}

TEST_F(TakionDataChunks, RefuseAMalformedGapBlockCount) {
  auto malformed = extended_acknowledgement();
  malformed[26] = std::byte{0x03};
  const auto result = Vectors::decode_control(malformed);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, takion::ParseFailureReason::invalid_length);
}

TEST_F(TakionDataChunks, RefuseAMalformedDuplicateCount) {
  auto malformed = extended_acknowledgement();
  malformed[28] = std::byte{0x03};
  const auto result = Vectors::decode_control(malformed);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, takion::ParseFailureReason::invalid_length);
}

TEST_F(TakionDataChunks, RefuseAChunkLengthBeyondThePacket) {
  auto malformed = extended_acknowledgement();
  malformed[15] = std::byte{0x00};
  malformed[16] = std::byte{0x24};
  EXPECT_FALSE(Vectors::decode_control(malformed));
}

}  // namespace
}  // namespace takion_test
