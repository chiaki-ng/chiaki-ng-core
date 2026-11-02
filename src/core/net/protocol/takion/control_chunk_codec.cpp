#include "src/core/net/protocol/takion/control_chunk_codec.hpp"

#include "src/core/net/protocol/takion/result.hpp"
#include "src/core/utils/bytes/byte_io.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace libchiaking::net::protocol::takion {

constexpr auto ControlChunkCodec::handshake_payload_size(
    ControlChunk type) noexcept -> std::optional<std::size_t> {
  switch (type) {
    case ControlChunk::init:
      return init_chunk_payload_size;
    case ControlChunk::init_acknowledgement:
      return init_chunk_payload_size + handshake_cookie_size;
    case ControlChunk::cookie:
      return handshake_cookie_size;
    case ControlChunk::cookie_acknowledgement:
      return 0;
    case ControlChunk::data:
    case ControlChunk::data_acknowledgement:
      return std::nullopt;
  }
  return std::nullopt;
}

auto ControlChunkCodec::chunk_type(const HandshakeControlChunk& chunk) noexcept
    -> ControlChunk {
  return std::visit(
      [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, InitChunk>)
          return ControlChunk::init;
        else if constexpr (std::is_same_v<T, InitAcknowledgementChunk>)
          return ControlChunk::init_acknowledgement;
        else if constexpr (std::is_same_v<T, CookieChunk>)
          return ControlChunk::cookie;
        else
          return ControlChunk::cookie_acknowledgement;
      },
      chunk);
}

auto ControlChunkCodec::begin_chunk(const ControlHeader& header,
                                    ControlChunk type,
                                    std::size_t payload_bytes)
    -> Expected<SecretBytes, ParseFailure> {
  if (payload_bytes >
      std::numeric_limits<std::uint16_t>::max() - control_length_bias)
    return unexpected(
        ParseFailure::at(PacketType{}, ParseFailureReason::invalid_length,
                         control_chunk_length_field_offset, payload_bytes, 0));
  SecretBytes bytes;
  bytes.reserve(control_header_size + payload_bytes);
  bytes::append_be(bytes, header.verification_tag.value);
  bytes.append(header.authentication_tag);
  bytes::append_be(bytes, header.key_position);
  bytes.push_back(static_cast<std::byte>(type));
  bytes.push_back(std::byte{header.flags});
  bytes::append_be(
      bytes, static_cast<std::uint16_t>(payload_bytes + control_length_bias));
  return bytes;
}

void ControlChunkCodec::append_init_values(SecretBytes& output,
                                           const InitValues& values) {
  bytes::append_be(output, values.tag.value);
  bytes::append_be(output, values.advertised_receive_window.value);
  bytes::append_be(output, values.outbound_streams.value);
  bytes::append_be(output, values.inbound_streams.value);
  bytes::append_be(output, values.initial_sequence_number.value);
}

auto ControlChunkCodec::encode(const HandshakeControlChunk& chunk)
    -> Expected<std::vector<std::byte>, ParseFailure> {
  const auto type = chunk_type(chunk);
  const auto& header = std::visit(
      [](const auto& value) -> const ControlHeader& { return value.header; },
      chunk);
  auto bytes =
      begin_chunk(header, type, handshake_payload_size(type).value_or(0));
  if (!bytes) return unexpected(bytes.error());
  std::visit(
      [&output = bytes.value()](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, InitChunk>) {
          append_init_values(output, value.values);
        } else if constexpr (std::is_same_v<T, InitAcknowledgementChunk>) {
          append_init_values(output, value.values.init);
          output.append(value.values.cookie);
        } else if constexpr (std::is_same_v<T, CookieChunk>) {
          output.append(value.cookie);
        }
      },
      chunk);
  return std::vector<std::byte>{bytes.value().begin(), bytes.value().end()};
}

auto ControlChunkCodec::read_header(bytes::Reader& reader,
                                    std::size_t total_size)
    -> Expected<DecodedHeader, ParseFailure> {
  const auto truncated = [](const bytes::Shortfall& shortfall) {
    return unexpected(ParseFailure::short_by(PacketType{}, shortfall));
  };
  const auto tag = reader.u32_be();
  if (!tag) return truncated(tag.error());
  const auto authentication = reader.take(control_authentication_tag_size);
  if (!authentication) return truncated(authentication.error());
  const auto key_position = reader.u32_be();
  if (!key_position) return truncated(key_position.error());
  const auto type = reader.u8();
  if (!type) return truncated(type.error());
  const auto flags = reader.u8();
  if (!flags) return truncated(flags.error());
  const auto wire_length = reader.u16_be();
  if (!wire_length) return truncated(wire_length.error());

  if (wire_length.value() < control_length_bias)
    return unexpected(
        ParseFailure::at(PacketType{}, ParseFailureReason::invalid_length,
                         control_chunk_length_field_offset, control_length_bias,
                         wire_length.value()));
  const auto payload_bytes =
      static_cast<std::size_t>(wire_length.value() - control_length_bias);
  const auto expected_total = control_header_size + payload_bytes;
  if (total_size != expected_total)
    return unexpected(ParseFailure::at(
        PacketType{}, ParseFailureReason::invalid_length,
        control_chunk_length_field_offset, expected_total, total_size));

  DecodedHeader decoded{ControlHeader{VerificationTag{tag.value()},
                                      {},
                                      key_position.value(),
                                      flags.value()},
                        type.value(), payload_bytes};
  std::ranges::copy(authentication.value(),
                    decoded.header.authentication_tag.begin());
  return decoded;
}

