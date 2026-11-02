#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"
#include "tests/unit/core/net/protocol/takion/control_chunk_vectors.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionPlainPackets : public ::testing::Test {
 protected:
  static constexpr std::array<std::uint32_t, 16> known_versions{
      7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 23, 25};
  static constexpr std::array<takion::PacketFamily, 11> families{
      takion::PacketFamily::control,
      takion::PacketFamily::feedback_history,
      takion::PacketFamily::video,
      takion::PacketFamily::audio_and_haptics,
      takion::PacketFamily::length_prefixed,
      takion::PacketFamily::congestion,
      takion::PacketFamily::feedback_state,
      takion::PacketFamily::client_information,
      takion::PacketFamily::trace,
      takion::PacketFamily::generic_data,
      takion::PacketFamily::extended_message,
  };
};

TEST_F(TakionPlainPackets, AcceptOnlyKnownFamiliesAtVersion7) {
  for (std::uint8_t code = 0; code < 16; ++code) {
    const std::vector<std::byte> wire{static_cast<std::byte>(code)};
    const auto decoded = takion::PacketCodec::decode_plain_packet(
        wire, TakionCodecRig::profile(7));
    const bool known = takion::PacketType::family_from_code(code).has_value();
    const bool extended_message =
        code ==
        static_cast<std::uint8_t>(takion::PacketFamily::extended_message);
    EXPECT_EQ(decoded.has_value(), known && !extended_message)
        << "code " << +code;
    if (decoded)
      EXPECT_EQ(decoded.value().type.raw, code);
    else if (extended_message)
      EXPECT_EQ(decoded.error().reason,
                takion::ParseFailureReason::unsupported_layout);
    else
      EXPECT_EQ(decoded.error().reason,
                takion::ParseFailureReason::unknown_packet_family)
          << "code " << +code;
  }
}

TEST_F(TakionPlainPackets, KeepTheFlagBitsOfTheType) {
  auto flagged = TakionControlChunkVectors::init();
  flagged[0] = std::byte{0x10};
  const auto decoded = takion::PacketCodec::decode_plain_packet(
      flagged, TakionCodecRig::profile(7));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().type.raw, 0x10U);
  EXPECT_EQ(decoded.value().type.flags(), 0x10U);
  EXPECT_EQ(decoded.value().type.family(), takion::PacketFamily::control);
  EXPECT_TRUE(TakionControlChunkVectors::round_trips_exactly<takion::InitChunk>(
      flagged));
}

TEST_F(TakionPlainPackets, CarryExtendedMessagesOnlyFromVersion18) {
  const takion::PacketType extended_type{0xae};
  const auto extended_payload = TakionCodecRig::bytes({0xde, 0xad});
  const takion::PlainPacket extended_packet{extended_type, std::nullopt,
                                            extended_payload};
  for (const std::uint32_t early_version : {7U, 9U, 12U, 15U, 17U}) {
    const auto early_encode = takion::PacketCodec::encode_plain_packet(
        extended_packet, TakionCodecRig::profile(early_version));
    ASSERT_FALSE(early_encode.has_value()) << "version " << early_version;
    EXPECT_EQ(early_encode.error().reason,
              takion::ParseFailureReason::unsupported_layout);
    const auto early_decode = takion::PacketCodec::decode_plain_packet(
        TakionCodecRig::bytes({extended_type.raw, 0xde, 0xad}),
        TakionCodecRig::profile(early_version));
    ASSERT_FALSE(early_decode.has_value()) << "version " << early_version;
    EXPECT_EQ(early_decode.error().reason,
              takion::ParseFailureReason::unsupported_layout);
  }

  const auto accepted_profile = takion::resolve_takion_wire_profile(18).value();
  const auto accepted = takion::PacketCodec::encode_plain_packet(
      extended_packet, accepted_profile);
  ASSERT_TRUE(accepted.has_value());
  EXPECT_TRUE(std::ranges::equal(
      accepted.value().bytes(),
      TakionCodecRig::bytes({extended_type.raw, 0xde, 0xad})));
  const auto accepted_decoded = takion::PacketCodec::decode_plain_packet(
      accepted.value(), accepted_profile);
  ASSERT_TRUE(accepted_decoded.has_value());
  EXPECT_TRUE(std::ranges::equal(accepted_decoded.value().payload,
                                 extended_packet.payload));
}

