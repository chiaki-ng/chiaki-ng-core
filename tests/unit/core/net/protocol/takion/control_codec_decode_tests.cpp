#include "src/core/net/protocol/takion/control_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"
#include "tests/unit/core/net/protocol/takion/control_codec_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionControlCodecDecoding : public TakionControlCodecTest {};

TEST_F(TakionControlCodecDecoding, AcceptAPackedProtocolRequest) {
  ProtoWire packed_payload;
  packed_payload.length(1, TakionCodecRig::bytes({0x07, 0x14}));
  const auto packed = ProtoWire::control(31, 31, packed_payload);
  const auto decoded = decoded_as<takion::ProtocolRequest>(packed);
  EXPECT_EQ(decoded.supported_versions, (std::vector<std::uint32_t>{7, 20}));
}

TEST_F(TakionControlCodecDecoding, RefuseAMalformedPackedProtocolRequest) {
  ProtoWire malformed_payload;
  malformed_payload.length(1, TakionCodecRig::bytes({0x80}));
  const auto malformed =
      codec.decode(ProtoWire::control(31, 31, malformed_payload));
  ASSERT_FALSE(malformed);
  EXPECT_EQ(malformed.error().reason,
            takion::ControlCodecErrorReason::malformed_message);
}

TEST_F(TakionControlCodecDecoding, KeepRepeatedStreamInfoEntries) {
  ProtoWire resolution;
  resolution.varint(1, 1920);
  resolution.varint(2, 1080);
  resolution.length(3, TakionCodecRig::bytes({}));
  ProtoWire audio_channel;
  audio_channel.varint(1, 2);
  audio_channel.length(2, TakionCodecRig::bytes({}));
  ProtoWire repeated_payload;
  repeated_payload.message(1, resolution);
  repeated_payload.message(1, resolution);
  repeated_payload.length(2, TakionCodecRig::bytes({}));
  repeated_payload.message(7, audio_channel);
  repeated_payload.message(7, audio_channel);
  const auto repeated = decoded_as<takion::StreamInfo>(
      ProtoWire::control(13, 15, repeated_payload));
  EXPECT_EQ(repeated.resolutions.size(), 2);
  EXPECT_EQ(repeated.audio_channels.size(), 2);
}

TEST_F(TakionControlCodecDecoding, RefuseATruncatedNestedResolution) {
  ProtoWire truncated_resolution_payload;
  truncated_resolution_payload.key(1, 0);
  truncated_resolution_payload.raw(TakionCodecRig::bytes({0x80}));
  ProtoWire truncated_resolution_stream;
  truncated_resolution_stream.message(1, truncated_resolution_payload);
  const auto truncated_resolution =
      codec.decode(ProtoWire::control(13, 15, truncated_resolution_stream));
  ASSERT_FALSE(truncated_resolution);
  EXPECT_EQ(truncated_resolution.error().reason,
            takion::ControlCodecErrorReason::malformed_message);
}

TEST_F(TakionControlCodecDecoding, RefuseATruncatedNestedAudioChannel) {
  ProtoWire truncated_audio_channel_payload;
  truncated_audio_channel_payload.key(2, 2);
  truncated_audio_channel_payload.raw_varint(5);
  truncated_audio_channel_payload.raw(TakionCodecRig::bytes({0xaa}));
  ProtoWire truncated_audio_channel_stream;
  truncated_audio_channel_stream.length(2, TakionCodecRig::bytes({0xaa}));
  truncated_audio_channel_stream.message(7, truncated_audio_channel_payload);
  const auto truncated_audio_channel =
      codec.decode(ProtoWire::control(13, 15, truncated_audio_channel_stream));
  ASSERT_FALSE(truncated_audio_channel);
  EXPECT_EQ(truncated_audio_channel.error().reason,
            takion::ControlCodecErrorReason::malformed_message);
}

TEST_F(TakionControlCodecDecoding, RefuseAResolutionMissingARequiredField) {
  ProtoWire incomplete_resolution_payload;
  incomplete_resolution_payload.varint(1, 1920);
  ProtoWire incomplete_resolution_stream;
  incomplete_resolution_stream.message(1, incomplete_resolution_payload);
  const auto incomplete_resolution =
      codec.decode(ProtoWire::control(13, 15, incomplete_resolution_stream));
  ASSERT_FALSE(incomplete_resolution);
  EXPECT_EQ(incomplete_resolution.error().reason,
            takion::ControlCodecErrorReason::missing_required_field);
}

