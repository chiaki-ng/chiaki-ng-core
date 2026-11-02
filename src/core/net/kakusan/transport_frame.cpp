#include "src/core/net/kakusan/transport_frame.hpp"

#include "src/core/utils/bytes/byte_io.hpp"
#include "src/core/utils/memory/constant_time.hpp"

#include <algorithm>

namespace libchiaking::net::kakusan {

auto TransportFrameCodec::media_tag(platform::CryptoProvider& crypto,
                                    const MediaKey& key,
                                    std::span<const std::byte> authenticated)
    -> Result<std::array<std::byte, media_tag_size>> {
  const auto mac = Hash::hmac_sha256(crypto, key, authenticated);
  if (!mac) return fail(mac.error());
  std::array<std::byte, media_tag_size> tag{};
  std::copy_n(mac.value().begin(), media_tag_size, tag.begin());
  return tag;
}

auto TransportFrameCodec::control_fragment_count(
    std::size_t message_size) noexcept -> std::size_t {
  return (message_size + control_fragment_size - 1) / control_fragment_size;
}

auto TransportFrameCodec::encode_control_fragments(
    std::uint32_t message_id, std::span<const std::byte> message)
    -> Result<std::vector<std::vector<std::byte>>> {
  if (message_id == 0 || message.empty() ||
      message.size() > max_control_message_size)
    return fail(Error::protocol);
  const auto count = control_fragment_count(message.size());
  std::vector<std::vector<std::byte>> fragments;
  fragments.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto offset = index * control_fragment_size;
    const auto part = message.subspan(
        offset, std::min(control_fragment_size, message.size() - offset));
    std::vector<std::byte> record;
    record.reserve(control_header_size + part.size());
    bytes::Appender writer{record};
    writer.text(control_prefix);
    writer.u8(static_cast<std::uint8_t>(ControlKind::data));
    writer.u8(0);
    writer.u16_be(static_cast<std::uint16_t>(index));
    writer.u32_be(message_id);
    writer.u16_be(static_cast<std::uint16_t>(count));
    writer.u16_be(static_cast<std::uint16_t>(part.size()));
    writer.bytes(part);
    fragments.push_back(std::move(record));
  }
  return fragments;
}

auto TransportFrameCodec::encode_control_ack(std::uint32_t message_id)
    -> std::array<std::byte, control_header_size> {
  std::array<std::byte, control_header_size> output{};
  bytes::Writer writer{output};
  static_cast<void>(writer.text(control_prefix) &&
                    writer.u8(static_cast<std::uint8_t>(ControlKind::ack)) &&
                    writer.zeros(3) && writer.u32_be(message_id) &&
                    writer.zeros(4));
  return output;
}

auto TransportFrameCodec::decode_control(std::span<const std::byte> record)
    -> Result<ControlFragment> {
  if (record.size() < control_header_size) return fail(Error::protocol);
  bytes::Reader reader{record};
  if (!reader.consume(control_prefix)) return fail(Error::protocol);
  const auto kind = reader.u8().value();
  const auto zero = reader.u8().value();
  ControlFragment fragment;
  fragment.index = reader.u16_be().value();
  fragment.message_id = reader.u32_be().value();
  fragment.count = reader.u16_be().value();
  const auto size = reader.u16_be().value();
  if (zero != 0 || fragment.message_id == 0 || reader.remaining() != size)
    return fail(Error::protocol);
  if (kind == static_cast<std::uint8_t>(ControlKind::ack)) {
    if (fragment.index != 0 || fragment.count != 0 || size != 0)
      return fail(Error::protocol);
    fragment.kind = ControlKind::ack;
    return fragment;
  }
  if (kind != static_cast<std::uint8_t>(ControlKind::data) ||
      fragment.count < 1 || fragment.count > max_control_fragments ||
      fragment.index >= fragment.count || size < 1 ||
      size > control_fragment_size)
    return fail(Error::protocol);
  const bool last = fragment.index + 1U == fragment.count;
  if (!last && size != control_fragment_size) return fail(Error::protocol);
  if (last && fragment.count == max_control_fragments &&
      size > final_fragment_limit)
    return fail(Error::protocol);
  fragment.kind = ControlKind::data;
  fragment.payload = reader.take(size).value();
  return fragment;
}

auto TransportFrameCodec::connection_id(platform::CryptoProvider& crypto,
                                        const Digest256& transcript)
    -> Result<ConnectionId> {
  std::vector<std::byte> input;
  input.reserve(connection_label.size() + 1 + transcript.size());
  bytes::Appender writer{input};
  writer.text(connection_label);
  writer.u8(0);
  writer.bytes(transcript);
  const auto digest = Hash::sha256(crypto, input);
  if (!digest) return fail(digest.error());
  ConnectionId id{};
  std::copy_n(digest.value().begin(), id.size(), id.begin());
  return id;
}

auto TransportFrameCodec::media_fragment_count(
    std::size_t original_size) noexcept -> std::size_t {
  return (original_size + media_fragment_size - 1) / media_fragment_size;
}