TEST_F(TakionPlainPackets, UseExtendedHeadersAndPrefixesFromVersion20) {
  for (const auto version : known_versions) {
    const auto resolved = takion::resolve_takion_wire_profile(version).value();
    for (std::uint16_t raw = 0; raw < 256; ++raw) {
      const takion::PacketType type{static_cast<std::uint8_t>(raw)};
      const bool extended =
          version >= 20 &&
          takion::PacketType::has_extended_header(type.family_code());
      EXPECT_EQ(resolved.uses_extended_header(type), extended)
          << "version " << version << " type " << raw;
      EXPECT_EQ(resolved.packet_header_size(type), extended ? 9U : 1U)
          << "version " << version << " type " << raw;
    }
    for (std::uint16_t raw = 0; raw < 256; ++raw) {
      const takion::MessageDataType type{static_cast<std::uint8_t>(raw)};
      const bool prefixed =
          version >= 20 && takion::MessageDataType::has_fixed_prefix(type.raw);
      EXPECT_EQ(resolved.data_message_prefix_size(type), prefixed ? 8U : 0U)
          << "version " << version << " data type " << raw;
    }
  }
}

TEST_F(TakionPlainPackets,
       ExemptControlLengthPrefixedAndClientInformationAtVersion20) {
  for (const auto family : families) {
    const auto code = static_cast<std::uint8_t>(family);
    const bool exempt = family == takion::PacketFamily::control ||
                        family == takion::PacketFamily::length_prefixed ||
                        family == takion::PacketFamily::client_information;
    const auto payload = TakionCodecRig::bytes({0xaa, 0xbb});
    const takion::PlainPacket packet{
        takion::PacketType{static_cast<std::uint8_t>(0xa0 | code)},
        exempt ? std::nullopt
               : std::optional<takion::ExtendedPacketHeader>{{0x11223344,
                                                              0x55667788}},
        payload,
    };
    const auto encoded = takion::PacketCodec::encode_plain_packet(
        packet, TakionCodecRig::profile(20));
    ASSERT_TRUE(encoded.has_value()) << "family " << +code;
    EXPECT_EQ(encoded.value().size(), exempt ? 3U : 11U) << "family " << +code;
    const auto decoded = takion::PacketCodec::decode_plain_packet(
        encoded.value(), TakionCodecRig::profile(20));
    ASSERT_TRUE(decoded.has_value()) << "family " << +code;
    EXPECT_EQ(decoded.value().type.raw, static_cast<std::uint8_t>(0xa0 | code));
    EXPECT_EQ(decoded.value().extended_header.has_value(), !exempt)
        << "family " << +code;
    if (!exempt) {
      EXPECT_EQ(decoded.value().extended_header->timestamp, 0x11223344U);
      EXPECT_EQ(decoded.value().extended_header->counter, 0x55667788U);
    }
    EXPECT_TRUE(std::ranges::equal(decoded.value().payload, payload))
        << "family " << +code;
  }
}

TEST_F(TakionPlainPackets, RefuseAMissingExtendedHeaderAtVersion20) {
  const auto missing = takion::PacketCodec::decode_plain_packet(
      TakionCodecRig::bytes({0x02, 0, 1, 2, 3}), TakionCodecRig::profile(20));
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().reason, takion::ParseFailureReason::truncated);
}

