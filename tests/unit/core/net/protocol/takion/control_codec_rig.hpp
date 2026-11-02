#pragma once

#include "src/core/net/protocol/takion/control_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"
#include "tests/unit/core/net/protocol/takion/control_message_copy.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace takion_test {

class ProtoWire {
 public:
  using Bytes = std::vector<std::byte>;

  static auto encode_varint(std::uint64_t value) -> Bytes {
    Bytes result;
    do {
      auto encoded = static_cast<std::uint8_t>(value & 0x7f);
      value >>= 7;
      if (value != 0) encoded |= 0x80;
      result.push_back(static_cast<std::byte>(encoded));
    } while (value != 0);
    return result;
  }

  static auto control(std::uint8_t type, std::uint32_t payload_field,
                      const Bytes& payload = {}) -> Bytes {
    ProtoWire result;
    result.varint(1, type);
    if (payload_field != 0) {
      result.key(payload_field, 2);
      result.length_prefixed(payload);
    }
    return result;
  }

  auto key(std::uint32_t field, std::uint32_t wire_format) -> ProtoWire& {
    return raw_varint((static_cast<std::uint64_t>(field) << 3) | wire_format);
  }

  auto raw_varint(std::uint64_t value) -> ProtoWire& {
    return raw(encode_varint(value));
  }

  auto raw(const Bytes& value) -> ProtoWire& {
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return *this;
  }

  auto length_prefixed(const Bytes& value) -> ProtoWire& {
    raw_varint(value.size());
    return raw(value);
  }

  auto varint(std::uint32_t field, std::uint64_t value) -> ProtoWire& {
    key(field, 0);
    return raw_varint(value);
  }

  auto length(std::uint32_t field, const Bytes& value) -> ProtoWire& {
    key(field, 2);
    return length_prefixed(value);
  }

  auto message(std::uint32_t field, const Bytes& value) -> ProtoWire& {
    return length(field, value);
  }

  auto string(std::uint32_t field, std::string_view value) -> ProtoWire& {
    key(field, 2);
    raw_varint(value.size());
    for (const auto character : value)
      bytes_.push_back(static_cast<std::byte>(character));
    return *this;
  }

  auto fixed32(std::uint32_t field, std::uint32_t value) -> ProtoWire& {
    key(field, 5);
    for (unsigned shift = 0; shift < 32; shift += 8)
      bytes_.push_back(static_cast<std::byte>(value >> shift));
    return *this;
  }

  auto fixed64(std::uint32_t field, std::uint64_t value) -> ProtoWire& {
    key(field, 1);
    for (unsigned shift = 0; shift < 64; shift += 8)
      bytes_.push_back(static_cast<std::byte>(value >> shift));
    return *this;
  }

  void clear() { bytes_.clear(); }

  auto bytes() const -> const Bytes& { return bytes_; }

  operator const Bytes&() const { return bytes_; }

 private:
  Bytes bytes_;
};

class TakionControlCodecTest : public ::testing::Test {
 protected:
  using ByteVector = std::vector<std::byte>;

  template <class T>
  auto decoded_as(std::span<const std::byte> wire) const -> T {
    auto result = codec.decode(wire);
    EXPECT_TRUE(result);
    if (!result) return T{};
    EXPECT_TRUE(std::holds_alternative<T>(result.value()));
    if (!std::holds_alternative<T>(result.value())) return T{};
    return std::get<T>(std::move(result).value());
  }

  template <class T>
  auto round_trip(const T& value, const std::optional<ByteVector>& expected =
                                      std::nullopt) const -> ByteVector {
    const auto encoded = codec.encode(
        libchiaking::net::protocol::takion::ControlMessage{copy_of(value)});
    EXPECT_TRUE(encoded);
    if (!encoded) return {};
    if (expected) {
      EXPECT_TRUE(std::ranges::equal(encoded.value().bytes(), *expected));
    }
    EXPECT_EQ(decoded_as<T>(encoded.value()), value);
    return ByteVector(encoded.value().begin(), encoded.value().end());
  }

