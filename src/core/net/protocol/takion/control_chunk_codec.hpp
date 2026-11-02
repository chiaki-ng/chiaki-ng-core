/// @file
/// @brief Declares the codec for Takion control packets (type 0x00): the
/// message header and the single chunk each packet carries.
///
/// After the type byte, a control packet is (all big-endian):
///
/// | Offset | Size | Field |
/// |-------:|-----:|-------|
/// | 0 | 4 | verification tag of the receiver (0 in INIT) |
/// | 4 | 4 | GMAC tag, zero until crypto is installed |
/// | 8 | 4 | low 32 bits of the key position |
/// | 12 | 1 | chunk type (ControlChunk) |
/// | 13 | 1 | chunk flags; for DATA, 0x01 marks the final fragment |
/// | 14 | 2 | chunk length: payload bytes + 4 |
/// | 16 | n | chunk payload |
#pragma once

#include "libchiaking/detail/secret_string.hpp"
#include "src/core/net/protocol/takion/detail/handshake_types.hpp"
#include "src/core/net/protocol/takion/detail/packet_types.hpp"
#include "src/core/net/protocol/takion/limits.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "src/core/net/protocol/takion/result.hpp"
#include "src/core/utils/bytes/byte_io.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief Bytes of the GMAC field in the message header.
inline constexpr std::size_t control_authentication_tag_size = 4;
/// @brief Added to the payload size to form the chunk length field, which
/// counts the chunk's own type, flags and length bytes.
inline constexpr std::uint16_t control_length_bias = 4;
/// @brief Offset of the chunk type byte, counted after the packet type byte.
inline constexpr std::size_t control_chunk_type_field_offset = 12;
/// @brief Offset of the chunk length field, counted after the packet type
/// byte.
inline constexpr std::size_t control_chunk_length_field_offset = 14;
/// @brief Bytes of an INIT payload.
inline constexpr std::size_t init_chunk_payload_size = 16;
/// @brief Bytes of a DATA_ACK payload before its gap blocks and duplicates:
/// cumulative sequence and window (u32 each), gap count and duplicate count
/// (u16 each).
inline constexpr std::size_t data_acknowledgement_fixed_payload_size = 12;

static_assert(control_chunk_type_offset ==
              packet_type_size + control_chunk_type_field_offset);

/// @brief The 12-byte message header and the chunk flags.
struct ControlHeader {
  /// @brief Tag of the side receiving the packet.
  VerificationTag verification_tag{};
  /// @brief The GMAC tag field as on the wire.
  std::array<std::byte, control_authentication_tag_size> authentication_tag{};
  /// @brief The low 32 bits of the packet's key position.
  std::uint32_t key_position{};
  /// @brief The chunk flags byte.
  std::uint8_t flags{};
};

/// @brief An INIT chunk (0x01).
struct InitChunk {
  /// @brief The header; its tag is 0.
  ControlHeader header{};
  /// @brief The client's values.
  InitValues values{};
};

/// @brief An INIT_ACK chunk (0x02).
struct InitAcknowledgementChunk {
  /// @brief The header.
  ControlHeader header{};
  /// @brief The console's values and cookie.
  InitAcknowledgementValues values{};
};

/// @brief A COOKIE_ECHO chunk (0x0a).
struct CookieChunk {
  /// @brief The header.
  ControlHeader header{};
  /// @brief The cookie echoed.
  HandshakeCookie cookie{};
};

/// @brief A COOKIE_ACK chunk (0x0b), with no payload.
struct CookieAcknowledgementChunk {
  /// @brief The header.
  ControlHeader header{};
};

/// @brief A DATA chunk (0x00) carrying one reliable fragment.
struct DataChunk {
  /// @brief The header; flags 0x01 marks the final fragment.
  ControlHeader header{};
  /// @brief The fragment: its 8-byte body header and data.
  std::vector<std::byte> payload{};
};

/// @brief A DATA_ACK chunk (0x03), a selective acknowledgement.
struct DataAcknowledgementChunk {
  /// @brief The header.
  ControlHeader header{};
  /// @brief The acknowledgement.
  DataAcknowledgement values{};
};

/// @brief Any of the four handshake chunks.
using HandshakeControlChunk =
    std::variant<InitChunk, InitAcknowledgementChunk, CookieChunk,
                 CookieAcknowledgementChunk>;

/// @brief Any chunk a control packet can carry.
using ControlChunkPacket = std::variant<InitChunk, InitAcknowledgementChunk,
                                        CookieChunk, CookieAcknowledgementChunk,
                                        DataChunk, DataAcknowledgementChunk>;

/// @brief Encodes and decodes control packets. Stateless; safe from any
/// thread.
///
/// Only one chunk per packet is understood: a packet whose length field does
/// not account for every byte is refused.
class ControlChunkCodec {
 public:
  /// @brief Encodes a handshake chunk with its message header.
  ///
  /// @param chunk The chunk.
  /// @return The bytes after the packet type byte.
  /// @retval ParseFailureReason::invalid_length The payload does not fit the
  ///     16-bit length.
  [[nodiscard]] static auto encode(const HandshakeControlChunk& chunk)
      -> Expected<std::vector<std::byte>, ParseFailure>;

