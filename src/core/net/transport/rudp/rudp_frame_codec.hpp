/// @file
/// @brief Declares FrameCodec, the encoder and decoder of RUDP packets, and the
/// frame types and replies it reads.
///
/// An RUDP packet is one or more frames back to back. Each frame is an 8-byte
/// header followed by its payload, every field big-endian:
///
/// | Offset | Size | Field |
/// | ------ | ---- | ----- |
/// | 0 | 2 | 0xC000 OR the frame length including this header (low 12 bits) |
/// | 2 | 4 | magic 0x244F244F |
/// | 6 | 2 | frame type; the high byte is the Subtype, the low byte 0x30 |
/// | 8 | n | payload, at most 4087 bytes |
///
/// Most payloads start with a 16-bit packet counter; the counter of an answer
/// is
/// the sender's counter, and the next counter expected from it is one more.
#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/transport/rudp/limits.hpp"

namespace libchiaking::net::transport::rudp {

/// @brief The full 16-bit types of the frames this library sends.
enum class FrameType : std::uint16_t {
  /// @brief A ctrl message: counter then a ctrl HTTP request or a ctrl
  /// command such as the switch-to-stream request.
  ctrl_message = 0x0230,
  /// @brief The leading frame of a session message: local counter then the
  /// remote counter being acknowledged.
  session_message = 0x2030,
  /// @brief A bare acknowledgement: local counter, remote counter, 0x00 0x92.
  ack = 0x2430,
  /// @brief The first handshake request, carrying the 14-byte handshake
  /// payload.
  init_request = 0x8030,
  /// @brief The second handshake request: the 14-byte handshake payload then
  /// the cookie from the init response.
  cookie_request = 0x9030,
};

/// @brief The high byte of a frame type, naming what the frame carries. Its low
/// nibble is the kind; subtypes of the same kind carry the same payload.
enum class Subtype : std::uint8_t {
  /// @brief A ctrl message, which carries a ctrl payload.
  ctrl_message = 0x02,
  /// @brief An acknowledgement of kind 2, the ctrl message kind, so it also
  /// carries a ctrl payload; the acknowledged counter is at payload offset 2.
  ack_ctrl_offset8 = 0x12,
  /// @brief A bare acknowledgement; the acknowledged counter is at payload
  /// offset 2.
  ack = 0x24,
  /// @brief An acknowledgement of kind 6, which also carries a ctrl payload;
  /// the acknowledged counter is at payload offset 2.
  ack_ctrl_offset6 = 0x26,
  /// @brief An acknowledgement of the same kind as ack_ctrl_offset6, also
  /// carrying a ctrl payload; the acknowledged counter is at payload offset
  /// 2.
  ack_ctrl_offset2 = 0x36,
  /// @brief The console's answer to a cookie request: its counter.
  cookie_response = 0xa0,
  /// @brief The console closing an exchange.
  finish = 0xc0,
  /// @brief The console's answer to an init request: its counter, then from
  /// payload offset 8 the cookie to send back.
  init_response = 0xd0,
};
/// @brief How long each handshake request, and each finish acknowledgement,
/// waits for its answer, in microseconds: 1 second.
inline constexpr std::uint64_t rudp_expect_timeout_us = 1'000'000;
/// @brief Default time a ctrl HTTP request over RUDP waits for its reply, in
/// microseconds: 1.5 seconds.
inline constexpr std::uint64_t rudp_ctrl_reply_timeout_us = 1'500'000;

/// @brief Why a packet could not be encoded or decoded.
enum class FrameError {
  /// @brief The packet is no longer than one header, has no frames, lacks the
  /// expected frame, or its payload is too short.
  malformed,
  /// @brief A payload exceeds 4087 bytes, a request or cookie its own limit,
  /// or the packet 65536 bytes.
  too_large,
  /// @brief More than 16 frames were given to encode.
  too_many_frames,
  /// @brief A frame length is inconsistent; not produced by the current
  /// codec.
  invalid_length,
};

/// @brief One frame of a packet.
struct Frame {
  /// @brief The full 16-bit frame type; see FrameType and Subtype.
  std::uint16_t type;
  /// @brief The bytes after the 8-byte header.
  std::vector<std::byte> payload;
};

/// @brief What the console's init response gives the handshake.
struct InitResponse {
  /// @brief The next counter expected from the console: its counter plus one.
  std::uint16_t remote_counter;
  /// @brief The cookie to echo in the cookie request; may be empty.
  std::vector<std::byte> cookie;
};

/// @brief What the console's cookie response gives the handshake.
struct CookieResponse {
  /// @brief The next counter expected from the console: its counter plus one.
  std::uint16_t remote_counter;
};

/// @brief A ctrl HTTP response carried in a ctrl payload.
struct CtrlHttpResponse {
  /// @brief The next counter expected from the console: its counter plus one.
  std::uint16_t remote_counter;
  /// @brief The HTTP response after the counter: headers, the blank line and
  /// whatever body followed in this frame.
  std::vector<std::byte> received;
  /// @brief Bytes of `received` up to and including the CRLF CRLF that ends
  /// the headers.
  std::size_t header_size{};
};

/// @brief Why a switch-to-stream request could not be built.
enum class SwitchPacketError {
  /// @brief The nonce could not be drawn.
  random_failed,
  /// @brief The packet could not be encoded.
  encode_failed
};

/// @brief Encodes and decodes RUDP packets and the handshake, ctrl, ack and
/// switch messages built from them.
///
/// Stateless: every member is static and safe to call from any thread.
/// Decoding is lenient: the magic is not checked, a frame shorter than its
/// header carries no payload, a payload cut off by the end of the packet is
/// kept as far as it goes, and bytes after the last whole header are ignored.
class FrameCodec {
 public:
  /// @brief Bytes in a frame header.
  static constexpr std::size_t header_bytes = 8;
  /// @brief Largest packet encoded or decoded, in bytes.
  static constexpr std::size_t max_packet_bytes = 65536;
  /// @brief Most frames encoded into one packet.
  static constexpr std::size_t max_frames = 16;
  /// @brief Bits of the first header field holding the frame length.
  static constexpr std::uint16_t length_mask = 0x0FFF;
  /// @brief Bits set above the length in the first header field.
  static constexpr std::uint16_t magic_nibble = 0xC000;
  /// @brief The constant at header offset 2.
  static constexpr std::uint32_t magic = 0x244F244FU;
  /// @brief Largest payload of one frame, in bytes: what fits the 12-bit
  /// length
  /// with the header.
  static constexpr std::size_t frame_max_payload = 4087;
  /// @brief Bytes of the handshake payload: counter (2), 0B 01 01 00 01 00,
  /// header value (4), 05 82.
  static constexpr std::size_t handshake_payload_bytes = 14;
  /// @brief Largest request a session message carries: a payload less its
  /// counter.
  static constexpr std::size_t session_message_max_request =
      frame_max_payload - 2U;
  /// @brief Largest cookie a cookie request carries.
  static constexpr std::size_t cookie_max_bytes =
      frame_max_payload - handshake_payload_bytes;

