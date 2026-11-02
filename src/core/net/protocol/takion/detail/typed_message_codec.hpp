/// @file
/// @brief Declares TypedMessageCodec, the codec for Takion's smaller packet
/// families and protobuf messages: length-prefixed packets, CLIENTINFO,
/// GKTRACE, and the chunks and control messages of generic-data transfers.
///
/// Generic data (version 15 on, with the direct_message_encryption and
/// generic_control_data features) moves an opaque blob with metadata in
/// 519-byte chunks of packet type 0x0c, steered by GENERICDATA direct messages
/// on the reliable control channel. A chunk packet's body after the type byte
/// and any extended header (big-endian):
///
/// | Offset | Size | Field |
/// |-------:|-----:|-------|
/// | 0 | 4 | GMAC tag |
/// | 4 | 4 | key position |
/// | 8 | 4 | transfer id |
/// | 12 | 1 | packet index |
/// | 13 | 4 | payload length |
/// | 17 | 4 | offset of the payload in the transfer |
/// | 21 | n | payload, encrypted |
#pragma once

#include "libchiaking/detail/secret_string.hpp"
#include "src/core/net/protocol/takion/detail/crypto_context.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_types.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/result.hpp"
#include "src/core/utils/bytes/byte_io.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief Largest length-prefixed payload, in bytes.
inline constexpr std::size_t length_prefixed_payload_limit = 0xffff;
/// @brief Key advance of a GKTRACE packet.
inline constexpr std::size_t trace_key_advance = 16;
/// @brief Bytes of each generic-data chunk but the last.
inline constexpr std::size_t generic_data_chunk_size = 519;
/// @brief A generic-data transfer must be smaller than this, in bytes.
inline constexpr std::size_t generic_data_data_size_limit = 51200;
/// @brief Generic-data metadata must be shorter than this, in bytes.
inline constexpr std::size_t generic_data_metadata_size_limit = 2048;
/// @brief Packet indices one missing-packets answer lists at most.
inline constexpr std::size_t generic_data_missing_response_limit = 25;
/// @brief Most chunks an incoming transfer may announce.
inline constexpr std::size_t generic_data_packet_count_limit = 256;

/// @brief The fields of a CLIENTINFO message (packet type 0x08).
struct ClientInfoValues {
  /// @brief The session key; shared so it is never copied into a plain string.
  std::shared_ptr<const SecretString> session_key{};
  /// @brief The GCM tag field, when present.
  std::optional<std::uint32_t> tag{};
  /// @brief The key position field, when present.
  std::optional<std::uint32_t> key_position{};
};

/// @brief The fields of a GKTRACE message (packet type 0x0a).
struct TraceValues {
  /// @brief The trace sequence number.
  std::uint32_t sequence{};
  /// @brief The detached GMAC tag, as a little-endian u32 of its 4 bytes; 0
  /// while the tag is computed.
  std::uint32_t tag{};
  /// @brief Low 32 bits of the key position the tag was computed at.
  std::uint32_t key_position{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const TraceValues&) const noexcept = default;
};

/// @brief One generic-data chunk packet.
struct GenericDataChunk {
  /// @brief The GMAC tag field.
  Mac tag{};
  /// @brief The key position field.
  std::uint32_t key_position{};
  /// @brief The transfer it belongs to.
  std::uint32_t transfer_id{};
  /// @brief Its index in the transfer, from 0.
  std::uint8_t packet_index{};
  /// @brief Byte offset of its payload in the transfer.
  std::uint32_t transfer_offset{};
  /// @brief Its payload.
  SecretBytes payload;

  /// @brief Compares every field and the payload bytes.
  ///
  /// @param other The other chunk.
  /// @return Whether both are the same.
  bool operator==(const GenericDataChunk& other) const {
    return tag == other.tag && key_position == other.key_position &&
           transfer_id == other.transfer_id &&
           packet_index == other.packet_index &&
           transfer_offset == other.transfer_offset &&
           std::ranges::equal(payload.bytes(), other.payload.bytes());
  }
};

/// @brief The GENERICDATA control message types.
enum class GenericDataControlType : std::uint32_t {
  /// @brief Sender: a transfer of this size, chunk count and metadata follows.
  announce = 0,
  /// @brief Receiver: ready for the announced transfer.
  ready = 1,
  /// @brief Receiver: the transfer is refused or failed; carries an error text.
  reject = 2,
  /// @brief Receiver: every chunk arrived.
  complete = 3,
  /// @brief Sender: which chunks are still missing?
  missing_query = 4,
  /// @brief Receiver: these chunk indices are missing (at most 25).
  missing_response = 5,
};