auto TransportFrameCodec::encode_media(platform::CryptoProvider& crypto,
                                       const MediaKey& key,
                                       const ConnectionId& connection,
                                       const MediaHeader& header,
                                       std::span<const std::byte> fragment)
    -> Result<std::vector<std::byte>> {
  if (!valid_media_shape(header, fragment.size())) return fail(Error::protocol);
  std::vector<std::byte> datagram;
  datagram.reserve(media_header_size + fragment.size() + media_tag_size);
  bytes::Appender writer{datagram};
  writer.text(media_prefix);
  writer.u8(media_version);
  writer.u8(static_cast<std::uint8_t>(header.kind));
  writer.u16_be(0);
  writer.bytes(connection);
  writer.u64_be(header.datagram_sequence);
  writer.u64_be(header.packet_sequence);
  writer.u32_be(media_epoch);
  writer.u64_be(header.key_position);
  writer.u16_be(header.original_size);
  writer.u16_be(header.fragment_offset);
  writer.u16_be(header.fragment_size);
  writer.u16_be(0);
  writer.bytes(fragment);
  const auto tag = media_tag(crypto, key, datagram);
  if (!tag) return fail(tag.error());
  writer.bytes(tag.value());
  return datagram;
}

auto TransportFrameCodec::media_matches(std::span<const std::byte> datagram,
                                        const ConnectionId& connection) noexcept
    -> bool {
  if (datagram.size() < media_header_size + media_tag_size ||
      datagram.size() > max_datagram_size)
    return false;
  bytes::Reader reader{datagram};
  return reader.consume(media_prefix) && reader.u8().value() == media_version &&
         std::ranges::equal(connection, datagram.subspan(connection_offset,
                                                         connection.size()));
}

auto TransportFrameCodec::authenticate_media(
    platform::CryptoProvider& crypto, const MediaKey& key,
    std::span<const std::byte> datagram) -> Result<bool> {
  if (datagram.size() < media_header_size + media_tag_size ||
      datagram.size() > max_datagram_size)
    return false;
  const auto body = datagram.first(datagram.size() - media_tag_size);
  const auto tag = media_tag(crypto, key, body);
  if (!tag) return fail(tag.error());
  return memory::constant_time_equal(tag.value(),
                                     datagram.subspan(body.size()));
}

auto TransportFrameCodec::decode_media(std::span<const std::byte> datagram)
    -> Result<MediaDatagram> {
  if (datagram.size() < media_header_size + media_tag_size ||
      datagram.size() > max_datagram_size)
    return fail(Error::protocol);
  bytes::Reader reader{datagram};
  if (!reader.consume(media_prefix)) return fail(Error::protocol);
  const auto version = reader.u8().value();
  const auto kind = reader.u8().value();
  const auto reserved = reader.u16_be().value();
  reader.skip(sizeof(ConnectionId)).value();
  MediaDatagram media;
  media.header.datagram_sequence = reader.u64_be().value();
  media.header.packet_sequence = reader.u64_be().value();
  const auto epoch = reader.u32_be().value();
  media.header.key_position = reader.u64_be().value();
  media.header.original_size = reader.u16_be().value();
  media.header.fragment_offset = reader.u16_be().value();
  media.header.fragment_size = reader.u16_be().value();
  const auto zero = reader.u16_be().value();
  if (version != media_version ||
      kind > static_cast<std::uint8_t>(MediaKind::probe) || reserved != 0 ||
      zero != 0 || epoch != media_epoch)
    return fail(Error::protocol);
  media.header.kind = static_cast<MediaKind>(kind);
  const auto fragment_size =
      datagram.size() - media_header_size - media_tag_size;
  if (!valid_media_shape(media.header, fragment_size))
    return fail(Error::protocol);
  media.fragment = reader.take(fragment_size).value();
  return media;
}

auto MediaAck::well_formed() const noexcept -> bool {
  if (ranges.empty() || ranges.size() > max_ack_ranges ||
      ranges.front().high != largest)
    return false;
  if (std::ranges::any_of(
          ranges, [](const AckRange& range) { return range.low > range.high; }))
    return false;
  return std::ranges::adjacent_find(
             ranges, [](const AckRange& newer, const AckRange& older) {
               return newer.low == 0 || older.high >= newer.low - 1;
             }) == ranges.end();
}

auto TransportFrameCodec::encode_media_ack(const MediaAck& ack)
    -> Result<std::vector<std::byte>> {
  if (!ack.well_formed()) return fail(Error::protocol);
  std::vector<std::byte> record;
  record.reserve(media_ack_header_size +
                 media_ack_range_size * ack.ranges.size());
  bytes::Appender writer{record};
  writer.text(media_ack_prefix);
  writer.u64_be(ack.largest);
  writer.u32_be(ack.delay_us);
  writer.u8(static_cast<std::uint8_t>(ack.ranges.size()));
  writer.zeros(media_ack_reserved_size);
  for (const auto& range : ack.ranges) {
    writer.u64_be(range.low);
    writer.u64_be(range.high);
  }
  return record;
}

auto TransportFrameCodec::decode_media_ack(std::span<const std::byte> record)
    -> Result<MediaAck> {
  if (record.size() < media_ack_header_size) return fail(Error::protocol);
  bytes::Reader reader{record};
  if (!reader.consume(media_ack_prefix)) return fail(Error::protocol);
  MediaAck ack;
  ack.largest = reader.u64_be().value();
  ack.delay_us = reader.u32_be().value();
  const auto count = reader.u8().value();
  const auto reserved = reader.take(media_ack_reserved_size).value();
  if (std::ranges::any_of(reserved,
                          [](std::byte byte) { return byte != std::byte{}; }) ||
      count < 1 || count > max_ack_ranges ||
      reader.remaining() != media_ack_range_size * count)
    return fail(Error::protocol);
  ack.ranges.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto low = reader.u64_be().value();
    const auto high = reader.u64_be().value();
    ack.ranges.push_back(AckRange{low, high});
  }
  if (!ack.well_formed()) return fail(Error::protocol);
  return ack;
}

}  // namespace libchiaking::net::kakusan