TEST_F(TakionControlCodecDecoding, RefuseAnAudioChannelMissingARequiredField) {
  ProtoWire incomplete_audio_channel_payload;
  incomplete_audio_channel_payload.length(2, TakionCodecRig::bytes({0xbb}));
  ProtoWire incomplete_audio_channel_stream;
  incomplete_audio_channel_stream.length(2, TakionCodecRig::bytes({0xaa}));
  incomplete_audio_channel_stream.message(7, incomplete_audio_channel_payload);
  const auto incomplete_audio_channel =
      codec.decode(ProtoWire::control(13, 15, incomplete_audio_channel_stream));
  ASSERT_FALSE(incomplete_audio_channel);
  EXPECT_EQ(incomplete_audio_channel.error().reason,
            takion::ControlCodecErrorReason::missing_required_field);
}

TEST_F(TakionControlCodecDecoding,
       RefuseAnUnknownSenkushaCommandWithAnEmptyPayload) {
  ProtoWire uninitialized_payload;
  uninitialized_payload.varint(1, 99);
  uninitialized_payload.message(3, ByteVector{});
  const auto uninitialized =
      codec.decode(ProtoWire::control(12, 14, uninitialized_payload));
  ASSERT_FALSE(uninitialized);
  EXPECT_EQ(uninitialized.error().reason,
            takion::ControlCodecErrorReason::missing_required_field);
}

TEST_F(TakionControlCodecDecoding,
       KeepAnUnknownSenkushaCommandWithoutAPayload) {
  ProtoWire preserved_payload;
  preserved_payload.varint(1, 99);
  const auto preserved =
      codec.decode(ProtoWire::control(12, 14, preserved_payload));
  ASSERT_TRUE(preserved);
  const auto command = std::get<takion::Senkusha>(preserved.value()).command;
  EXPECT_TRUE(std::holds_alternative<takion::UnknownSenkushaCommand>(command));
  EXPECT_EQ(std::get<takion::UnknownSenkushaCommand>(command).command, 99);
}

TEST_F(TakionControlCodecDecoding, RefuseANonCanonicalTenthVarintByte) {
  const auto overflow = codec.decode(TakionCodecRig::bytes(
      {0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f}));
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().reason,
            takion::ControlCodecErrorReason::malformed_message);
}

TEST_F(TakionControlCodecDecoding, KeepUnknownMessageTypesAsUnsupported) {
  const auto unknown_type = codec.decode(TakionCodecRig::bytes({0x08, 0x63}));
  ASSERT_TRUE(unknown_type);
  EXPECT_TRUE(std::holds_alternative<takion::UnsupportedControlMessage>(
      unknown_type.value()));
  EXPECT_EQ(
      static_cast<std::uint32_t>(
          std::get<takion::UnsupportedControlMessage>(unknown_type.value())
              .type),
      99U);

  const auto maximum_unknown_type =
      codec.decode(TakionCodecRig::bytes({0x08, 0xff, 0xff, 0xff, 0xff, 0x0f}));
  ASSERT_TRUE(maximum_unknown_type);
  EXPECT_EQ(
      static_cast<std::uint32_t>(std::get<takion::UnsupportedControlMessage>(
                                     maximum_unknown_type.value())
                                     .type),
      std::numeric_limits<std::uint32_t>::max());
}

TEST_F(TakionControlCodecDecoding, RefuseAMessageTypeBeyond32Bits) {
  const auto oversized_enum =
      codec.decode(TakionCodecRig::bytes({0x08, 0x80, 0x80, 0x80, 0x80, 0x10}));
  ASSERT_FALSE(oversized_enum);
  EXPECT_EQ(oversized_enum.error().reason,
            takion::ControlCodecErrorReason::malformed_message);
}

TEST_F(TakionControlCodecDecoding, RoundTripUnknownSenkushaCommands) {
  ProtoWire unknown_senkusha_payload;
  unknown_senkusha_payload.varint(1, 99);
  const auto unknown_senkusha_wire =
      ProtoWire::control(12, 14, unknown_senkusha_payload);
  const auto unknown_senkusha =
      round_trip(takion::Senkusha{takion::UnknownSenkushaCommand{99}},
                 unknown_senkusha_wire);
  EXPECT_TRUE(std::holds_alternative<takion::UnknownSenkushaCommand>(
      decoded_as<takion::Senkusha>(unknown_senkusha_wire).command));

  ProtoWire negative_senkusha_payload;
  negative_senkusha_payload.key(1, 0);
  negative_senkusha_payload.raw(TakionCodecRig::bytes(
      {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}));
  const auto negative_senkusha_wire =
      ProtoWire::control(12, 14, negative_senkusha_payload);
  round_trip(takion::Senkusha{takion::UnknownSenkushaCommand{-1}},
             negative_senkusha_wire);
}