  /// @brief The 16-bit value of a frame type.
  ///
  /// @param type The type.
  /// @return Its wire value.
  static constexpr auto type_value(FrameType type) noexcept -> std::uint16_t {
    return static_cast<std::uint16_t>(type);
  }

  /// @brief The subtype of a frame type.
  ///
  /// @param type A full frame type as read.
  /// @return Its high byte; may be a value Subtype does not name.
  static constexpr auto subtype_of(std::uint16_t type) noexcept -> Subtype {
    return static_cast<Subtype>(type >> 8U);
  }

  /// @brief The kind of a subtype, its low nibble.
  ///
  /// @param subtype The subtype.
  /// @return The low four bits.
  static constexpr auto kind_of(Subtype subtype) noexcept -> std::uint8_t {
    return std::to_underlying(subtype) & subtype_kind_mask;
  }

  /// @brief Whether frames of this subtype carry a ctrl payload (counter then
  /// message).
  ///
  /// @param subtype The subtype.
  /// @return True for kinds 2 (ctrl_message, ack_ctrl_offset8) and 6
  ///     (ack_ctrl_offset6, ack_ctrl_offset2).
  static constexpr auto carries_ctrl_payload(Subtype subtype) noexcept -> bool {
    return kind_of(subtype) == kind_of(Subtype::ctrl_message) ||
           kind_of(subtype) == kind_of(Subtype::ack_ctrl_offset6);
  }