auto ControlChunkCodec::read_init_values(bytes::Reader& reader)
    -> Expected<InitValues, ParseFailure> {
  const auto truncated = [](const bytes::Shortfall& shortfall) {
    return unexpected(ParseFailure::short_by(PacketType{}, shortfall));
  };
  const auto tag = reader.u32_be();
  if (!tag) return truncated(tag.error());
  const auto window = reader.u32_be();
  if (!window) return truncated(window.error());
  const auto outbound = reader.u16_be();
  if (!outbound) return truncated(outbound.error());
  const auto inbound = reader.u16_be();
  if (!inbound) return truncated(inbound.error());
  const auto sequence = reader.u32_be();
  if (!sequence) return truncated(sequence.error());
  return InitValues{
      VerificationTag{tag.value()},
      AdvertisedReceiveWindow{window.value()},
      StreamCount{outbound.value()},
      StreamCount{inbound.value()},
      InitialSequenceNumber{sequence.value()},
  };
}

auto ControlChunkCodec::read_cookie(bytes::Reader& reader)
    -> Expected<HandshakeCookie, ParseFailure> {
  const auto bytes = reader.take(handshake_cookie_size);
  if (!bytes)
    return unexpected(ParseFailure::short_by(PacketType{}, bytes.error()));
  HandshakeCookie cookie{};
  std::ranges::copy(bytes.value(), cookie.begin());
  return cookie;
}

auto ControlChunkCodec::decode(std::span<const std::byte> bytes)
    -> Expected<HandshakeControlChunk, ParseFailure> {
  bytes::Reader reader{bytes};
  const auto decoded = read_header(reader, bytes.size());
  if (!decoded) return unexpected(decoded.error());
  const auto& [header, raw_type, payload_bytes] = decoded.value();

  const auto known_type = ControlChunkType::from_code(raw_type);
  if (!known_type)
    return unexpected(ParseFailure::of(
        PacketType{}, ParseFailureReason::unknown_control_chunk,
        control_chunk_type_field_offset));
  const auto exact_payload_size = handshake_payload_size(*known_type);
  if (!exact_payload_size)
    return unexpected(ParseFailure::of(PacketType{},
                                       ParseFailureReason::unsupported_layout,
                                       control_chunk_type_field_offset));
  if (payload_bytes != *exact_payload_size)
    return unexpected(ParseFailure::at(
        PacketType{}, ParseFailureReason::invalid_length,
        control_chunk_length_field_offset, *exact_payload_size, payload_bytes));

  switch (*known_type) {
    case ControlChunk::init:
      return read_init_values(reader).transform(
          [&header](const InitValues& values) -> HandshakeControlChunk {
            return InitChunk{header, values};
          });
    case ControlChunk::init_acknowledgement: {
      const auto init = read_init_values(reader);
      if (!init) return unexpected(init.error());
      return read_cookie(reader).transform(
          [&header,
           &init](const HandshakeCookie& cookie) -> HandshakeControlChunk {
            return InitAcknowledgementChunk{header, {init.value(), cookie}};
          });
    }
    case ControlChunk::cookie:
      return read_cookie(reader).transform(
          [&header](const HandshakeCookie& cookie) -> HandshakeControlChunk {
            return CookieChunk{header, cookie};
          });
    default:
      return HandshakeControlChunk{CookieAcknowledgementChunk{header}};
  }
}

auto ControlChunkCodec::encode_data_packet(const ControlHeader& header,
                                           std::span<const std::byte> payload)
    -> Expected<SecretBytes, ParseFailure> {
  auto bytes = begin_chunk(header, ControlChunk::data, payload.size());
  if (bytes) bytes.value().append(payload);
  return bytes;
}