TEST_F(TakionControlCodecDecoding, RoundTripUnknownControllerTypes) {
  ProtoWire unknown_controller_payload;
  unknown_controller_payload.varint(3, 99);
  const auto unknown_controller_wire =
      ProtoWire::control(21, 22, unknown_controller_payload);
  const auto unknown_controller = round_trip(
      takion::ControllerConnection{
          std::nullopt, std::nullopt,
          takion::ControllerConnectionTypeValue{
              takion::UnknownControllerConnectionType{99}}},
      unknown_controller_wire);
  const auto decoded_unknown_controller =
      decoded_as<takion::ControllerConnection>(unknown_controller_wire);
  ASSERT_TRUE(decoded_unknown_controller.controller_type.has_value());
  EXPECT_TRUE(std::holds_alternative<takion::UnknownControllerConnectionType>(
      *decoded_unknown_controller.controller_type));

  ProtoWire negative_controller_payload;
  negative_controller_payload.key(3, 0);
  negative_controller_payload.raw(TakionCodecRig::bytes(
      {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}));
  const auto negative_controller_wire =
      ProtoWire::control(21, 22, negative_controller_payload);
  round_trip(
      takion::ControllerConnection{
          std::nullopt, std::nullopt,
          takion::ControllerConnectionTypeValue{
              takion::UnknownControllerConnectionType{-1}}},
      negative_controller_wire);
}

TEST_F(TakionControlCodecDecoding, KeepAProtocolAcknowledgementBesideABang) {
  const auto coexisting =
      decoded_as<takion::Bang>(expected_full_bang_with_acknowledgement());
  ASSERT_TRUE(coexisting.protocol_ack.has_value());
  EXPECT_EQ(coexisting.protocol_ack->selected_version, 20);

  auto empty_acknowledgement = full_bang_with_acknowledgement();
  empty_acknowledgement.protocol_ack = takion::ProtocolRequestAck{};
  auto expected_empty_ack = expected_full_bang_with_acknowledgement();
  expected_empty_ack.resize(expected_empty_ack.size() - 2);
  expected_empty_ack.back() = static_cast<std::byte>(0);
  const auto coexisting_empty =
      round_trip(empty_acknowledgement, expected_empty_ack);
  const auto decoded_empty = decoded_as<takion::Bang>(coexisting_empty);
  ASSERT_TRUE(decoded_empty.protocol_ack.has_value());
  EXPECT_FALSE(decoded_empty.protocol_ack->selected_version.has_value());
}

TEST_F(TakionControlCodecDecoding, RefuseAProtocolAcknowledgementWithoutABang) {
  ProtoWire acknowledgement_only;
  acknowledgement_only.varint(1, 20);
  const auto missing_bang =
      codec.decode(ProtoWire::control(1, 32, acknowledgement_only));
  ASSERT_FALSE(missing_bang);
  EXPECT_EQ(missing_bang.error().reason,
            takion::ControlCodecErrorReason::missing_payload);
}

TEST_F(TakionControlCodecDecoding, RefuseMalformedNestedFields) {
  ProtoWire invalid_nested_length;
  invalid_nested_length.key(6, 2);
  invalid_nested_length.raw_varint(5);
  invalid_nested_length.raw(TakionCodecRig::bytes({0x0a, 0x03, 0x41}));
  const auto malformed_nested =
      codec.decode(ProtoWire::control(1, 3, invalid_nested_length));
  ASSERT_FALSE(malformed_nested);
  EXPECT_EQ(malformed_nested.error().reason,
            takion::ControlCodecErrorReason::malformed_message);

  const auto malformed_packed =
      codec.decode(ProtoWire::control(31, 31, TakionCodecRig::bytes({0x80})));
  ASSERT_FALSE(malformed_packed);
  EXPECT_EQ(malformed_packed.error().reason,
            takion::ControlCodecErrorReason::malformed_message);

  ProtoWire truncated_fixed;
  truncated_fixed.key(3, 5);
  truncated_fixed.raw(TakionCodecRig::bytes({0x00, 0x00}));
  const auto malformed_fixed =
      codec.decode(ProtoWire::control(16, 17, truncated_fixed));
  ASSERT_FALSE(malformed_fixed);
  EXPECT_EQ(malformed_fixed.error().reason,
            takion::ControlCodecErrorReason::malformed_message);

  const auto malformed_repeated_client_metric = codec.decode(
      ProtoWire::control(17, 18, TakionCodecRig::bytes({0x0a, 0x01, 0x00})));
  ASSERT_FALSE(malformed_repeated_client_metric);
  EXPECT_EQ(malformed_repeated_client_metric.error().reason,
            takion::ControlCodecErrorReason::malformed_message);
}

