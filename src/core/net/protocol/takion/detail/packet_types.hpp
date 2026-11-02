/// @file
/// @brief Declares the vocabulary of a Takion datagram: its packet families,
/// control chunk types, reliable message data types, header sizes and bit
/// fields, and the description of a packet that failed to parse.
///
/// Byte 0 of every Takion packet is its type: the low nibble names the family,
/// bit 0x10 is a flag (on AV packets, the NALU-info descriptor follows the
/// header). From version 20 every family but control, length-prefixed and
/// client information then carries an 8-byte extended header: a u32
/// big-endian sender timestamp and a u32 big-endian packet counter.
#pragma once

#include "src/core/net/protocol/takion/limits.hpp"
#include "src/core/utils/bytes/byte_io.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace libchiaking::net::protocol::takion {

/// @brief Mask of the family in a packet's type byte.
inline constexpr std::uint8_t packet_family_mask = 0x0f;
/// @brief Bytes of a control packet's headers after the type byte: the 12-byte
/// message header and the 4-byte chunk header.
inline constexpr std::size_t control_header_size = 16;
/// @brief Bytes of the PSN relay prefix (PsnEnvelope).
inline constexpr std::size_t psn_envelope_size = 4;
/// @brief Bytes of the version 20 extended header: timestamp and counter,
/// u32 each.
inline constexpr std::size_t extended_header_size = 8;
/// @brief Bytes of the prefix a version 20 reliable message of a fixed-layout
/// data type carries before its body.
inline constexpr std::size_t fixed_data_message_prefix_size = 8;
/// @brief Bytes of a version 7 audio AV header after the type byte: packet
/// index (2), frame index (2), packed unit word (4), codec (1), reserved (8).
inline constexpr std::size_t audio_av_header_size_v7 = 0x11;
/// @brief Bytes of a version 7 video AV header after the type byte: the audio
/// fields plus a 16-bit video word and the adaptive stream byte.
inline constexpr std::size_t video_av_header_size_v7 = 0x14;
/// @brief Bytes of an untagged audio AV header from version 9 (the version 7
/// fields plus one byte); a packet must exceed it to be decoded.
inline constexpr std::size_t reference_audio_header_size = 0x12;
/// @brief Bytes of a tagged audio AV header from version 11 (one more byte for
/// the lane tag); a packet must exceed it to be decoded.
inline constexpr std::size_t reference_tagged_audio_header_size = 0x13;
/// @brief Bytes of a video AV header from version 9 with the NALU-info
/// descriptor; a packet must exceed it to be decoded.
inline constexpr std::size_t reference_video_header_size = 0x17;
/// @brief Bytes of the video NALU-info descriptor that bit 0x10 announces.
inline constexpr std::size_t nalu_info_structs_size = 3;
/// @brief Bytes of a congestion packet including its type byte.
inline constexpr std::size_t congestion_packet_size = 15;
/// @brief Bytes of a length-prefixed packet's header including its type byte.
inline constexpr std::size_t length_prefixed_header_size = 3;
/// @brief Bytes of a generic-data chunk packet's header including its type
/// byte.
inline constexpr std::size_t generic_data_header_size = 22;
/// @brief First Takion version with generic-data transfers.
inline constexpr std::uint32_t generic_data_minimum_version = 15;
/// @brief Shift of the unit index in the packed unit word of video packets and
/// version 7 audio packets (bits 21 to 31).
inline constexpr std::uint32_t av_unit_index_shift = 21;
/// @brief Mask of the unit index after shifting (11 bits).
inline constexpr std::uint32_t av_unit_index_mask = max_unit_index;
/// @brief Shift of "units in frame - 1" in the packed unit word (bits 10 to
/// 20).
inline constexpr std::uint32_t av_units_total_shift = 10;
/// @brief Mask of "units in frame - 1" after shifting (11 bits).
inline constexpr std::uint32_t av_units_total_mask = max_unit_index;
/// @brief Mask of the FEC unit count in the packed unit word (bits 0 to 9).
inline constexpr std::uint32_t av_units_fec_mask = 0x3ff;
/// @brief Mask of the audio unit shape field in the packed word from version
/// 15 (bits 0 to 11).
inline constexpr std::uint32_t av_audio_packed_field_mask = 0xfff;
/// @brief Shift of the audio mode in the packed word from version 15 (bits 12
/// to 15).
inline constexpr std::uint32_t av_audio_mode_shift = 12;
/// @brief Mask of the audio mode after shifting.
inline constexpr std::uint32_t av_audio_mode_mask = 0xf;
/// @brief Mask of the lane tag in an audio packet's tag byte from version 15
/// (low nibble).
inline constexpr std::uint8_t av_audio_metadata_mask = 0x0f;
/// @brief Shift of the audio profile in the tag byte from version 15 (high
/// nibble).
inline constexpr std::uint8_t av_audio_profile_shift = 4;
/// @brief Shift of the adaptive stream index in a video packet's stream byte
/// (top 3 bits).
inline constexpr std::uint8_t adaptive_stream_index_shift = 5;
/// @brief Highest adaptive stream index, 7.
inline constexpr std::uint8_t max_adaptive_stream_index = 7;
/// @brief Bit of the video stream byte flagging dynamic-range metadata, from
/// version 15.
inline constexpr std::uint8_t video_dynamic_range_flag_bit = 0x01;
/// @brief Bit of the video stream byte asking not to present the frame, from
/// version 24.
inline constexpr std::uint8_t video_suppress_presentation_bit = 0x02;