auto ControlChunkCodec::encode_packet(const ControlChunkPacket& chunk)
    -> Expected<SecretBytes, ParseFailure> {
  return std::visit(
      [](const auto& value) -> Expected<SecretBytes, ParseFailure> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, DataChunk>) {
          return encode_data_packet(value.header, value.payload);
        } else if constexpr (!std::is_same_v<T, DataAcknowledgementChunk>) {
          return encode(HandshakeControlChunk{value})
              .transform([](const std::vector<std::byte>& handshake) {
                return SecretBytes{handshake};
              });
        } else {
          const auto& acknowledgement = value.values;
          constexpr auto count_limit =
              std::numeric_limits<std::uint16_t>::max();
          if (acknowledgement.gap_blocks.size() > count_limit ||
              acknowledgement.duplicate_sequences.size() > count_limit)
            return unexpected(ParseFailure::at(
                PacketType{}, ParseFailureReason::invalid_field, 24,
                acknowledgement.gap_blocks.size() +
                    acknowledgement.duplicate_sequences.size(),
                count_limit));
          const std::size_t payload_bytes =
              data_acknowledgement_fixed_payload_size +
              gap_block_size * acknowledgement.gap_blocks.size() +
              sizeof(std::uint32_t) *
                  acknowledgement.duplicate_sequences.size();
          auto bytes = begin_chunk(
              value.header, ControlChunk::data_acknowledgement, payload_bytes);
          if (!bytes) return bytes;
          auto& output = bytes.value();
          bytes::append_be(output, acknowledgement.cumulative_sequence);
          bytes::append_be(output, acknowledgement.advertised_receive_window);
          bytes::append_be(output, static_cast<std::uint16_t>(
                                       acknowledgement.gap_blocks.size()));
          bytes::append_be(output,
                           static_cast<std::uint16_t>(
                               acknowledgement.duplicate_sequences.size()));
          for (const auto& gap : acknowledgement.gap_blocks) {
            bytes::append_be(output, gap.start);
            bytes::append_be(output, gap.end);
          }
          for (const auto sequence : acknowledgement.duplicate_sequences)
            bytes::append_be(output, sequence);
          return bytes;
        }
      },
      chunk);
}

auto ControlChunkCodec::decode_packet(std::span<const std::byte> bytes)
    -> Expected<ControlChunkPacket, ParseFailure> {
  const auto raw_type = bytes.size() > control_chunk_type_field_offset
                            ? std::optional{std::to_integer<std::uint8_t>(
                                  bytes[control_chunk_type_field_offset])}
                            : std::nullopt;
  if (raw_type != std::to_underlying(ControlChunk::data) &&
      raw_type != std::to_underlying(ControlChunk::data_acknowledgement))
    return decode(bytes).transform([](const HandshakeControlChunk& handshake) {
      return std::visit(
          [](const auto& value) -> ControlChunkPacket { return value; },
          handshake);
    });

  bytes::Reader reader{bytes};
  const auto decoded = read_header(reader, bytes.size());
  if (!decoded) return unexpected(decoded.error());
  const auto& [header, type, payload_bytes] = decoded.value();
  const auto truncated = [](const bytes::Shortfall& shortfall) {
    return unexpected(ParseFailure::short_by(PacketType{}, shortfall));
  };

  if (type == std::to_underlying(ControlChunk::data)) {
    const auto payload = reader.take(payload_bytes);
    if (!payload) return truncated(payload.error());
    return ControlChunkPacket{
        DataChunk{header, {payload.value().begin(), payload.value().end()}}};
  }

  if (payload_bytes < data_acknowledgement_fixed_payload_size)
    return unexpected(ParseFailure::at(
        PacketType{}, ParseFailureReason::invalid_length,
        control_chunk_length_field_offset,
        data_acknowledgement_fixed_payload_size, payload_bytes));
  const auto cumulative = reader.u32_be();
  if (!cumulative) return truncated(cumulative.error());
  const auto window = reader.u32_be();
  if (!window) return truncated(window.error());
  const auto gaps = reader.u16_be();
  if (!gaps) return truncated(gaps.error());
  const auto duplicates = reader.u16_be();
  if (!duplicates) return truncated(duplicates.error());
  const auto expected_payload_bytes =
      data_acknowledgement_fixed_payload_size + gap_block_size * gaps.value() +
      sizeof(std::uint32_t) * duplicates.value();
  if (payload_bytes != expected_payload_bytes)
    return unexpected(ParseFailure::at(PacketType{},
                                       ParseFailureReason::invalid_length,
                                       control_chunk_length_field_offset,
                                       expected_payload_bytes, payload_bytes));

  DataAcknowledgement acknowledgement;
  acknowledgement.cumulative_sequence = cumulative.value();
  acknowledgement.advertised_receive_window = window.value();
  acknowledgement.gap_blocks.reserve(gaps.value());
  for (std::size_t index = 0; index < gaps.value(); ++index) {
    const auto start = reader.u16_be();
    if (!start) return truncated(start.error());
    const auto end = reader.u16_be();
    if (!end) return truncated(end.error());
    acknowledgement.gap_blocks.push_back({start.value(), end.value()});
  }
  acknowledgement.duplicate_sequences.reserve(duplicates.value());
  for (std::size_t index = 0; index < duplicates.value(); ++index) {
    const auto sequence = reader.u32_be();
    if (!sequence) return truncated(sequence.error());
    acknowledgement.duplicate_sequences.push_back(sequence.value());
  }
  return ControlChunkPacket{
      DataAcknowledgementChunk{header, std::move(acknowledgement)}};
}

}  // namespace libchiaking::net::protocol::takion
