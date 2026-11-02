#pragma once

#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace takion_test {

class TakionControlChunkVectors {
 public:
  using Bytes = std::vector<std::byte>;

  static auto init() -> Bytes {
    return TakionCodecRig::bytes({
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x14, 0x11, 0x22, 0x33, 0x44, 0x00,
        0x01, 0x90, 0x00, 0x00, 0x64, 0x00, 0x64, 0x55, 0x66, 0x77, 0x88,
    });
  }

  static auto init_acknowledgement() -> Bytes {
    auto result = TakionCodecRig::bytes({
        0x00, 0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x02, 0x00, 0x00, 0x34, 0xa1, 0xb2, 0xc3, 0xd4, 0x00,
        0x01, 0x90, 0x00, 0x00, 0x64, 0x00, 0x64, 0x89, 0xab, 0xcd, 0xef,
    });
    for (std::uint8_t value = 0; value < 32; ++value)
      result.push_back(static_cast<std::byte>(value));
    return result;
  }

  static auto cookie() -> Bytes {
    auto result = TakionCodecRig::bytes({0x00, 0xa1, 0xb2, 0xc3, 0xd4, 0x00,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x0a, 0x00, 0x00, 0x24});
    for (std::uint8_t value = 0; value < 32; ++value)
      result.push_back(static_cast<std::byte>(value));
    return result;
  }

  static auto cookie_acknowledgement() -> Bytes {
    return TakionCodecRig::bytes({0x00, 0x11, 0x22, 0x33, 0x44, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b,
                                  0x00, 0x00, 0x04});
  }

  static auto data() -> Bytes {
    return TakionCodecRig::bytes({
        0x00, 0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
        0x03, 0x04, 0x00, 0x03, 0x00, 0x08, 0xde, 0xad, 0xbe, 0xef,
    });
  }

  static auto data_acknowledgement() -> Bytes {
    return TakionCodecRig::bytes({
        0x00, 0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x02, 0x03, 0x04, 0x03, 0xa5, 0x00, 0x10, 0x55, 0x66, 0x77,
        0x88, 0x00, 0x01, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00,
    });
  }

  static auto decode_handshake(std::span<const std::byte> wire)
      -> libchiaking::net::protocol::takion::Expected<
          libchiaking::net::protocol::takion::HandshakeControlChunk,
          libchiaking::net::protocol::takion::ParseFailure> {
    namespace takion = libchiaking::net::protocol::takion;
    auto packet = takion::PacketCodec::decode_plain_packet(
        wire, TakionCodecRig::profile(7));
    if (!packet) return takion::unexpected(packet.error());
    EXPECT_EQ(packet.value().type.family(), takion::PacketFamily::control);
    return takion::ControlChunkCodec::decode(packet.value().payload);
  }

  static auto decode_control(std::span<const std::byte> wire)
      -> libchiaking::net::protocol::takion::Expected<
          libchiaking::net::protocol::takion::ControlChunkPacket,
          libchiaking::net::protocol::takion::ParseFailure> {
    namespace takion = libchiaking::net::protocol::takion;
    auto packet = takion::PacketCodec::decode_plain_packet(
        wire, TakionCodecRig::profile(7));
    if (!packet) return takion::unexpected(packet.error());
    EXPECT_EQ(packet.value().type.family(), takion::PacketFamily::control);
    return takion::ControlChunkCodec::decode_packet(packet.value().payload);
  }

  template <class Chunk>
  static auto round_trips_exactly(const Bytes& wire)
      -> testing::AssertionResult {
    namespace takion = libchiaking::net::protocol::takion;
    const auto decoded = decode_handshake(wire);
    if (!decoded.has_value())
      return testing::AssertionFailure() << "the vector does not decode";
    if (!std::holds_alternative<Chunk>(decoded.value()))
      return testing::AssertionFailure()
             << "the vector decodes as another chunk";
    const auto chunk = takion::ControlChunkCodec::encode(decoded.value());
    if (!chunk.has_value())
      return testing::AssertionFailure() << "the chunk does not encode";
    const takion::PlainPacket packet{
        takion::PacketType{wire.front() == std::byte{0x10} ? std::uint8_t{0x10}
                                                           : std::uint8_t{0}},
        std::nullopt,
        chunk.value(),
    };
    const auto encoded = takion::PacketCodec::encode_plain_packet(
        packet, TakionCodecRig::profile(7));
    if (!encoded.has_value())
      return testing::AssertionFailure() << "the packet does not encode";
    if (!std::ranges::equal(encoded.value().bytes(), wire))
      return testing::AssertionFailure() << "the encoded packet differs";
    return testing::AssertionSuccess();
  }
};

}  // namespace takion_test