/// @brief The packet families a Takion type byte can name.
enum class PacketFamily : std::uint8_t {
  /// @brief SCTP-style control: handshake chunks and reliable DATA / DATA_ACK.
  control = 0x00,
  /// @brief Client to console: controller button, touch and other event
  /// records.
  feedback_history = 0x01,
  /// @brief Console to client: a video unit.
  video = 0x02,
  /// @brief Console to client: an audio or haptics unit; client to console:
  /// microphone audio and Senkusha probes.
  audio_and_haptics = 0x03,
  /// @brief A body with a 16-bit length prefix, sent unauthenticated.
  length_prefixed = 0x04,
  /// @brief Client to console: received and lost packet counts for congestion
  /// control.
  congestion = 0x05,
  /// @brief Client to console: the controller's sticks, triggers and motion.
  feedback_state = 0x06,
  /// @brief Session start information (CLIENTINFO), sent unauthenticated.
  client_information = 0x08,
  /// @brief GKTRACE: a sequence number with its own detached tag.
  trace = 0x0a,
  /// @brief A chunk of a generic-data transfer, from version 15.
  generic_data = 0x0c,
  /// @brief An extended message (round-trip ping and pong, link metrics),
  /// when the version supports them.
  extended_message = 0x0e,
};

/// @brief A packet's type byte.
struct PacketType {
  /// @brief Names the family of a type nibble.
  ///
  /// @param code The low nibble.
  /// @return The family; nothing for an unassigned value.
  [[nodiscard]] static constexpr auto family_from_code(
      std::uint8_t code) noexcept -> std::optional<PacketFamily> {
    switch (code) {
      case 0x00:
        return PacketFamily::control;
      case 0x01:
        return PacketFamily::feedback_history;
      case 0x02:
        return PacketFamily::video;
      case 0x03:
        return PacketFamily::audio_and_haptics;
      case 0x04:
        return PacketFamily::length_prefixed;
      case 0x05:
        return PacketFamily::congestion;
      case 0x06:
        return PacketFamily::feedback_state;
      case 0x08:
        return PacketFamily::client_information;
      case 0x0a:
        return PacketFamily::trace;
      case 0x0c:
        return PacketFamily::generic_data;
      case 0x0e:
        return PacketFamily::extended_message;
      default:
        return std::nullopt;
    }
  }

