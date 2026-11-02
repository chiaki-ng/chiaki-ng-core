/// @file
/// @brief Declares Kakusan's transport frames: reliable control fragments,
/// forwarded media datagrams and media acknowledgements.
#pragma once

#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace libchiaking::net::kakusan {

/// @brief Prefix of a reliable control record (data or ACK).
inline constexpr std::string_view control_prefix = "RCC1";
/// @brief Prefix of a forwarded media datagram, sent outside DTLS.
inline constexpr std::string_view media_prefix = "RCPM";
/// @brief Prefix of a guest's media acknowledgement record.
inline constexpr std::string_view media_ack_prefix = "RCA1";
/// @brief Domain label of the connection id hash.
inline constexpr std::string_view connection_label = "RCP/1/connection";
/// @brief DTLS exporter label of the per-spoke media MAC key.
inline constexpr std::string_view media_exporter_label = "EXPORTER-RCP-1-media";

/// @brief Largest UDP payload sent, in bytes.
inline constexpr std::size_t max_datagram_size = 1200;
/// @brief Bytes of an RCC1 header.
inline constexpr std::size_t control_header_size = 16;
/// @brief Bytes of every control fragment but the last.
inline constexpr std::size_t control_fragment_size = 960;
/// @brief Largest control message, in bytes.
inline constexpr std::size_t max_control_message_size = 8192;
/// @brief Most fragments of a control message.
inline constexpr std::size_t max_control_fragments = 9;
/// @brief Bytes of an RCPM header.
inline constexpr std::size_t media_header_size = 60;
/// @brief Bytes of an RCPM tag: truncated HMAC-SHA256.
inline constexpr std::size_t media_tag_size = 16;
/// @brief Bytes of every media fragment but the last.
inline constexpr std::size_t media_fragment_size = 1124;
/// @brief Largest source Takion datagram forwarded, in bytes.
inline constexpr std::size_t max_source_datagram_size = 8168;
/// @brief Bytes of the media MAC key.
inline constexpr std::size_t media_key_size = 32;
/// @brief Bytes of an RCA1 header.
inline constexpr std::size_t media_ack_header_size = 20;
/// @brief Most ranges in a media acknowledgement.
inline constexpr std::size_t max_ack_ranges = 32;
/// @brief Bytes of one acknowledged range: two u64.
inline constexpr std::size_t media_ack_range_size = 16;
/// @brief Zero bytes after an RCA1's range count.
inline constexpr std::size_t media_ack_reserved_size = 3;
/// @brief The only media epoch.
inline constexpr std::uint32_t media_epoch = 1;

/// @brief A spoke's connection id: the first 16 bytes of
/// SHA-256(`RCP/1/connection` 0x00 transcript).
using ConnectionId = std::array<std::byte, 16>;
/// @brief A spoke's media MAC key, exported from its DTLS association.
using MediaKey = SecretArray<media_key_size>;

/// @brief The kind byte of an RCC1 record.
enum class ControlKind : std::uint8_t {
  /// @brief A fragment of a message.
  data = 1,
  /// @brief The acknowledgement of a whole message.
  ack = 2,
};

/// @brief One decoded RCC1 record.
struct ControlFragment {
  /// @brief Data or ACK.
  ControlKind kind{ControlKind::data};
  /// @brief The message id, from 1.
  std::uint32_t message_id{};
  /// @brief The fragment index; 0 for an ACK.
  std::uint16_t index{};
  /// @brief Fragments in the message, 1 to 9; 0 for an ACK.
  std::uint16_t count{};
  /// @brief The fragment's bytes, a view into the record; empty for an ACK.
  std::span<const std::byte> payload;
};

/// @brief The kind byte of an RCPM datagram.
enum class MediaKind : std::uint8_t {
  /// @brief A fragment of a source video datagram.
  video = 0,
  /// @brief A fragment of a source audio datagram.
  audio = 1,
  /// @brief A 76-byte probe with no source data, sent to elicit
  /// acknowledgements.
  probe = 2,
};