  /// @brief Decodes a handshake chunk.
  ///
  /// @param bytes The bytes after the packet type byte.
  /// @return The chunk.
  /// @retval ParseFailureReason::truncated The header or payload is short.
  /// @retval ParseFailureReason::invalid_length The length field is below 4,
  ///     does not match the packet, or does not match the chunk type's fixed
  ///     size.
  /// @retval ParseFailureReason::unknown_control_chunk The chunk type is
  ///     unknown.
  /// @retval ParseFailureReason::unsupported_layout The chunk is DATA or
  ///     DATA_ACK.
  [[nodiscard]] static auto decode(std::span<const std::byte> bytes)
      -> Expected<HandshakeControlChunk, ParseFailure>;

  /// @brief Encodes any control chunk with its message header.
  ///
  /// @param chunk The chunk.
  /// @return The bytes after the packet type byte.
  /// @retval ParseFailureReason::invalid_length The payload does not fit.
  /// @retval ParseFailureReason::invalid_field A DATA_ACK has more than 65535
  ///     gap blocks or duplicates.
  [[nodiscard]] static auto encode_packet(const ControlChunkPacket& chunk)
      -> Expected<SecretBytes, ParseFailure>;

  /// @brief Encodes a DATA chunk around a fragment.
  ///
  /// @param header The header and flags.
  /// @param payload The fragment body.
  /// @return The bytes after the packet type byte.
  /// @retval ParseFailureReason::invalid_length The payload does not fit.
  [[nodiscard]] static auto encode_data_packet(
      const ControlHeader& header, std::span<const std::byte> payload)
      -> Expected<SecretBytes, ParseFailure>;

  /// @brief Decodes any control chunk.
  ///
  /// @param bytes The bytes after the packet type byte.
  /// @return The chunk.
  /// @retval ParseFailure As decode(), or a DATA_ACK whose counts do not match
  ///     its length.
  [[nodiscard]] static auto decode_packet(std::span<const std::byte> bytes)
      -> Expected<ControlChunkPacket, ParseFailure>;

 private:
  /// @brief A decoded header before its payload is read.
  struct DecodedHeader {
    /// @brief The header.
    ControlHeader header;
    /// @brief The raw chunk type.
    std::uint8_t type{};
    /// @brief Payload bytes the length field announces.
    std::size_t payload_bytes{};
  };

  /// @brief Tells the fixed payload size of a handshake chunk.
  ///
  /// @param type The chunk type.
  /// @return 16 for INIT, 48 for INIT_ACK, 32 for COOKIE_ECHO, 0 for
  ///     COOKIE_ACK; nothing for DATA and DATA_ACK.
  static constexpr auto handshake_payload_size(ControlChunk type) noexcept
      -> std::optional<std::size_t>;
  /// @brief Names a handshake chunk's type.
  ///
  /// @param chunk The chunk.
  /// @return Its type.
  static auto chunk_type(const HandshakeControlChunk& chunk) noexcept
      -> ControlChunk;
  /// @brief Writes the message header and chunk header.
  ///
  /// @param header The header.
  /// @param type The chunk type.
  /// @param payload_bytes The payload size to announce.
  /// @return The bytes so far.
  /// @retval ParseFailureReason::invalid_length The size does not fit.
  [[nodiscard]] static auto begin_chunk(const ControlHeader& header,
                                        ControlChunk type,
                                        std::size_t payload_bytes)
      -> Expected<SecretBytes, ParseFailure>;
  /// @brief Writes INIT values in wire order.
  ///
  /// @param output Where to append.
  /// @param values The values.
  static void append_init_values(SecretBytes& output, const InitValues& values);
  /// @brief Reads and checks the message and chunk headers.
  ///
  /// @param reader Positioned at the start.
  /// @param total_size The packet's size after the type byte.
  /// @return The header and announced payload size.
  /// @retval ParseFailureReason::truncated The header is short.
  /// @retval ParseFailureReason::invalid_length The length is below 4 or does
  /// not
  ///     match the packet.
  [[nodiscard]] static auto read_header(bytes::Reader& reader,
                                        std::size_t total_size)
      -> Expected<DecodedHeader, ParseFailure>;
  /// @brief Reads INIT values.
  ///
  /// @param reader Positioned at them.
  /// @return The values.
  /// @retval ParseFailureReason::truncated They are short.
  [[nodiscard]] static auto read_init_values(bytes::Reader& reader)
      -> Expected<InitValues, ParseFailure>;
  /// @brief Reads the 32-byte cookie.
  ///
  /// @param reader Positioned at it.
  /// @return The cookie.
  /// @retval ParseFailureReason::truncated It is short.
  [[nodiscard]] static auto read_cookie(bytes::Reader& reader)
      -> Expected<HandshakeCookie, ParseFailure>;
};

}  // namespace libchiaking::net::protocol::takion
