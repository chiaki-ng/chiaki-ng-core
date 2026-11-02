#include "src/core/net/protocol/takion/detail/typed_message_codec.hpp"

#include "src/core/net/protocol/takion/nanopb_message.hpp"
#include "src/core/net/protocol/takion/result.hpp"
#include "src/core/utils/bytes/byte_io.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace libchiaking::net::protocol::takion {

/// @brief Builds and reads the generated nanopb messages for
/// TypedMessageCodec.
class TypedMessageCodec::Wire {
 public:
  /// @brief Serialises a protobuf message.
  ///
  /// @tparam Message A NanopbMessage.
  /// @param message The message.
  /// @param family The family the failure names.
  /// @return The bytes.
  /// @retval ParseFailureReason::invalid_length It cannot be serialised.
  template <class Message>
  [[nodiscard]] static auto serialize_message(const Message& message,
                                              PacketFamily family)
      -> Expected<std::vector<std::byte>, ParseFailure>;
  /// @brief Parses a GKTRACE message.
  ///
  /// @param bytes The message bytes.
  /// @param message Receives it.
  /// @return Nothing.
  /// @retval ParseFailureReason::invalid_field Malformed or incomplete.
  /// @retval ParseFailureReason::unsupported_layout Another message type.
  [[nodiscard]] static auto parse_trace(std::span<const std::byte> bytes,
                                        TakionWire& message)
      -> Expected<void, ParseFailure>;
  /// @brief Copies a control message into its protobuf.
  ///
  /// @param payload The protobuf.
  /// @param control The message.
  static void assign_generic_data(tkproto_GenericDataPayload& payload,
                                  const GenericDataControl& control);
};

template <class Message>
[[nodiscard]] auto TypedMessageCodec::Wire::serialize_message(
    const Message& message, PacketFamily family)
    -> Expected<std::vector<std::byte>, ParseFailure> {
  auto bytes = message.template encode<std::vector<std::byte>>(
      static_cast<std::size_t>(std::numeric_limits<int>::max()));
  if (!bytes)
    return unexpected(ParseFailure::of(PacketType::of(family),
                                       ParseFailureReason::invalid_length));
  return std::move(bytes).value();
}

[[nodiscard]] auto TypedMessageCodec::Wire::parse_trace(
    std::span<const std::byte> bytes, TakionWire& message)
    -> Expected<void, ParseFailure> {
  if (!message.decode(bytes))
    return unexpected(ParseFailure::of(PacketType::of(PacketFamily::trace),
                                       ParseFailureReason::invalid_field));
  if (message->type != tkproto_TakionMessage_PayloadType_GKTRACE ||
      !message->has_gk_trace_payload)
    return unexpected(ParseFailure::of(PacketType::of(PacketFamily::trace),
                                       ParseFailureReason::unsupported_layout));
  return {};
}

void TypedMessageCodec::Wire::assign_generic_data(
    tkproto_GenericDataPayload& payload, const GenericDataControl& control) {
  payload.transfer_id = control.transfer_id;
  payload.type = control.type;
  if (control.data_size) {
    payload.has_data_size = true;
    payload.data_size = *control.data_size;
  }
  if (control.metadata)
    GenericDataWire::assign(payload.metadata, *control.metadata);
  if (control.packet_count) {
    payload.has_num_packets = true;
    payload.num_packets = *control.packet_count;
  }
  GenericDataWire::assign(payload.error, control.error.value_or(std::string{}));
  std::ranges::copy(control.missing_packets,
                    GenericDataWire::resize(payload.missing_packets,
                                            payload.missing_packets_count,
                                            control.missing_packets.size())
                        .begin());
}

auto TypedMessageCodec::encode_length_prefixed_body(
    std::span<const std::byte> payload)
    -> Expected<std::vector<std::byte>, ParseFailure> {
  if (payload.size() > length_prefixed_payload_limit)
    return unexpected(
        ParseFailure::at(PacketType::of(PacketFamily::length_prefixed),
                         ParseFailureReason::invalid_length, 1,
                         length_prefixed_payload_limit, payload.size()));
  std::vector<std::byte> body;
  body.reserve(length_prefixed_header_size - packet_type_size + payload.size());
  bytes::Appender output{body};
  output.u16_be(static_cast<std::uint16_t>(payload.size()));
  output.bytes(payload);
  return body;
}

auto TypedMessageCodec::decode_length_prefixed_body(
    std::span<const std::byte> body)
    -> Expected<std::vector<std::byte>, ParseFailure> {
  bytes::Reader reader{body};
  const auto length = reader.u16_be();
  if (!length)
    return unexpected(ParseFailure::short_by(
        PacketType::of(PacketFamily::length_prefixed), length.error()));
  const auto payload = reader.take(length.value());
  if (!payload)
    return unexpected(ParseFailure::short_by(
        PacketType::of(PacketFamily::length_prefixed), payload.error()));
  return std::vector<std::byte>{payload.value().begin(), payload.value().end()};
}