  /// @brief Tells whether a family carries the version 20 extended header.
  ///
  /// @param family_code The low nibble.
  /// @return Whether it is any family but control, length-prefixed and client
  ///     information.
  static constexpr bool has_extended_header(std::uint8_t family_code) noexcept {
    const auto base = family_code & packet_family_mask;
    return base != static_cast<std::uint8_t>(PacketFamily::control) &&
           base != static_cast<std::uint8_t>(PacketFamily::length_prefixed) &&
           base != static_cast<std::uint8_t>(PacketFamily::client_information);
  }

  /// @brief Makes the type byte of a family with no flags.
  ///
  /// @param family The family.
  /// @return The type.
  static constexpr auto of(PacketFamily family) noexcept -> PacketType {
    return PacketType{static_cast<std::uint8_t>(family)};
  }

  /// @brief The byte as on the wire.
  std::uint8_t raw{};

  /// @brief Reads the family nibble.
  ///
  /// @return The low nibble.
  constexpr std::uint8_t family_code() const noexcept {
    return raw & packet_family_mask;
  }

  /// @brief Reads the flag bits.
  ///
  /// @return The high nibble, in place.
  constexpr std::uint8_t flags() const noexcept {
    return raw & static_cast<std::uint8_t>(~packet_family_mask);
  }

  /// @brief Names the family.
  ///
  /// @return The family; nothing when unassigned.
  constexpr auto family() const noexcept -> std::optional<PacketFamily> {
    return family_from_code(family_code());
  }

  /// @brief Tells whether the family is assigned.
  ///
  /// @return Whether it is.
  constexpr bool is_known() const noexcept { return family().has_value(); }
};

/// @brief The chunk types a control packet can carry; SCTP's numbering.
enum class ControlChunk : std::uint8_t {
  /// @brief DATA: one reliable fragment.
  data = 0x00,
  /// @brief INIT: the client opens the association.
  init = 0x01,
  /// @brief INIT_ACK: the console answers with its values and a cookie.
  init_acknowledgement = 0x02,
  /// @brief DATA_ACK: a selective acknowledgement.
  data_acknowledgement = 0x03,
  /// @brief COOKIE_ECHO: the client returns the cookie.
  cookie = 0x0a,
  /// @brief COOKIE_ACK: the association is up.
  cookie_acknowledgement = 0x0b,
};

/// @brief A chunk type byte as received.
struct ControlChunkType {
  /// @brief Names a chunk type.
  ///
  /// @param code The byte.
  /// @return The type; nothing when unknown.
  [[nodiscard]] static constexpr auto from_code(std::uint8_t code) noexcept
      -> std::optional<ControlChunk> {
    switch (code) {
      case 0x00:
        return ControlChunk::data;
      case 0x01:
        return ControlChunk::init;
      case 0x02:
        return ControlChunk::init_acknowledgement;
      case 0x03:
        return ControlChunk::data_acknowledgement;
      case 0x0a:
        return ControlChunk::cookie;
      case 0x0b:
        return ControlChunk::cookie_acknowledgement;
      default:
        return std::nullopt;
    }
  }

  /// @brief The byte as on the wire.
  std::uint8_t raw{};

  /// @brief Names the chunk type.
  ///
  /// @return The type; nothing when unknown.
  constexpr auto known() const noexcept -> std::optional<ControlChunk> {
    return from_code(raw);
  }
};

/// @brief The data types of a reliable message, carried in its first
/// fragment.
enum class MessageDataKind : std::uint8_t {
  /// @brief A protobuf TakionMessage.
  protobuf = 0x00,
  /// @brief Controller rumble: player selector, left and right motor strength.
  rumble = 0x07,
  /// @brief Controller lightbar colour and pad settings.
  pad_information = 0x09,
  /// @brief DualSense adaptive trigger effects.
  trigger_effects = 0x0b,
  /// @brief Data type 13, a selector and four bytes, from version 17; its
  /// meaning is not known.
  controller_message_13 = 0x0d,
};