TEST_F(TakionControlCodecDecoding, RefuseAMessageBeyondTheAllocationLimit) {
  const auto full_request = expected_full_protocol_request();
  const takion::ControlCodec exact{full_request.size()};
  EXPECT_TRUE(exact.decode(full_request));
  const takion::ControlCodec too_small{full_request.size() - 1};
  const auto allocation_rejected = too_small.decode(full_request);
  ASSERT_FALSE(allocation_rejected);
  EXPECT_EQ(allocation_rejected.error().reason,
            takion::ControlCodecErrorReason::message_too_large);
  EXPECT_EQ(allocation_rejected.error().size, full_request.size());
  EXPECT_EQ(allocation_rejected.error().limit, full_request.size() - 1);
}

TEST_F(TakionControlCodecDecoding, RequireTheInfoFieldsOfAnUnsupportedInfo) {
  const auto invalid_info = codec.decode(ProtoWire::control(2, 4));
  ASSERT_FALSE(invalid_info);
  EXPECT_EQ(invalid_info.error().reason,
            takion::ControlCodecErrorReason::missing_required_field);

  ProtoWire initialized_info;
  for (std::uint32_t field = 1; field <= 13; ++field) {
    initialized_info.varint(field, 0);
  }
  const auto supported_info =
      codec.decode(ProtoWire::control(2, 4, initialized_info));
  ASSERT_TRUE(supported_info);
  EXPECT_TRUE(std::holds_alternative<takion::UnsupportedControlMessage>(
      supported_info.value()));
  EXPECT_EQ(
      std::get<takion::UnsupportedControlMessage>(supported_info.value()).type,
      takion::ControlPayloadType::info);
}

TEST_F(TakionControlCodecDecoding, RequireTheSessionOfAnUnsupportedClientInfo) {
  const auto invalid_client_info = codec.decode(ProtoWire::control(22, 23));
  ASSERT_FALSE(invalid_client_info);
  EXPECT_EQ(invalid_client_info.error().reason,
            takion::ControlCodecErrorReason::missing_required_field);

  ProtoWire client_info;
  client_info.string(1, "session");
  const auto initialized_client_info =
      codec.decode(ProtoWire::control(22, 23, client_info));
  ASSERT_TRUE(initialized_client_info);
  EXPECT_TRUE(std::holds_alternative<takion::UnsupportedControlMessage>(
      initialized_client_info.value()));
}

TEST_F(TakionControlCodecDecoding, RequireTheFieldsOfAMicConnection) {
  const auto invalid_mic_connection = codec.decode(ProtoWire::control(30, 30));
  ASSERT_FALSE(invalid_mic_connection);
  EXPECT_EQ(invalid_mic_connection.error().reason,
            takion::ControlCodecErrorReason::missing_required_field);

  ProtoWire mic_connection;
  mic_connection.varint(1, 1);
  mic_connection.varint(2, 1);
  const auto initialized_mic_connection =
      codec.decode(ProtoWire::control(30, 30, mic_connection));
  ASSERT_TRUE(initialized_mic_connection);
  EXPECT_EQ(std::get<takion::MicConnection>(initialized_mic_connection.value()),
            (takion::MicConnection{1, true, std::nullopt}));
}

TEST_F(TakionControlCodecDecoding, RefuseAnEmptyMessage) {
  expect_decode_error({},
                      takion::ControlCodecErrorReason::missing_required_field);
}