auto TypedMessageCodec::encode_client_info_message(
    const ClientInfoValues& values)
    -> Expected<std::vector<std::byte>, ParseFailure> {
  if (!values.session_key)
    return unexpected(
        ParseFailure::of(PacketType::of(PacketFamily::client_information),
                         ParseFailureReason::invalid_field));
  TakionWire message;
  message->type =
      TakionWire::number(tkproto_TakionMessage_PayloadType_CLIENTINFO);
  message->has_client_info_payload = true;
  auto& payload = message->client_info_payload;
  TakionWire::assign(payload.session_key, values.session_key->view());
  if (values.tag) {
    payload.has_gcm_tag = true;
    payload.gcm_tag = *values.tag;
  }
  if (values.key_position) {
    payload.has_key_pos = true;
    payload.key_pos = *values.key_position;
  }
  return Wire::serialize_message(message, PacketFamily::client_information);
}

auto TypedMessageCodec::decode_client_info_message(
    std::span<const std::byte> bytes)
    -> Expected<ClientInfoValues, ParseFailure> {
  TakionWire message;
  if (!message.decode(bytes))
    return unexpected(
        ParseFailure::of(PacketType::of(PacketFamily::client_information),
                         ParseFailureReason::invalid_field));
  if (message->type != tkproto_TakionMessage_PayloadType_CLIENTINFO ||
      !message->has_client_info_payload)
    return unexpected(
        ParseFailure::of(PacketType::of(PacketFamily::client_information),
                         ParseFailureReason::unsupported_layout));
  const auto& payload = message->client_info_payload;
  ClientInfoValues values;
  values.session_key = std::make_shared<const SecretString>(
      TakionWire::text_of(payload.session_key));
  if (payload.has_gcm_tag) values.tag = payload.gcm_tag;
  if (payload.has_key_pos) values.key_position = payload.key_pos;
  return values;
}

auto TypedMessageCodec::encode_trace_message(const TraceValues& values)
    -> Expected<std::vector<std::byte>, ParseFailure> {
  TakionWire message;
  message->type = TakionWire::number(tkproto_TakionMessage_PayloadType_GKTRACE);
  message->has_gk_trace_payload = true;
  auto& trace = message->gk_trace_payload;
  trace.seq_no = values.sequence;
  trace.gcm_tag = values.tag;
  trace.key_pos = values.key_position;
  return Wire::serialize_message(message, PacketFamily::trace);
}

auto TypedMessageCodec::decode_trace_message(std::span<const std::byte> bytes)
    -> Expected<TraceValues, ParseFailure> {
  TakionWire message;
  if (auto parsed = Wire::parse_trace(bytes, message); !parsed)
    return unexpected(parsed.error());
  const auto& trace = message->gk_trace_payload;
  return TraceValues{trace.seq_no, trace.gcm_tag, trace.key_pos};
}

auto TypedMessageCodec::trace_authenticated_message(
    std::span<const std::byte> bytes)
    -> Expected<std::vector<std::byte>, ParseFailure> {
  TakionWire message;
  if (auto parsed = Wire::parse_trace(bytes, message); !parsed)
    return unexpected(parsed.error());
  message->gk_trace_payload.gcm_tag = 0;
  message->gk_trace_payload.key_pos = 0;
  return Wire::serialize_message(message, PacketFamily::trace);
}

auto TypedMessageCodec::encode_generic_data_chunk_body(
    const GenericDataChunk& chunk, const WireProfile& profile)
    -> Expected<SecretBytes, ParseFailure> {
  if (profile.version() < generic_data_minimum_version)
    return unexpected(
        ParseFailure::of(PacketType::of(PacketFamily::generic_data),
                         ParseFailureReason::unsupported_layout));
  if (chunk.payload.size() > std::numeric_limits<std::uint32_t>::max())
    return unexpected(
        ParseFailure::of(PacketType::of(PacketFamily::generic_data),
                         ParseFailureReason::invalid_length));
  SecretBytes body;
  body.reserve(generic_data_fields_size + chunk.payload.size());
  body.append(chunk.tag);
  bytes::append_be(body, chunk.key_position);
  bytes::append_be(body, chunk.transfer_id);
  body.push_back(std::byte{chunk.packet_index});
  bytes::append_be(body, static_cast<std::uint32_t>(chunk.payload.size()));
  bytes::append_be(body, chunk.transfer_offset);
  body.append(chunk.payload.bytes());
  return body;
}