/// @brief How an outgoing transfer ended.
enum class GenericDataOutcome : std::uint8_t {
  /// @brief The receiver reported it complete.
  completed,
  /// @brief The receiver refused it.
  rejected,
  /// @brief 10 s passed since it was announced or the receiver last said
  /// ready.
  timed_out,
};

/// @brief A GENERICDATA control message.
struct GenericDataControl {
  /// @brief The transfer.
  std::uint32_t transfer_id{};
  /// @brief A GenericDataControlType value; unknown values are ignored.
  std::uint32_t type{};
  /// @brief Bytes in the transfer, for announce.
  std::optional<std::uint32_t> data_size{};
  /// @brief The transfer's metadata, for announce.
  std::optional<std::string> metadata{};
  /// @brief Chunks in the transfer, for announce.
  std::optional<std::uint32_t> packet_count{};
  /// @brief Why, for reject.
  std::optional<std::string> error{};
  /// @brief Missing chunk indices, for a missing response.
  std::vector<std::uint32_t> missing_packets{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const GenericDataControl&) const = default;
};

/// @brief Encodes and decodes the smaller Takion packet bodies. Stateless;
/// safe from any thread.
class TypedMessageCodec {
 public:
  /// @brief Tells whether a version has GKTRACE messages.
  ///
  /// @param profile The version's layout.
  /// @return Whether it has the trace_messages feature.
  static constexpr bool trace_messages_enabled(
      const WireProfile& profile) noexcept {
    return profile.features().contains(Feature::trace_messages);
  }

  /// @brief Tells whether a version has generic-data transfers.
  ///
  /// @param profile The version's layout.
  /// @return Whether it is version 15 or later with the direct-message
  ///     encryption and generic control data features.
  static constexpr bool generic_data_enabled(
      const WireProfile& profile) noexcept {
    return profile.version() >= generic_data_minimum_version &&
           profile.features().contains(Feature::direct_message_encryption) &&
           profile.features().contains(Feature::generic_control_data);
  }

  /// @brief Encodes a length-prefixed body: a u16 big-endian length and the
  /// payload.
  ///
  /// @param payload The payload.
  /// @return The body after the type byte.
  /// @retval ParseFailureReason::invalid_length It exceeds 65535 bytes.
  [[nodiscard]] static auto encode_length_prefixed_body(
      std::span<const std::byte> payload)
      -> Expected<std::vector<std::byte>, ParseFailure>;

  /// @brief Decodes a length-prefixed body; bytes after the announced length
  /// are ignored.
  ///
  /// @param body The body after the type byte.
  /// @return The payload.
  /// @retval ParseFailureReason::truncated The length or payload is short.
  [[nodiscard]] static auto decode_length_prefixed_body(
      std::span<const std::byte> body)
      -> Expected<std::vector<std::byte>, ParseFailure>;

  /// @brief Encodes a CLIENTINFO protobuf message.
  ///
  /// @param values The fields.
  /// @return The message bytes.
  /// @retval ParseFailureReason::invalid_field No session key.
  /// @retval ParseFailureReason::invalid_length It cannot be serialised.
  [[nodiscard]] static auto encode_client_info_message(
      const ClientInfoValues& values)
      -> Expected<std::vector<std::byte>, ParseFailure>;

  /// @brief Decodes a CLIENTINFO protobuf message.
  ///
  /// @param message The message bytes.
  /// @return The fields.
  /// @retval ParseFailureReason::invalid_field Malformed or incomplete.
  /// @retval ParseFailureReason::unsupported_layout Another message type.
  [[nodiscard]] static auto decode_client_info_message(
      std::span<const std::byte> message)
      -> Expected<ClientInfoValues, ParseFailure>;

  /// @brief Encodes a GKTRACE protobuf message.
  ///
  /// @param values The fields.
  /// @return The message bytes.
  /// @retval ParseFailureReason::invalid_length It cannot be serialised.
  [[nodiscard]] static auto encode_trace_message(const TraceValues& values)
      -> Expected<std::vector<std::byte>, ParseFailure>;

  /// @brief Decodes a GKTRACE protobuf message.
  ///
  /// @param message The message bytes.
  /// @return The fields.
  /// @retval ParseFailureReason::invalid_field Malformed or incomplete.
  /// @retval ParseFailureReason::unsupported_layout Another message type.
  [[nodiscard]] static auto decode_trace_message(
      std::span<const std::byte> message)
      -> Expected<TraceValues, ParseFailure>;