TEST_F(TakionControlCodecDecoding, RefuseMessagesMissingRequiredFields) {
  const ByteVector supported_payload_cases[] = {
      ProtoWire::control(0, 2),
      ProtoWire::control(
          0, 2, TakionCodecRig::bytes({0x12, 0x00, 0x1a, 0x00, 0x22, 0x00})),
      ProtoWire::control(
          0, 2, TakionCodecRig::bytes({0x08, 0x09, 0x1a, 0x00, 0x22, 0x00})),
      ProtoWire::control(
          0, 2, TakionCodecRig::bytes({0x08, 0x09, 0x12, 0x00, 0x22, 0x00})),
      ProtoWire::control(
          0, 2, TakionCodecRig::bytes({0x08, 0x09, 0x12, 0x00, 0x1a, 0x00})),
      ProtoWire::control(1, 3),
      ProtoWire::control(1, 3,
                         TakionCodecRig::bytes(
                             {0x10, 0x22, 0x18, 0x01, 0x20, 0x01, 0x2a, 0x00})),
      ProtoWire::control(1, 3,
                         TakionCodecRig::bytes(
                             {0x08, 0x0c, 0x18, 0x01, 0x20, 0x01, 0x2a, 0x00})),
      ProtoWire::control(1, 3,
                         TakionCodecRig::bytes(
                             {0x08, 0x0c, 0x10, 0x22, 0x20, 0x01, 0x2a, 0x00})),
      ProtoWire::control(1, 3,
                         TakionCodecRig::bytes(
                             {0x08, 0x0c, 0x10, 0x22, 0x18, 0x01, 0x2a, 0x00})),
      ProtoWire::control(1, 3,
                         TakionCodecRig::bytes(
                             {0x08, 0x0c, 0x10, 0x22, 0x18, 0x01, 0x20, 0x01})),
      ProtoWire::control(8, 10),
      ProtoWire::control(8, 10, TakionCodecRig::bytes({})),
      ProtoWire::control(13, 15, TakionCodecRig::bytes({})),
      ProtoWire::control(13, 15, TakionCodecRig::bytes({0x0a, 0x00})),
      ProtoWire::control(5, 6),
      ProtoWire::control(5, 6, TakionCodecRig::bytes({0x10, 0x02})),
      ProtoWire::control(5, 6, TakionCodecRig::bytes({0x08, 0x01})),
  };
  for (const auto& wire : supported_payload_cases) {
    expect_decode_error(
        wire, takion::ControlCodecErrorReason::missing_required_field);
  }
}

TEST_F(TakionControlCodecDecoding, RefuseMessagesMissingTheirPayload) {
  const ByteVector missing_payload_cases[] = {
      ProtoWire::control(0, 0),  ProtoWire::control(1, 0),
      ProtoWire::control(5, 0),  ProtoWire::control(8, 0),
      ProtoWire::control(12, 0), ProtoWire::control(13, 0),
      ProtoWire::control(16, 0), ProtoWire::control(21, 0),
      ProtoWire::control(31, 0), ProtoWire::control(32, 0),
  };
  for (const auto& wire : missing_payload_cases)
    expect_decode_error(wire, takion::ControlCodecErrorReason::missing_payload);
}

TEST_F(TakionControlCodecDecoding, RefuseStreamInfoWithIncompleteEntries) {
  ProtoWire stream_payload;
  ProtoWire resolution;
  resolution.varint(1, 1920);
  stream_payload.length(1, resolution);
  stream_payload.length(2, TakionCodecRig::bytes({0xaa}));
  expect_decode_error(ProtoWire::control(13, 15, stream_payload),
                      takion::ControlCodecErrorReason::missing_required_field);

  ProtoWire channel;
  channel.length(2, TakionCodecRig::bytes({0xbb}));
  stream_payload.clear();
  stream_payload.length(2, TakionCodecRig::bytes({0xaa}));
  stream_payload.key(7, 2);
  stream_payload.length_prefixed(channel);
  expect_decode_error(ProtoWire::control(13, 15, stream_payload),
                      takion::ControlCodecErrorReason::missing_required_field);
}