/// @brief The fields of an RCPM header.
struct MediaHeader {
  /// @brief Video, audio or probe.
  MediaKind kind{MediaKind::video};
  /// @brief Increases by one for every datagram sent, probes included; never
  /// wraps.
  std::uint64_t datagram_sequence{};
  /// @brief Increases by one for each source datagram admitted; 0 for probes.
  std::uint64_t packet_sequence{};
  /// @brief The source datagram's full 64-bit key position; 0 for probes.
  std::uint64_t key_position{};
  /// @brief The source datagram's size, 1 to 8168; 0 for probes.
  std::uint16_t original_size{};
  /// @brief Offset of this fragment, a multiple of 1124.
  std::uint16_t fragment_offset{};
  /// @brief This fragment's size: min(1124, original − offset); 0 for probes.
  std::uint16_t fragment_size{};
};

/// @brief A decoded RCPM datagram.
struct MediaDatagram {
  /// @brief The header.
  MediaHeader header;
  /// @brief The ciphertext fragment, a view into the datagram.
  std::span<const std::byte> fragment;
};

/// @brief An inclusive range of acknowledged datagram sequences.
struct AckRange {
  /// @brief The first sequence.
  std::uint64_t low{};
  /// @brief The last sequence.
  std::uint64_t high{};

  /// @brief Compares both ends.
  ///
  /// @return True when equal.
  constexpr bool operator==(const AckRange&) const noexcept = default;
};

/// @brief A guest's media acknowledgement.
struct MediaAck {
  /// @brief Whether the ranges are valid.
  ///
  /// @return True for 1 to 32 ranges, the first ending at `largest`, each
  ///     with
  ///     low <= high, descending, neither overlapping nor adjacent.
  auto well_formed() const noexcept -> bool;

  /// @brief The largest sequence acknowledged.
  std::uint64_t largest{};
  /// @brief Microseconds from accepting `largest` to sending the
  /// acknowledgement.
  std::uint32_t delay_us{};
  /// @brief The ranges, newest first.
  std::vector<AckRange> ranges;
};

/// @brief Encodes and decodes Kakusan's transport frames (RCP TRANSPORT
/// sections 1 to 4); all integers big-endian. Stateless.
///
/// RCC1, inside DTLS: prefix (4), kind (1), zero (1), fragment index (u16),
/// message id (u32), fragment count (u16), fragment length (u16), payload.
///
/// RCPM, plain UDP: prefix (4), version 1 (1), kind (1), zero (u16),
/// connection id (16), datagram sequence (u64), packet sequence (u64), epoch 1
/// (u32), key position (u64), original size (u16), fragment offset (u16),
/// fragment size (u16), zero (u16), fragment, then the first 16 bytes of
/// HMAC-SHA256(media key, everything before the tag).
///
/// RCA1, inside DTLS: prefix (4), largest (u64), delay in microseconds (u32),
/// range count (1), three zero bytes, then count pairs of (low, high) u64.
class TransportFrameCodec {
 public:
  /// @brief How many fragments a message needs.
  ///
  /// @param message_size The message's bytes.
  /// @return ceil(size / 960).
  static auto control_fragment_count(std::size_t message_size) noexcept
      -> std::size_t;

  /// @brief Splits a message into RCC1 data records.
  ///
  /// @param message_id The message id; not 0.
  /// @param message The message, 1 to 8192 bytes.
  /// @return The records in index order.
  /// @retval Error::protocol The id is 0 or the size is out of range.
  static auto encode_control_fragments(std::uint32_t message_id,
                                       std::span<const std::byte> message)
      -> Result<std::vector<std::vector<std::byte>>>;

  /// @brief Builds the ACK of a message.
  ///
  /// @param message_id The completed message's id.
  /// @return The 16-byte record, with zero index, count and length.
  static auto encode_control_ack(std::uint32_t message_id)
      -> std::array<std::byte, control_header_size>;

  /// @brief Reads an RCC1 record.
  ///
  /// @param record The record.
  /// @return The fragment.
  /// @retval Error::protocol Any field is out of range: short, wrong prefix,
  ///     non-zero reserved byte, id 0, length mismatch, a non-final fragment
  ///     not 960 bytes, or a ninth fragment over 512 bytes.
  static auto decode_control(std::span<const std::byte> record)
      -> Result<ControlFragment>;

  /// @brief Derives a spoke's connection id from its transcript.
  ///
  /// @param crypto Hashes.
  /// @param transcript The transcript.
  /// @return The id.
  /// @retval Error Hashing failed.
  static auto connection_id(platform::CryptoProvider& crypto,
                            const Digest256& transcript)
      -> Result<ConnectionId>;