/// @brief A data type byte as received.
struct MessageDataType {
  /// @brief Names a data type.
  ///
  /// @param code The byte.
  /// @return The type; nothing when the library does not model it.
  [[nodiscard]] static constexpr auto kind_from_code(std::uint8_t code) noexcept
      -> std::optional<MessageDataKind> {
    switch (code) {
      case 0x00:
        return MessageDataKind::protobuf;
      case 0x07:
        return MessageDataKind::rumble;
      case 0x09:
        return MessageDataKind::pad_information;
      case 0x0b:
        return MessageDataKind::trigger_effects;
      case 0x0d:
        return MessageDataKind::controller_message_13;
      default:
        return std::nullopt;
    }
  }

  /// @brief Tells whether a data type has the 8-byte prefix from version 20.
  ///
  /// @param raw_data_type The byte.
  /// @return Whether it is 5, 7, 9, 11, 12 or 13.
  static constexpr bool has_fixed_prefix(std::uint8_t raw_data_type) noexcept {
    switch (raw_data_type) {
      case 5:
      case 7:
      case 9:
      case 11:
      case 12:
      case 13:
        return true;
      default:
        return false;
    }
  }

  /// @brief The byte as on the wire.
  std::uint8_t raw{};

  /// @brief Names the data type.
  ///
  /// @return The type; nothing when not modelled.
  constexpr auto known() const noexcept -> std::optional<MessageDataKind> {
    return kind_from_code(raw);
  }
};

/// @brief A byte offset into a packet.
struct PacketOffset {
  /// @brief The offset.
  std::size_t value{};
};

/// @brief Why a packet could not be parsed or built.
enum class ParseFailureReason : std::uint8_t {
  /// @brief It ended before a field.
  truncated,
  /// @brief A length field disagrees with the packet, or a value is too long.
  invalid_length,
  /// @brief The type byte names no family.
  unknown_packet_family,
  /// @brief The control chunk type is unknown.
  unknown_control_chunk,
  /// @brief A flag is not allowed here.
  invalid_flags,
  /// @brief A field holds a value out of range.
  invalid_field,
  /// @brief The family or field is not part of this version's layout.
  unsupported_layout,
  /// @brief A tag did not check.
  authentication_failed,
};

/// @brief Where and why a packet failed to parse or build.
struct ParseFailure {
  /// @brief The packet's type byte, when known.
  PacketType packet_type{};
  /// @brief Where the problem is.
  PacketOffset offset{};
  /// @brief Why.
  ParseFailureReason reason{ParseFailureReason::invalid_field};
  /// @brief Bytes the field needed, or the value expected.
  std::size_t needed{};
  /// @brief Bytes available, or the value found.
  std::size_t available{};

  /// @brief Describes a failure in full.
  ///
  /// @param type The packet type.
  /// @param reason Why.
  /// @param offset Where.
  /// @param needed What was needed.
  /// @param available What was there.
  /// @return The failure.
  static constexpr auto at(PacketType type, ParseFailureReason reason,
                           std::size_t offset, std::size_t needed,
                           std::size_t available) noexcept -> ParseFailure {
    return ParseFailure{type, PacketOffset{offset}, reason, needed, available};
  }

  /// @brief Describes a failure with needed and available both 1.
  ///
  /// @param type The packet type.
  /// @param reason Why.
  /// @param offset Where.
  /// @return The failure.
  static constexpr auto of(PacketType type, ParseFailureReason reason,
                           std::size_t offset = 0) noexcept -> ParseFailure {
    return at(type, reason, offset, 1, 1);
  }

  /// @brief Describes a truncation a byte reader reported.
  ///
  /// @param type The packet type.
  /// @param shortfall The reader's shortfall.
  /// @param base Added to its offsets, when it read from inside the packet.
  /// @return The failure.
  static constexpr auto short_by(PacketType type,
                                 const bytes::Shortfall& shortfall,
                                 std::size_t base = 0) noexcept
      -> ParseFailure {
    return at(type, ParseFailureReason::truncated, base + shortfall.offset,
              base + shortfall.needed, base + shortfall.available);
  }
};

}  // namespace libchiaking::net::protocol::takion