auto TypedMessageCodec::decode_generic_data_chunk(const PlainPacket& packet,
                                                  const WireProfile& profile)
    -> Expected<GenericDataChunk, ParseFailure> {
  if (packet.type.family() != PacketFamily::generic_data)
    return unexpected(ParseFailure::of(
        packet.type, ParseFailureReason::unknown_packet_family));
  if (profile.version() < generic_data_minimum_version)
    return unexpected(
        ParseFailure::of(PacketType::of(PacketFamily::generic_data),
                         ParseFailureReason::unsupported_layout));
  const auto base = profile.packet_header_size(packet.type);
  bytes::Reader reader{packet.payload};
  const auto truncated = [base](const bytes::Shortfall& error) {
    return ParseFailure::short_by(PacketType::of(PacketFamily::generic_data),
                                  error, base);
  };
  GenericDataChunk chunk;
  const auto tag = reader.take(takion_mac_size);
  if (!tag) return unexpected(truncated(tag.error()));
  std::copy(tag.value().begin(), tag.value().end(), chunk.tag.begin());
  const auto key_position = reader.u32_be();
  if (!key_position) return unexpected(truncated(key_position.error()));
  const auto transfer_id = reader.u32_be();
  if (!transfer_id) return unexpected(truncated(transfer_id.error()));
  const auto index = reader.u8();
  if (!index) return unexpected(truncated(index.error()));
  const auto length = reader.u32_be();
  if (!length) return unexpected(truncated(length.error()));
  const auto offset = reader.u32_be();
  if (!offset) return unexpected(truncated(offset.error()));
  if (length.value() != reader.remaining())
    return unexpected(ParseFailure::at(
        PacketType::of(PacketFamily::generic_data),
        ParseFailureReason::invalid_length, base + generic_data_fields_size,
        length.value(), reader.remaining()));
  const auto payload = reader.take(reader.remaining());
  chunk.key_position = key_position.value();
  chunk.transfer_id = transfer_id.value();
  chunk.packet_index = index.value();
  chunk.transfer_offset = offset.value();
  chunk.payload.assign(payload.value());
  return chunk;
}

auto TypedMessageCodec::encode_generic_data_payload(
    const GenericDataControl& control)
    -> Expected<std::vector<std::byte>, ParseFailure> {
  GenericDataWire payload;
  Wire::assign_generic_data(*payload, control);
  return Wire::serialize_message(payload, PacketFamily::control);
}

auto TypedMessageCodec::decode_generic_data_payload(
    std::span<const std::byte> bytes)
    -> Expected<GenericDataControl, ParseFailure> {
  GenericDataWire payload;
  if (!payload.decode(bytes))
    return unexpected(ParseFailure::of(PacketType::of(PacketFamily::control),
                                       ParseFailureReason::invalid_field));
  GenericDataControl control;
  control.transfer_id = payload->transfer_id;
  control.type = payload->type;
  if (payload->has_data_size) control.data_size = payload->data_size;
  if (payload->metadata != nullptr)
    control.metadata = std::string{GenericDataWire::text_of(payload->metadata)};
  if (payload->has_num_packets) control.packet_count = payload->num_packets;
  if (payload->error != nullptr)
    control.error = std::string{GenericDataWire::text_of(payload->error)};
  control.missing_packets.assign(
      payload->missing_packets,
      payload->missing_packets + payload->missing_packets_count);
  return control;
}

auto TypedMessageCodec::encode_generic_data_direct_message(
    const GenericDataControl& control)
    -> Expected<std::vector<std::byte>, ParseFailure> {
  auto data = encode_generic_data_payload(control);
  if (!data) return unexpected(data.error());
  TakionWire message;
  message->type =
      TakionWire::number(tkproto_TakionMessage_PayloadType_DIRECTMESSAGE);
  message->has_direct_message_payload = true;
  auto& direct = message->direct_message_payload;
  direct.direct_message_type = TakionWire::number(
      tkproto_DirectMessagePayload_DirectMessageType_GENERICDATA);
  direct.destination =
      TakionWire::number(tkproto_DirectMessagePayload_Destination_CLIENT);
  TakionWire::assign(direct.data, std::span<const std::byte>{data.value()});
  return Wire::serialize_message(message, PacketFamily::control);
}

auto TypedMessageCodec::direct_message_data_size(
    std::span<const std::byte> bytes) -> std::size_t {
  TakionWire message;
  if (!message.decode(bytes) ||
      message->type != tkproto_TakionMessage_PayloadType_DIRECTMESSAGE ||
      !message->has_direct_message_payload ||
      message->direct_message_payload.data == nullptr)
    return 0;
  const auto data = TakionWire::bytes_of(message->direct_message_payload.data);
  if (data.size() > bytes.size() ||
      !std::ranges::equal(data, bytes.last(data.size())))
    return 0;
  return data.size();
}

auto TypedMessageCodec::decode_generic_data_direct_message(
    std::span<const std::byte> bytes)
    -> Expected<std::optional<GenericDataControl>, ParseFailure> {
  TakionWire message;
  if (!message.decode(bytes))
    return unexpected(ParseFailure::of(PacketType::of(PacketFamily::control),
                                       ParseFailureReason::invalid_field));
  if (message->type != tkproto_TakionMessage_PayloadType_DIRECTMESSAGE ||
      !message->has_direct_message_payload ||
      message->direct_message_payload.direct_message_type !=
          tkproto_DirectMessagePayload_DirectMessageType_GENERICDATA)
    return std::optional<GenericDataControl>{};
  if (message->direct_message_payload.data == nullptr)
    return unexpected(ParseFailure::of(PacketType::of(PacketFamily::control),
                                       ParseFailureReason::invalid_field));
  auto control = decode_generic_data_payload(
      TakionWire::bytes_of(message->direct_message_payload.data));
  if (!control) return unexpected(control.error());
  return std::optional<GenericDataControl>{std::move(control).value()};
}

}  // namespace libchiaking::net::protocol::takion