TEST_F(TakionControlCodecDecoding,
       RefuseIncompleteSenkushaAndNegotiationMessages) {
  expect_decode_error(ProtoWire::control(12, 0),
                      takion::ControlCodecErrorReason::missing_payload);
  expect_decode_error(ProtoWire::control(12, 14, TakionCodecRig::bytes({})),
                      takion::ControlCodecErrorReason::missing_required_field);
  expect_decode_error(
      ProtoWire::control(12, 14, TakionCodecRig::bytes({0x08, 0x00})),
      takion::ControlCodecErrorReason::missing_payload);
  expect_decode_error(
      ProtoWire::control(12, 14, TakionCodecRig::bytes({0x08, 0x01})),
      takion::ControlCodecErrorReason::missing_payload);
  expect_decode_error(
      ProtoWire::control(
          12, 14, TakionCodecRig::bytes({0x08, 0x01, 0x1a, 0x02, 0x08, 0x01})),
      takion::ControlCodecErrorReason::missing_required_field);
  expect_decode_error(
      ProtoWire::control(12, 14, TakionCodecRig::bytes({0x08, 0x03})),
      takion::ControlCodecErrorReason::missing_payload);
  expect_decode_error(
      ProtoWire::control(12, 14, TakionCodecRig::bytes({0x08, 0x04})),
      takion::ControlCodecErrorReason::missing_payload);
  expect_decode_error(ProtoWire::control(31, 0),
                      takion::ControlCodecErrorReason::missing_payload);
  expect_decode_error(ProtoWire::control(32, 0),
                      takion::ControlCodecErrorReason::missing_payload);
}

TEST_F(TakionControlCodecDecoding, RefuseMalformedWire) {
  const ByteVector malformed_cases[] = {
      TakionCodecRig::bytes({0x00}),
      TakionCodecRig::bytes({0x07}),
      TakionCodecRig::bytes({0x08}),
      TakionCodecRig::bytes({0x08, 0x03, 0x0c}),
      TakionCodecRig::bytes({0x08, 0x03, 0xa0, 0x06}),
      TakionCodecRig::bytes({0x08, 0x0c, 0x72, 0x06, 0x08, 0x02}),
  };
  for (const auto& wire : malformed_cases) {
    const auto result = codec.decode(wire);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().reason,
              takion::ControlCodecErrorReason::malformed_message);
  }
}

TEST_F(TakionControlCodecDecoding, SkipUnknownFields) {
  const auto heartbeat_unknown =
      codec.decode(TakionCodecRig::bytes({0x08, 0x03, 0xa0, 0x06, 0x01}));
  ASSERT_TRUE(heartbeat_unknown);
  EXPECT_TRUE(
      std::holds_alternative<takion::Heartbeat>(heartbeat_unknown.value()));

  const auto heartbeat_unknown_bytes = codec.decode(
      TakionCodecRig::bytes({0x08, 0x03, 0xf2, 0x1e, 0x02, 0xaa, 0xbb}));
  ASSERT_TRUE(heartbeat_unknown_bytes);
  EXPECT_TRUE(std::holds_alternative<takion::Heartbeat>(
      heartbeat_unknown_bytes.value()));

  const auto big_unknown_nested = codec.decode(
      TakionCodecRig::bytes({0x08, 0x00, 0x12, 0x0b, 0x08, 0x09, 0x12, 0x00,
                             0x1a, 0x00, 0x22, 0x00, 0xf8, 0x01, 0x01}));
  ASSERT_TRUE(big_unknown_nested);
  EXPECT_TRUE(std::holds_alternative<takion::Big>(big_unknown_nested.value()));
}

TEST_F(TakionControlCodecDecoding, KeepTheInfoTypeAsUnsupported) {
  const auto unsupported = codec.decode(TakionCodecRig::bytes({0x08, 0x02}));
  ASSERT_TRUE(unsupported);
  EXPECT_TRUE(std::holds_alternative<takion::UnsupportedControlMessage>(
      unsupported.value()));
  EXPECT_EQ(
      std::get<takion::UnsupportedControlMessage>(unsupported.value()).type,
      takion::ControlPayloadType::info);

  const auto unsupported_with_invalid_payload =
      codec.decode(ProtoWire::control(2, 4));
  ASSERT_FALSE(unsupported_with_invalid_payload);
  EXPECT_EQ(unsupported_with_invalid_payload.error().reason,
            takion::ControlCodecErrorReason::missing_required_field);
}