  void expect_decode_error(
      std::span<const std::byte> wire,
      libchiaking::net::protocol::takion::ControlCodecErrorReason reason)
      const {
    const auto result = codec.decode(wire);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().reason, reason);
  }

  auto no_strict_prefix_decodes(const ByteVector& wire) const
      -> testing::AssertionResult {
    for (std::size_t length = 0; length < wire.size(); ++length)
      if (codec.decode(std::span<const std::byte>{wire.data(), length}))
        return testing::AssertionFailure()
               << "the prefix of " << length << " bytes decodes";
    return testing::AssertionSuccess();
  }

  auto prefixes_never_decode_partially(const ByteVector& wire) const
      -> testing::AssertionResult {
    for (std::size_t length = 0; length < wire.size(); ++length) {
      const auto result =
          codec.decode(std::span<const std::byte>{wire.data(), length});
      if (result) {
        // A prefix that ends exactly on a top-level field boundary can be
        // a valid message with later optional fields omitted. Partial
        // fields must never decode.
        std::size_t position = 0;
        bool complete = false;
        while (position < length) {
          std::uint64_t key_value = 0;
          unsigned shift = 0;
          bool finished = false;
          while (position < length) {
            const auto encoded =
                std::to_integer<std::uint8_t>(wire[position++]);
            key_value |= static_cast<std::uint64_t>(encoded & 0x7fU) << shift;
            if ((encoded & 0x80U) == 0) {
              finished = true;
              break;
            }
            shift += 7;
          }
          if (!finished) {
            complete = false;
            break;
          }
          const auto format = key_value & 7;
          std::size_t value_size = 0;
          if (format == 0) {
            bool value_finished = false;
            while (position < length) {
              const auto encoded =
                  std::to_integer<std::uint8_t>(wire[position++]);
              if ((encoded & 0x80U) == 0) {
                value_finished = true;
                break;
              }
            }
            if (!value_finished) {
              complete = false;
              break;
            }
          } else if (format == 1) {
            value_size = 8;
          } else if (format == 2) {
            std::uint64_t encoded_length = 0;
            shift = 0;
            bool length_finished = false;
            while (position < length) {
              const auto encoded =
                  std::to_integer<std::uint8_t>(wire[position++]);
              encoded_length |= static_cast<std::uint64_t>(encoded & 0x7fU)
                                << shift;
              if ((encoded & 0x80U) == 0) {
                length_finished = true;
                break;
              }
              shift += 7;
            }
            if (!length_finished || encoded_length > length - position) {
              complete = false;
              break;
            }
            value_size = static_cast<std::size_t>(encoded_length);
          } else if (format == 5) {
            value_size = 4;
          } else {
            complete = false;
            break;
          }
          if (value_size != 0) {
            if (length - position < value_size) {
              complete = false;
              break;
            }
            position += value_size;
          }
          complete = position == length;
        }
        if (!complete)
          return testing::AssertionFailure()
                 << "the prefix of " << length << " bytes decodes";
      }
    }
    return testing::AssertionSuccess();
  }

  static auto full_event()
      -> libchiaking::net::protocol::takion::DisconnectEventCode {
    return libchiaking::net::protocol::takion::DisconnectEventCode{
        "EVENT", "name", std::optional<std::string>{"description"}};
  }

  static auto full_big() -> libchiaking::net::protocol::takion::Big {
    libchiaking::net::protocol::takion::Big value;
    value.client_version = 20;
    value.session_key =
        libchiaking::net::protocol::takion::SharedSecretText{"session-key"};
    value.launch_spec =
        libchiaking::net::protocol::takion::SharedSecretText{"launch-spec"};
    value.encrypted_key = TakionCodecRig::bytes({0x01, 0x02, 0x03, 0x04});
    value.ecdh_pub_key = TakionCodecRig::bytes({0x05});
    value.ecdh_sig = TakionCodecRig::bytes({0x06});
    return value;
  }

  static auto full_bang() -> libchiaking::net::protocol::takion::Bang {
    libchiaking::net::protocol::takion::Bang value;
    value.server_version = 20;
    value.token = 0x123456;
    value.encrypted_key_accepted = true;
    value.version_accepted = true;
    value.session_key =
        libchiaking::net::protocol::takion::SharedSecretText{"session-key"};
    value.extended_info = full_event();
    value.server_version_string = "server";
    value.ecdh_pub_key = TakionCodecRig::bytes({0x07});
    value.ecdh_sig = TakionCodecRig::bytes({0x08});
    return value;
  }

  static auto full_bang_with_acknowledgement()
      -> libchiaking::net::protocol::takion::Bang {
    auto value = full_bang();
    value.protocol_ack =
        libchiaking::net::protocol::takion::ProtocolRequestAck{20};
    return value;
  }

  static auto full_stream_info()
      -> libchiaking::net::protocol::takion::StreamInfo {
    libchiaking::net::protocol::takion::StreamInfo value;
    value.resolutions.push_back(
        libchiaking::net::protocol::takion::StreamInfoResolution{
            1920, 1080, TakionCodecRig::bytes({0x56})});
    value.resolutions.push_back(
        libchiaking::net::protocol::takion::StreamInfoResolution{
            1280, 720, TakionCodecRig::bytes({0x57, 0x58})});
    value.audio_header = TakionCodecRig::bytes({0xaa});
    value.start_timeout = 1000;
    value.afk_timeout = 500;
    value.afk_timeout_disconnect = 1000;
    value.congestion_control_interval = 300;
    value.audio_channels.push_back(
        libchiaking::net::protocol::takion::StreamInfoAudioChannel{
            2,
            TakionCodecRig::bytes({0xbb}),
            true,
            {TakionCodecRig::bytes({0x01, 0x02}), TakionCodecRig::bytes({})},
            2});
    value.audio_channels.push_back(
        libchiaking::net::protocol::takion::StreamInfoAudioChannel{
            3, TakionCodecRig::bytes({0xcc}), false});
    return value;
  }

  static auto full_mtu_command()
      -> libchiaking::net::protocol::takion::SenkushaMtuCommand {
    libchiaking::net::protocol::takion::SenkushaMtuCommand command;
    command.id = 9;
    command.mtu_request = 1500;
    command.mtu_sent = 1472;
    command.number = 3;
    command.send_delay = 40;
    command.delta = 28;
    return command;
  }

  static auto full_bandwidth_command()
      -> libchiaking::net::protocol::takion::SenkushaBandwidthCommand {
    libchiaking::net::protocol::takion::SenkushaBandwidthCommand command;
    command.id = 10;
    command.time = 123456;
    command.bandwidth = 20'000'000;
    command.fps = 60;
    command.mtu = 1500;
    command.nalu_count = 7;
    command.total_sent = 987654;
    command.upstream = true;
    command.loss = 0.5;
    return command;
  }

  static auto full_client_mtu_command()
      -> libchiaking::net::protocol::takion::SenkushaClientMtuCommand {
    libchiaking::net::protocol::takion::SenkushaClientMtuCommand command;
    command.id = 11;
    command.mtu_request = 1454;
    command.state = true;
    command.mtu_down = 1400;
    return command;
  }

  static auto full_connection_quality()
      -> libchiaking::net::protocol::takion::ConnectionQuality {
    libchiaking::net::protocol::takion::ConnectionQuality value;
    value.target_bitrate = 1000;
    value.upstream_bitrate = 2000;
    value.upstream_loss = 0.5f;
    value.disable_upstream_audio = true;
    value.rtt = 0.5;
    value.loss = 300;
    return value;
  }

  static auto expected_full_big() -> ByteVector {
    ProtoWire payload;
    payload.varint(1, 20);
    payload.string(2, "session-key");
    payload.string(3, "launch-spec");
    payload.length(4, TakionCodecRig::bytes({0x01, 0x02, 0x03, 0x04}));
    payload.length(5, TakionCodecRig::bytes({0x05}));
    payload.length(6, TakionCodecRig::bytes({0x06}));

    ProtoWire wire;
    wire.varint(1, 0);
    wire.message(2, payload);
    return wire;
  }

  static auto expected_full_bang_with_acknowledgement() -> ByteVector {
    ProtoWire event;
    event.string(1, "EVENT");
    event.string(2, "name");
    event.string(3, "description");

    ProtoWire payload;
    payload.varint(1, 20);
    payload.varint(2, 0x123456);
    payload.varint(3, 1);
    payload.varint(4, 1);
    payload.string(5, "session-key");
    payload.message(6, event);
    payload.string(7, "server");
    payload.length(8, TakionCodecRig::bytes({0x07}));
    payload.length(9, TakionCodecRig::bytes({0x08}));

    ProtoWire acknowledgement;
    acknowledgement.varint(1, 20);

    ProtoWire wire;
    wire.varint(1, 1);
    wire.message(3, payload);
    wire.message(32, acknowledgement);
    return wire;
  }

  static auto expected_full_stream_info_payload() -> ProtoWire {
    ProtoWire first_resolution;
    first_resolution.varint(1, 1920);
    first_resolution.varint(2, 1080);
    first_resolution.length(3, TakionCodecRig::bytes({0x56}));

    ProtoWire second_resolution;
    second_resolution.varint(1, 1280);
    second_resolution.varint(2, 720);
    second_resolution.length(3, TakionCodecRig::bytes({0x57, 0x58}));

    ProtoWire first_channel;
    first_channel.varint(1, 2);
    first_channel.length(2, TakionCodecRig::bytes({0xbb}));
    first_channel.varint(3, 1);
    first_channel.length(4, TakionCodecRig::bytes({0x01, 0x02}));
    first_channel.length(4, TakionCodecRig::bytes({}));
    first_channel.varint(5, 2);

    ProtoWire second_channel;
    second_channel.varint(1, 3);
    second_channel.length(2, TakionCodecRig::bytes({0xcc}));
    second_channel.varint(3, 0);

    ProtoWire payload;
    payload.message(1, first_resolution);
    payload.message(1, second_resolution);
    payload.length(2, TakionCodecRig::bytes({0xaa}));
    payload.varint(3, 1000);
    payload.varint(4, 500);
    payload.varint(5, 1000);
    payload.varint(6, 300);
    payload.message(7, first_channel);
    payload.message(7, second_channel);
    return payload;
  }

  static auto expected_full_stream_info() -> ByteVector {
    ProtoWire wire;
    wire.varint(1, 13);
    wire.message(15, expected_full_stream_info_payload());
    return wire;
  }

  static auto expected_full_mtu_command() -> ByteVector {
    ProtoWire command;
    command.varint(1, 9);
    command.varint(2, 1500);
    command.varint(3, 1472);
    command.varint(4, 3);
    command.varint(5, 40);
    command.varint(6, 28);

    ProtoWire payload;
    payload.varint(1, 1);
    payload.message(3, command);

    ProtoWire wire;
    wire.varint(1, 12);
    wire.message(14, payload);
    return wire;
  }

  static auto expected_full_bandwidth_command() -> ByteVector {
    ProtoWire command;
    command.varint(1, 10);
    command.varint(2, 123456);
    command.varint(3, 20'000'000);
    command.varint(4, 60);
    command.varint(5, 1500);
    command.varint(6, 7);
    command.varint(7, 987654);
    command.varint(8, 1);
    command.fixed64(9, std::bit_cast<std::uint64_t>(0.5));

    ProtoWire payload;
    payload.varint(1, 3);
    payload.message(4, command);

    ProtoWire wire;
    wire.varint(1, 12);
    wire.message(14, payload);
    return wire;
  }

  static auto expected_full_client_mtu_command() -> ByteVector {
    ProtoWire command;
    command.varint(1, 11);
    command.varint(2, 1454);
    command.varint(3, 1);
    command.varint(4, 1400);

    ProtoWire payload;
    payload.varint(1, 4);
    payload.message(5, command);

    ProtoWire wire;
    wire.varint(1, 12);
    wire.message(14, payload);
    return wire;
  }

  static auto expected_full_connection_quality() -> ByteVector {
    ProtoWire payload;
    payload.varint(1, 1000);
    payload.varint(2, 2000);
    payload.fixed32(3, std::bit_cast<std::uint32_t>(0.5f));
    payload.varint(4, 1);
    payload.fixed64(5, std::bit_cast<std::uint64_t>(0.5));
    payload.varint(6, 300);

    ProtoWire wire;
    wire.varint(1, 16);
    wire.message(17, payload);
    return wire;
  }

  static auto expected_full_controller_connection() -> ByteVector {
    ProtoWire payload;
    payload.varint(1, static_cast<std::uint64_t>(static_cast<std::uint64_t>(
                          static_cast<std::int64_t>(-1))));
    payload.varint(2, 1);
    payload.varint(3, 6);

    ProtoWire wire;
    wire.varint(1, 21);
    wire.message(22, payload);
    return wire;
  }

  static auto expected_full_protocol_request() -> ByteVector {
    ProtoWire payload;
    payload.varint(1, 7);
    payload.varint(1, 20);

    ProtoWire wire;
    wire.varint(1, 31);
    wire.message(31, payload);
    return wire;
  }

  const libchiaking::net::protocol::takion::ControlCodec codec;
};

}  // namespace takion_test