  /// @brief Re-encodes a GKTRACE message with its tag and key position zeroed:
  /// the bytes its detached tag covers.
  ///
  /// @param message The message bytes.
  /// @return The canonical bytes.
  /// @retval ParseFailureReason As decode_trace_message().
  [[nodiscard]] static auto trace_authenticated_message(
      std::span<const std::byte> message)
      -> Expected<std::vector<std::byte>, ParseFailure>;

  /// @brief Turns a GMAC tag into the trace message's tag field.
  ///
  /// @param mac The tag.
  /// @return Its bytes as a little-endian u32.
  static constexpr std::uint32_t trace_tag_value(const Mac& mac) noexcept {
    return bytes::load_le<std::uint32_t>(mac);
  }

  /// @brief Turns the trace message's tag field back into tag bytes.
  ///
  /// @param value The field.
  /// @return The tag.
  static constexpr Mac trace_tag_bytes(std::uint32_t value) noexcept {
    Mac mac{};
    bytes::store_le<std::uint32_t>(mac, value);
    return mac;
  }

  /// @brief Encodes a generic-data chunk body, tag and key position as given.
  ///
  /// @param chunk The chunk.
  /// @param profile The version's layout.
  /// @return The body after the type byte and extended header.
  /// @retval ParseFailureReason::unsupported_layout Before version 15.
  /// @retval ParseFailureReason::invalid_length The payload exceeds 32 bits.
  [[nodiscard]] static auto encode_generic_data_chunk_body(
      const GenericDataChunk& chunk, const WireProfile& profile)
      -> Expected<SecretBytes, ParseFailure>;

  /// @brief Decodes a generic-data chunk packet.
  ///
  /// @param packet The split packet.
  /// @param profile The version's layout.
  /// @return The chunk, payload still encrypted.
  /// @retval ParseFailureReason::unknown_packet_family Not type 0x0c.
  /// @retval ParseFailureReason::unsupported_layout Before version 15.
  /// @retval ParseFailureReason::truncated The header is short.
  /// @retval ParseFailureReason::invalid_length The length field disagrees with
  ///     the payload.
  [[nodiscard]] static auto decode_generic_data_chunk(
      const PlainPacket& packet, const WireProfile& profile)
      -> Expected<GenericDataChunk, ParseFailure>;

  /// @brief Encodes a GENERICDATA payload protobuf.
  ///
  /// @param control The message.
  /// @return The payload bytes.
  /// @retval ParseFailureReason::invalid_length It cannot be serialised.
  [[nodiscard]] static auto encode_generic_data_payload(
      const GenericDataControl& control)
      -> Expected<std::vector<std::byte>, ParseFailure>;

  /// @brief Decodes a GENERICDATA payload protobuf.
  ///
  /// @param payload The payload bytes.
  /// @return The message.
  /// @retval ParseFailureReason::invalid_field Malformed or incomplete.
  [[nodiscard]] static auto decode_generic_data_payload(
      std::span<const std::byte> payload)
      -> Expected<GenericDataControl, ParseFailure>;

  /// @brief Wraps a GENERICDATA payload in a DIRECTMESSAGE TakionMessage
  /// addressed to CLIENT.
  ///
  /// @param control The message.
  /// @return The TakionMessage bytes.
  /// @retval ParseFailureReason::invalid_length It cannot be serialised.
  [[nodiscard]] static auto encode_generic_data_direct_message(
      const GenericDataControl& control)
      -> Expected<std::vector<std::byte>, ParseFailure>;

  /// @brief Measures a direct message's data, which ends the message and is
  /// what a session with direct-message encryption encrypts.
  ///
  /// @param bytes The TakionMessage bytes.
  /// @return The size of the data at the end of the bytes; 0 when they are
  ///     not a direct message ending with its data.
  [[nodiscard]] static auto direct_message_data_size(
      std::span<const std::byte> bytes) -> std::size_t;

  /// @brief Unwraps a GENERICDATA direct message.
  ///
  /// @param message The TakionMessage bytes.
  /// @return The control message; nothing when it is some other message.
  /// @retval ParseFailureReason::invalid_field Malformed, or a GENERICDATA
  ///     direct message without valid data.
  [[nodiscard]] static auto decode_generic_data_direct_message(
      std::span<const std::byte> message)
      -> Expected<std::optional<GenericDataControl>, ParseFailure>;

 private:
  /// @brief Bytes of a chunk header after the type byte.
  static constexpr std::size_t generic_data_fields_size =
      generic_data_header_size - packet_type_size;
  /// @brief The nanopb side of the codec: building and reading the generated
  /// messages, kept out of this header.
  class Wire;
};

}  // namespace libchiaking::net::protocol::takion