  /// @brief Splits a packet into frames.
  ///
  /// @param packet The datagram.
  /// @return The frames in order; at least one.
  /// @retval FrameError::malformed The packet is no longer than one header.
  /// @retval FrameError::too_large The packet exceeds 65536 bytes.
  [[nodiscard]] static Expected<std::vector<Frame>, FrameError> decode(
      std::span<const std::byte> packet);

  /// @brief Whether a packet holds a finish frame.
  ///
  /// @param packet The datagram.
  /// @return True when it decodes and one of its frames is a finish.
  static bool is_finish_packet(std::span<const std::byte> packet);

  /// @brief Joins frames into a packet.
  ///
  /// @param frames The frames, in order.
  /// @return The packet.
  /// @retval FrameError::malformed No frames were given.
  /// @retval FrameError::too_many_frames More than 16 frames.
  /// @retval FrameError::too_large A payload exceeds 4087 bytes or the packet
  ///     65536 bytes.
  [[nodiscard]] static Expected<std::vector<std::byte>, FrameError> encode(
      std::span<const Frame> frames);

  [[nodiscard]] static Expected<std::vector<std::byte>, FrameError>
  /// @brief Builds the first handshake request.
  ///
  /// @param counter This side's packet counter.
  /// @param header This side's random 32-bit header value.
  /// @return One init_request frame with the 14-byte handshake payload.
  encode_init_request(std::uint16_t counter, std::uint32_t header);

  [[nodiscard]] static Expected<InitResponse, FrameError>
  /// @brief Reads the console's init response.
  ///
  /// @param packet The datagram.
  /// @return The console's next counter and cookie.
  /// @retval FrameError The packet does not decode, has no init_response
  ///     frame, or its payload is shorter than 8 bytes.
  decode_init_response(std::span<const std::byte> packet);

  [[nodiscard]] static Expected<CtrlHttpResponse, FrameError>
  /// @brief Reads a ctrl HTTP response from a ctrl payload.
  ///
  /// @param payload The frame payload: counter then the HTTP response.
  /// @return The console's next counter and the response.
  /// @retval FrameError::too_large The payload exceeds 4087 bytes.
  /// @retval FrameError::malformed The payload is shorter than 3 bytes or
  ///     holds
  ///     no CRLF CRLF.
  decode_ctrl_http_response(std::span<const std::byte> payload);

  [[nodiscard]] static Expected<CtrlHttpResponse, FrameError>
  /// @brief Reads a ctrl HTTP response from the first frame of a packet that
  /// carries a ctrl payload.
  ///
  /// @param packet The datagram.
  /// @return The console's next counter and the response.
  /// @retval FrameError The packet does not decode, has no ctrl payload, or
  ///     the
  ///     payload is not an HTTP response.
  decode_ctrl_http_response_packet(std::span<const std::byte> packet);

  [[nodiscard]] static Expected<std::vector<std::byte>, FrameError>
  /// @brief Builds the second handshake request.
  ///
  /// @param counter This side's packet counter.
  /// @param header This side's header value, as sent in the init request.
  /// @param cookie The cookie from the init response.
  /// @return One cookie_request frame: the handshake payload then the cookie.
  /// @retval FrameError::too_large The cookie exceeds 4073 bytes.
  encode_cookie_request(std::uint16_t counter, std::uint32_t header,
                        std::span<const std::byte> cookie);

  [[nodiscard]] static Expected<CookieResponse, FrameError>
  /// @brief Reads the console's cookie response.
  ///
  /// @param packet The datagram.
  /// @return The console's next counter.
  /// @retval FrameError The packet does not decode, has no cookie_response
  ///     frame, or its payload is shorter than 2 bytes.
  decode_cookie_response(std::span<const std::byte> packet);

