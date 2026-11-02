#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

namespace libchiaking::net::transport::rudp {

Expected<std::vector<Frame>, FrameError> FrameCodec::decode(
    std::span<const std::byte> packet) {
  if (packet.size() <= header_bytes) return unexpected(FrameError::malformed);
  if (packet.size() > max_packet_bytes)
    return unexpected(FrameError::too_large);

  std::vector<Frame> frames;
  std::size_t offset = 0;
  while (packet.size() - offset >= header_bytes) {
    const std::byte* header = packet.data() + offset;
    const std::size_t remaining = packet.size() - offset - header_bytes;
    const std::size_t length =
        bytes::load_be<std::uint16_t>({header, 2}) & length_mask;
    const std::size_t data =
        length > header_bytes ? std::min(length - header_bytes, remaining) : 0;
    Frame frame;
    frame.type = bytes::load_be<std::uint16_t>({header + 6, 2});
    frame.payload.assign(header + header_bytes, header + header_bytes + data);
    frames.push_back(std::move(frame));
    offset += header_bytes + data;
  }
  return frames;
}

bool FrameCodec::is_finish_packet(std::span<const std::byte> packet) {
  auto frames = decode(packet);
  if (!frames.has_value()) return false;
  return first_payload(frames.value(), Subtype::finish).has_value();
}

Expected<std::vector<std::byte>, FrameError> FrameCodec::encode(
    std::span<const Frame> frames) {
  if (frames.empty()) return unexpected(FrameError::malformed);
  if (frames.size() > max_frames)
    return unexpected(FrameError::too_many_frames);

  std::size_t total = 0;
  for (const auto& frame : frames) {
    if (frame.payload.size() > frame_max_payload)
      return unexpected(FrameError::too_large);
    total += header_bytes + frame.payload.size();
    if (total > max_packet_bytes) return unexpected(FrameError::too_large);
  }

  std::vector<std::byte> packet;
  packet.reserve(total);
  for (const auto& frame : frames) {
    const std::uint16_t length =
        static_cast<std::uint16_t>(header_bytes + frame.payload.size());
    bytes::append_be<std::uint16_t>(
        packet, static_cast<std::uint16_t>(magic_nibble | length));
    bytes::append_be<std::uint32_t>(packet, magic);
    bytes::append_be<std::uint16_t>(packet, frame.type);
    packet.insert(packet.end(), frame.payload.begin(), frame.payload.end());
  }
  return packet;
}

Expected<CtrlHttpResponse, FrameError> FrameCodec::decode_ctrl_http_response(
    std::span<const std::byte> payload) {
  if (payload.size() > frame_max_payload)
    return unexpected(FrameError::too_large);
  if (payload.size() < 3) return unexpected(FrameError::malformed);

  const auto message = payload.subspan(counter_bytes);
  const auto terminator = std::ranges::search(message, header_terminator);
  if (terminator.empty()) return unexpected(FrameError::malformed);
  CtrlHttpResponse out{};
  out.remote_counter = static_cast<std::uint16_t>(
      bytes::load_be<std::uint16_t>({payload.data(), 2}) + 1U);
  out.received.assign(message.begin(), message.end());
  out.header_size =
      static_cast<std::size_t>(terminator.end() - message.begin());
  return out;
}

Expected<CtrlHttpResponse, FrameError>
FrameCodec::decode_ctrl_http_response_packet(
    std::span<const std::byte> packet) {
  auto frames = decode(packet);
  if (!frames) return unexpected(frames.error());
  const auto ctrl = first_payload(frames.value(), carries_ctrl_payload);
  if (!ctrl) return unexpected(FrameError::malformed);
  return decode_ctrl_http_response(*ctrl);
}

Expected<std::vector<std::byte>, FrameError> FrameCodec::encode_session_message(
    std::uint16_t local_counter, std::uint16_t remote_counter,
    std::span<const std::byte> request) {
  if (request.size() > session_message_max_request)
    return unexpected(FrameError::too_large);
  std::vector<std::byte> outer_payload;
  outer_payload.reserve(4);
  bytes::append_be<std::uint16_t>(outer_payload, local_counter);
  bytes::append_be<std::uint16_t>(outer_payload, remote_counter);
  std::vector<std::byte> inner_payload;
  inner_payload.reserve(2 + request.size());
  bytes::append_be<std::uint16_t>(inner_payload, local_counter);
  inner_payload.insert(inner_payload.end(), request.begin(), request.end());
  const Frame frames[2] = {
      Frame{type_value(FrameType::session_message), std::move(outer_payload)},
      Frame{type_value(FrameType::ctrl_message), std::move(inner_payload)},
  };
  return encode(frames);
}

Expected<std::vector<std::byte>, FrameError> FrameCodec::encode_ack_message(
    std::uint16_t local_counter, std::uint16_t remote_counter) {
  std::vector<std::byte> payload;
  payload.reserve(6);
  bytes::append_be<std::uint16_t>(payload, local_counter);
  bytes::append_be<std::uint16_t>(payload, remote_counter);
  payload.push_back(std::byte{0x00});
  payload.push_back(std::byte{0x92});
  const Frame frame{type_value(FrameType::ack), std::move(payload)};
  return encode({&frame, 1});
}

std::vector<std::byte> FrameCodec::make_handshake_payload(
    std::uint16_t counter, std::uint32_t header) {
  std::vector<std::byte> payload;
  payload.reserve(handshake_payload_bytes);
  bytes::append_be<std::uint16_t>(payload, counter);
  const std::byte fixed_bytes[6] = {std::byte{0x0B}, std::byte{0x01},
                                    std::byte{0x01}, std::byte{0x00},
                                    std::byte{0x01}, std::byte{0x00}};
  payload.insert(payload.end(), std::begin(fixed_bytes), std::end(fixed_bytes));
  bytes::append_be<std::uint32_t>(payload, header);
  payload.push_back(std::byte{0x05});
  payload.push_back(std::byte{0x82});
  return payload;
}

Expected<std::vector<std::byte>, FrameError> FrameCodec::encode_init_request(
    std::uint16_t counter, std::uint32_t header) {
  const Frame frame{type_value(FrameType::init_request),
                    make_handshake_payload(counter, header)};
  return encode({&frame, 1});
}

Expected<std::vector<std::byte>, FrameError> FrameCodec::encode_cookie_request(
    std::uint16_t counter, std::uint32_t header,
    std::span<const std::byte> cookie) {
  if (cookie.size() > cookie_max_bytes)
    return unexpected(FrameError::too_large);

  auto payload = make_handshake_payload(counter, header);
  payload.reserve(payload.size() + cookie.size());
  payload.insert(payload.end(), cookie.begin(), cookie.end());

  const Frame frame{type_value(FrameType::cookie_request), std::move(payload)};
  return encode({&frame, 1});
}

Expected<CookieResponse, FrameError> FrameCodec::decode_cookie_response(
    std::span<const std::byte> packet) {
  auto frames = decode(packet);
  if (!frames.has_value()) return unexpected(frames.error());
  const auto payload = first_payload(frames.value(), Subtype::cookie_response);
  if (!payload || payload->size() < counter_bytes)
    return unexpected(FrameError::malformed);

  CookieResponse out;
  out.remote_counter = static_cast<std::uint16_t>(
      bytes::load_be<std::uint16_t>(payload->first(counter_bytes)) + 1U);
  return out;
}

Expected<InitResponse, FrameError> FrameCodec::decode_init_response(
    std::span<const std::byte> packet) {
  auto frames = decode(packet);
  if (!frames.has_value()) return unexpected(frames.error());
  const auto payload = first_payload(frames.value(), Subtype::init_response);
  if (!payload || payload->size() < init_response_cookie_offset)
    return unexpected(FrameError::malformed);

  InitResponse out;
  out.remote_counter = static_cast<std::uint16_t>(
      bytes::load_be<std::uint16_t>(payload->first(counter_bytes)) + 1U);
  const auto cookie = payload->subspan(init_response_cookie_offset);
  out.cookie.assign(cookie.begin(), cookie.end());
  return out;
}

Expected<std::vector<std::byte>, FrameError>
FrameCodec::encode_switch_to_stream_connection_request(
    std::uint16_t counter, std::span<const std::byte, rudp_nonce_size> nonce) {
  std::vector<std::byte> payload;
  payload.reserve(26);
  bytes::append_be<std::uint16_t>(payload, counter);
  const std::byte fixed_bytes[8] = {
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x10},
      std::byte{0x00}, std::byte{0x0D}, std::byte{0x00}, std::byte{0x00}};
  payload.insert(payload.end(), std::begin(fixed_bytes), std::end(fixed_bytes));
  payload.insert(payload.end(), nonce.begin(), nonce.end());

  const Frame frame{type_value(FrameType::ctrl_message), std::move(payload)};
  return encode({&frame, 1});
}

Expected<std::vector<std::byte>, SwitchPacketError>
FrameCodec::encode_switch_request_random(
    std::uint16_t counter, platform::SecureRandomProvider& random) {
  std::array<std::byte, rudp_nonce_size> nonce{};
  if (!random.fill_bytes(nonce).has_value())
    return unexpected(SwitchPacketError::random_failed);

  auto packet = encode_switch_to_stream_connection_request(counter, nonce);
  if (!packet.has_value()) return unexpected(SwitchPacketError::encode_failed);
  return std::move(packet).value();
}

auto FrameCodec::first_payload(std::span<const Frame> frames, Subtype subtype)
    -> std::optional<std::span<const std::byte>> {
  return first_payload(
      frames, [subtype](Subtype candidate) { return candidate == subtype; });
}

}  // namespace libchiaking::net::transport::rudp