TEST_F(TakionControlCodecDecoding, KeepEveryOtherKnownTypeAsUnsupported) {
  const takion::ControlPayloadType remaining_known_types[] = {
      takion::ControlPayloadType::info,
      takion::ControlPayloadType::packet_loss,
      takion::ControlPayloadType::cursor,
      takion::ControlPayloadType::timer,
      takion::ControlPayloadType::log,
      takion::ControlPayloadType::header_request,
      takion::ControlPayloadType::debug,
      takion::ControlPayloadType::client_metric,
      takion::ControlPayloadType::fps_change,
      takion::ControlPayloadType::client_info,
      takion::ControlPayloadType::video_capture,
      takion::ControlPayloadType::audio_capture,
      takion::ControlPayloadType::gk_trace,
      takion::ControlPayloadType::periodic_timestamp,
      takion::ControlPayloadType::server_settings,
      takion::ControlPayloadType::direct_message,
  };
  for (const auto type : remaining_known_types) {
    const auto result = codec.decode(
        TakionCodecRig::bytes({0x08, static_cast<std::uint8_t>(type)}));
    ASSERT_TRUE(result);
    EXPECT_TRUE(std::holds_alternative<takion::UnsupportedControlMessage>(
        result.value()));
    EXPECT_EQ(std::get<takion::UnsupportedControlMessage>(result.value()).type,
              type);
  }
  EXPECT_FALSE(codec.decode(TakionCodecRig::bytes(
      {0x08,
       static_cast<std::uint8_t>(takion::ControlPayloadType::xmb_command)})));
  EXPECT_FALSE(codec.decode(TakionCodecRig::bytes(
      {0x08, static_cast<std::uint8_t>(
                 takion::ControlPayloadType::mic_connection)})));
  EXPECT_FALSE(codec.decode(TakionCodecRig::bytes(
      {0x08, static_cast<std::uint8_t>(
                 takion::ControlPayloadType::play_time_left)})));
  EXPECT_FALSE(codec.decode(TakionCodecRig::bytes(
      {0x08, static_cast<std::uint8_t>(
                 takion::ControlPayloadType::server_message)})));

  const auto unknown_type = codec.decode(TakionCodecRig::bytes({0x08, 0x63}));
  ASSERT_TRUE(unknown_type);
  EXPECT_TRUE(std::holds_alternative<takion::UnsupportedControlMessage>(
      unknown_type.value()));
  EXPECT_EQ(
      static_cast<std::uint32_t>(
          std::get<takion::UnsupportedControlMessage>(unknown_type.value())
              .type),
      99U);
}

TEST_F(TakionControlCodecDecoding, RefuseToEncodeAnUnsupportedMessage) {
  const auto unsupported_encoding = codec.encode(takion::ControlMessage{
      takion::UnsupportedControlMessage{takion::ControlPayloadType::info}});
  ASSERT_FALSE(unsupported_encoding);
  EXPECT_EQ(unsupported_encoding.error().reason,
            takion::ControlCodecErrorReason::unsupported_for_encoding);
}

TEST_F(TakionControlCodecDecoding, RefuseIncompleteOrUnknownAudioState) {
  const auto missing_payload =
      codec.decode(TakionCodecRig::bytes({0x08, 0x21}));
  ASSERT_FALSE(missing_payload);
  EXPECT_EQ(missing_payload.error().reason,
            takion::ControlCodecErrorReason::missing_payload);

  const auto unknown_kind = codec.decode(
      TakionCodecRig::bytes({0x08, 0x21, 0x8a, 0x02, 0x02, 0x08, 0x07}));
  ASSERT_FALSE(unknown_kind);
  EXPECT_EQ(unknown_kind.error().reason,
            takion::ControlCodecErrorReason::missing_required_field);

  const auto oversized_kind = codec.decode(TakionCodecRig::bytes(
      {0x08, 0x21, 0x8a, 0x02, 0x07, 0x08, 0x80, 0x80, 0x80, 0x80, 0x10}));
  ASSERT_FALSE(oversized_kind);
  EXPECT_EQ(oversized_kind.error().reason,
            takion::ControlCodecErrorReason::malformed_message);

  const auto rejected = codec.encode(takion::ControlMessage{takion::AudioState{
      static_cast<takion::AudioStateType>(7), ByteVector{}}});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().reason,
            takion::ControlCodecErrorReason::unsupported_for_encoding);
}

TEST_F(TakionControlCodecDecoding, KeepTypesBeyondAudioStateAsUnsupported) {
  const auto beyond = codec.decode(TakionCodecRig::bytes({0x08, 0x22}));
  ASSERT_TRUE(beyond);
  EXPECT_EQ(std::get<takion::UnsupportedControlMessage>(beyond.value()).type,
            static_cast<takion::ControlPayloadType>(34));
}

}  // namespace
}  // namespace takion_test