  [[nodiscard]] static Expected<std::vector<std::byte>, FrameError>
  /// @brief Builds the ctrl command asking the console to move the stream
  /// from
  /// RUDP to a Takion connection.
  ///
  /// The 26-byte ctrl payload is the counter, 00 00 00 10 00 0D 00 00, then
  /// the
  /// nonce.
  ///
  /// @param counter This side's packet counter.
  /// @param nonce The 16 random bytes identifying the request.
  /// @return One ctrl_message frame.
  encode_switch_to_stream_connection_request(
      std::uint16_t counter, std::span<const std::byte, rudp_nonce_size> nonce);

  [[nodiscard]] static Expected<std::vector<std::byte>, FrameError>
  /// @brief Builds a session message carrying a ctrl HTTP request.
  ///
  /// Two frames: session_message with the local and remote counters, then
  /// ctrl_message with the local counter and the request.
  ///
  /// @param local_counter This side's packet counter.
  /// @param remote_counter The console's next expected counter, acknowledged.
  /// @param request The request bytes.
  /// @return The packet.
  /// @retval FrameError::too_large The request exceeds 4085 bytes.
  encode_session_message(std::uint16_t local_counter,
                         std::uint16_t remote_counter,
                         std::span<const std::byte> request);

  [[nodiscard]] static Expected<std::vector<std::byte>, FrameError>
  /// @brief Builds a bare acknowledgement.
  ///
  /// @param local_counter This side's packet counter.
  /// @param remote_counter The console's next expected counter, acknowledged.
  /// @return One ack frame: the two counters then 0x00 0x92.
  encode_ack_message(std::uint16_t local_counter, std::uint16_t remote_counter);

  [[nodiscard]] static Expected<std::vector<std::byte>, SwitchPacketError>
  /// @brief Builds a switch-to-stream request with a fresh random nonce.
  ///
  /// @param counter This side's packet counter.
  /// @param random Draws the nonce.
  /// @return The packet.
  /// @retval SwitchPacketError::random_failed The nonce could not be
  ///     drawn.
  /// @retval SwitchPacketError::encode_failed The packet could not be
  ///     encoded.
  encode_switch_request_random(std::uint16_t counter,
                               platform::SecureRandomProvider& random);

 private:
  /// @brief Mask of a subtype's kind nibble.
  static constexpr std::uint8_t subtype_kind_mask = 0x0F;
  /// @brief Bytes of the counter that starts most payloads.
  static constexpr std::size_t counter_bytes = 2;
  /// @brief Payload offset of the cookie in an init response.
  static constexpr std::size_t init_response_cookie_offset = 8;
  /// @brief CRLF CRLF, the end of HTTP headers.
  static constexpr std::array<std::byte, 4> header_terminator{
      std::byte{0x0D}, std::byte{0x0A}, std::byte{0x0D}, std::byte{0x0A}};

  /// @brief Finds the payload of the first frame whose subtype matches.
  ///
  /// @tparam Matches A predicate on Subtype.
  /// @param frames The decoded frames.
  /// @param matches Chooses the frame.
  /// @return A view of its payload into `frames`; nothing when no frame
  ///     matches.
  template <std::predicate<Subtype> Matches>
  static auto first_payload(std::span<const Frame> frames, Matches matches)
      -> std::optional<std::span<const std::byte>> {
    const auto found =
        std::ranges::find_if(frames, [&matches](const Frame& frame) {
          return matches(subtype_of(frame.type));
        });
    if (found == frames.end()) return std::nullopt;
    return std::span<const std::byte>{found->payload};
  }

  /// @brief Finds the payload of the first frame of one subtype.
  ///
  /// @param frames The decoded frames.
  /// @param subtype The subtype wanted.
  /// @return A view of its payload; nothing when no frame has it.
  static auto first_payload(std::span<const Frame> frames, Subtype subtype)
      -> std::optional<std::span<const std::byte>>;

  static std::vector<std::byte>
  /// @brief Builds the 14-byte payload shared by both handshake requests.
  ///
  /// @param counter This side's packet counter.
  /// @param header This side's header value.
  /// @return Counter, 0B 01 01 00 01 00, header value, 05 82.
  make_handshake_payload(std::uint16_t counter, std::uint32_t header);
};

}  // namespace libchiaking::net::transport::rudp