  /// @brief How many fragments a source datagram needs.
  ///
  /// @param original_size Its bytes.
  /// @return ceil(size / 1124).
  static auto media_fragment_count(std::size_t original_size) noexcept
      -> std::size_t;

  /// @brief Builds an RCPM datagram and its tag.
  ///
  /// @param crypto Computes the tag.
  /// @param key The spoke's media key.
  /// @param connection The spoke's connection id.
  /// @param header The header fields.
  /// @param fragment The ciphertext fragment.
  /// @return The datagram, at most 1200 bytes.
  /// @retval Error::protocol The fragment does not fit the header.
  /// @retval Error The MAC failed.
  static auto encode_media(platform::CryptoProvider& crypto,
                           const MediaKey& key, const ConnectionId& connection,
                           const MediaHeader& header,
                           std::span<const std::byte> fragment)
      -> Result<std::vector<std::byte>>;

  /// @brief Whether a datagram is RCPM for this spoke, before any MAC work.
  ///
  /// @param datagram The datagram.
  /// @param connection The spoke's connection id.
  /// @return True for the right size, prefix, version and connection id.
  static auto media_matches(std::span<const std::byte> datagram,
                            const ConnectionId& connection) noexcept -> bool;

  /// @brief Checks an RCPM tag in constant time.
  ///
  /// @param crypto Computes the tag.
  /// @param key The spoke's media key.
  /// @param datagram The datagram.
  /// @return Whether the tag is valid; false for a wrong size.
  /// @retval Error The MAC failed.
  static auto authenticate_media(platform::CryptoProvider& crypto,
                                 const MediaKey& key,
                                 std::span<const std::byte> datagram)
      -> Result<bool>;

  /// @brief Reads an authenticated RCPM datagram's fields.
  ///
  /// @param datagram The datagram.
  /// @return The header and fragment.
  /// @retval Error::protocol A field is out of range.
  static auto decode_media(std::span<const std::byte> datagram)
      -> Result<MediaDatagram>;

  /// @brief Builds an RCA1 record.
  ///
  /// @param ack The acknowledgement.
  /// @return The record.
  /// @retval Error::protocol It is not well formed.
  static auto encode_media_ack(const MediaAck& ack)
      -> Result<std::vector<std::byte>>;
  /// @brief Reads an RCA1 record.
  ///
  /// @param record The record.
  /// @return The acknowledgement.
  /// @retval Error::protocol A field or the range order is invalid.
  static auto decode_media_ack(std::span<const std::byte> record)
      -> Result<MediaAck>;

 private:
  /// @brief The RCPM version byte.
  static constexpr std::uint8_t media_version = 1;
  /// @brief Largest final fragment of a nine-fragment control message.
  static constexpr std::size_t final_fragment_limit = 512;
  /// @brief Offset of the connection id in an RCPM header.
  static constexpr std::size_t connection_offset = 8;
  /// @brief Computes an RCPM tag.
  ///
  /// @param crypto Computes the MAC.
  /// @param key The media key.
  /// @param authenticated The header and fragment.
  /// @return The first 16 bytes of HMAC-SHA256.
  /// @retval Error The MAC failed.
  static auto media_tag(platform::CryptoProvider& crypto, const MediaKey& key,
                        std::span<const std::byte> authenticated)
      -> Result<std::array<std::byte, media_tag_size>>;
  /// @brief Whether header fields and a fragment agree.
  ///
  /// @param header The header.
  /// @param fragment_size The fragment's actual size.
  /// @return True for a probe with every source field zero, or a fragment at
  ///     a
  ///     1124-byte boundary of a 1 to 8168 byte datagram with the expected
  ///     size.
  static constexpr auto valid_media_shape(const MediaHeader& header,
                                          std::size_t fragment_size) noexcept
      -> bool {
    if (header.fragment_size != fragment_size) return false;
    if (header.kind == MediaKind::probe)
      return header.packet_sequence == 0 && header.key_position == 0 &&
             header.original_size == 0 && header.fragment_offset == 0 &&
             header.fragment_size == 0;
    if (header.original_size < 1 ||
        header.original_size > max_source_datagram_size ||
        header.fragment_offset >= header.original_size ||
        header.fragment_offset % media_fragment_size != 0)
      return false;
    const auto rest =
        static_cast<std::size_t>(header.original_size - header.fragment_offset);
    return header.fragment_size == std::min(media_fragment_size, rest);
  }
};

}  // namespace libchiaking::net::kakusan