TEST_F(TakionPlainPackets, EncodeDecodeAndTruncateEveryFamilyAtEveryVersion) {
  for (const auto version : known_versions) {
    const auto resolved = takion::resolve_takion_wire_profile(version).value();
    for (const auto family : families) {
      const auto code = static_cast<std::uint8_t>(family);
      const takion::PacketType type{static_cast<std::uint8_t>(0xa0 | code)};
      SCOPED_TRACE(testing::Message()
                   << "version " << version << " family " << +code);
      if (family == takion::PacketFamily::extended_message &&
          !resolved.features().contains(takion::Feature::extended_messages)) {
        EXPECT_LT(version, 18U);
        const auto refused = takion::PacketCodec::encode_plain_packet(
            takion::PlainPacket{type, std::nullopt,
                                TakionCodecRig::bytes({0xde, 0xad})},
            resolved);
        ASSERT_FALSE(refused.has_value());
        EXPECT_EQ(refused.error().reason,
                  takion::ParseFailureReason::unsupported_layout);
        continue;
      }
      const bool extended = resolved.uses_extended_header(type);
      const auto payload = TakionCodecRig::bytes({0xde, 0xad});
      std::vector<std::byte> expected{static_cast<std::byte>(type.raw)};
      if (extended) {
        expected.insert(expected.end(),
                        {std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
                         std::byte{0x44}, std::byte{0x55}, std::byte{0x66},
                         std::byte{0x77}, std::byte{0x88}});
      }
      expected.insert(expected.end(), payload.begin(), payload.end());

      const takion::PlainPacket packet{
          type,
          extended ? std::optional<takion::ExtendedPacketHeader>{{0x11223344,
                                                                  0x55667788}}
                   : std::nullopt,
          payload,
      };
      const auto encoded =
          takion::PacketCodec::encode_plain_packet(packet, resolved);
      ASSERT_TRUE(encoded.has_value());
      EXPECT_TRUE(std::ranges::equal(encoded.value().bytes(), expected));

      const auto decoded =
          takion::PacketCodec::decode_plain_packet(expected, resolved);
      ASSERT_TRUE(decoded.has_value());
      EXPECT_EQ(decoded.value().type.raw, type.raw);
      EXPECT_EQ(decoded.value().extended_header.has_value(), extended);
      if (extended) {
        EXPECT_EQ(decoded.value().extended_header->timestamp, 0x11223344U);
        EXPECT_EQ(decoded.value().extended_header->counter, 0x55667788U);
      }
      EXPECT_TRUE(std::ranges::equal(decoded.value().payload, payload));
      const auto reencoded =
          takion::PacketCodec::encode_plain_packet(decoded.value(), resolved);
      ASSERT_TRUE(reencoded.has_value());
      EXPECT_TRUE(std::ranges::equal(reencoded.value().bytes(), expected));

      const auto header_size = resolved.packet_header_size(type);
      for (std::size_t size = 0; size < header_size; ++size) {
        const auto truncated = takion::PacketCodec::decode_plain_packet(
            std::span{expected}.first(size), resolved);
        ASSERT_FALSE(truncated.has_value()) << "size " << size;
        EXPECT_EQ(truncated.error().reason,
                  takion::ParseFailureReason::truncated);
      }

      const takion::PlainPacket missing_header{type, std::nullopt, payload};
      const auto missing_result =
          takion::PacketCodec::encode_plain_packet(missing_header, resolved);
      const takion::PlainPacket unexpected_header{
          type,
          takion::ExtendedPacketHeader{0x11223344, 0x55667788},
          payload,
      };
      const auto unexpected_result =
          takion::PacketCodec::encode_plain_packet(unexpected_header, resolved);
      if (extended) {
        ASSERT_FALSE(missing_result.has_value());
        EXPECT_EQ(missing_result.error().reason,
                  takion::ParseFailureReason::invalid_field);
        EXPECT_TRUE(unexpected_result.has_value());
      } else {
        EXPECT_TRUE(missing_result.has_value());
        ASSERT_FALSE(unexpected_result.has_value());
        EXPECT_EQ(unexpected_result.error().reason,
                  takion::ParseFailureReason::invalid_field);
      }
    }
  }
}

}  // namespace
}  // namespace takion_test
