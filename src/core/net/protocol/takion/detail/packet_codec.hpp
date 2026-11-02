/// @file
/// @brief Declares PacketCodec, which encodes and decodes Takion's binary
/// (non-protobuf) packets for each version's layout, and the values those
/// packets carry: AV units, Senkusha probes, congestion reports, controller
/// state and controller event records.
///
/// AV header after the type byte and any extended header (all big-endian):
///
/// | Offset | Size | Field |
/// |-------:|-----:|-------|
/// | 0 | 2 | packet index |
/// | 2 | 2 | frame index |
/// | 4 | 4 | packed unit word |
/// | 8 | 1 | codec |
/// | 9 | 8 | reserved |
/// | 17 | 2 | video only: video word |
/// | 19 | 1 | video only: adaptive stream byte (index in the top 3 bits) |
///
/// then, with bit 0x10, the 3-byte NALU-info descriptor for video and for
/// version 7 audio; for audio from version 9 one byte (two with bit 0x10
/// from version 15), and from version 11 the lane tag byte. In video
/// packets and version 7 audio packets the unit word holds unit index << 21,
/// (units in frame - 1) << 10 and the FEC unit count; in later audio packets
/// it holds unit index << 24, (units in frame - 1) << 16 and a 16-bit unit
/// shape, of which from version 15 the top 4 bits are the audio mode.
#pragma once

#include "libchiaking/detail/secret_string.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_types.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/result.hpp"
#include "src/core/net/protocol/takion/source_tap.hpp"
#include "src/core/utils/bytes/byte_io.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief Mask of the source unit count in the audio shape when the version
/// encodes counts only (feature av_unit_count_only_encoding).
inline constexpr std::uint32_t av_unit_count_only_source_mask = 0x1fff;
/// @brief Mask of the source unit count in the audio shape otherwise (bits 0
/// to 3).
inline constexpr std::uint32_t av_unit_source_mask = 0xf;
/// @brief Shift of the redundant unit count in the audio shape (bits 4 to 7).
inline constexpr std::uint32_t av_unit_redundant_shift = 4;
/// @brief Mask of the redundant unit count after shifting.
inline constexpr std::uint32_t av_unit_redundant_mask = 0xf;
/// @brief Shift of the per-unit size in the audio shape (bits 8 and up).
inline constexpr std::uint32_t av_unit_size_shift = 8;
/// @brief Largest per-unit size, in bytes.
inline constexpr std::uint32_t av_unit_size_max = 0xffff;

/// @brief The version 20 extended header, which follows the type byte of most
/// families.
struct ExtendedPacketHeader {
  /// @brief The sender's clock, in microseconds since its session epoch,
  /// truncated to 32 bits.
  std::uint32_t timestamp{};
  /// @brief The sender's packet counter, one per packet with this header.
  std::uint32_t counter{};
};

/// @brief A packet split into type, extended header and the rest.
///
/// The payload is borrowed: from the packet bytes a decode read, or from the
/// caller's buffer for an encode, which must outlive the packet.
struct PlainPacket {
  /// @brief The type byte.
  PacketType type{};
  /// @brief The extended header, present exactly when the version and family
  /// call for it.
  std::optional<ExtendedPacketHeader> extended_header{};
  /// @brief Everything after the headers.
  std::span<const std::byte> payload{};
};

/// @brief The header fields of a Senkusha probe to encode.
struct SenkushaProbeValues {
  /// @brief Whether it is a video packet; audio otherwise.
  bool video{};
  /// @brief Whether to set bit 0x10 and add zeroed NALU-info bytes.
  bool uses_nalu_info_structs{};
  /// @brief Packet index.
  std::uint16_t packet_index{};
  /// @brief Frame index; the test index or request id.
  std::uint16_t frame_index{};
  /// @brief Unit index, at most 2047.
  std::uint16_t unit_index{};
  /// @brief Units in the frame, 1 to 2048.
  std::uint16_t units_in_frame_total{};
  /// @brief FEC units, at most 1023.
  std::uint16_t units_in_frame_fec{};
  /// @brief Codec byte.
  std::uint8_t codec{};
  /// @brief Video word, for video.
  std::uint16_t video_word{};
  /// @brief Adaptive stream index, at most 7, for video.
  std::uint8_t adaptive_stream_index{};
  /// @brief Payload after the header.
  std::vector<std::byte> payload;
};

/// @brief The slice descriptor of a video unit with bit 0x10: a 24-bit
/// big-endian word of slice index (bits 13 and up), slice count (bits 2 to
/// 12), start (bit 1) and end (bit 0).
struct VideoSliceExtension {
  /// @brief Slice index within the frame.
  std::uint16_t index{};
  /// @brief Slices in the frame.
  std::uint16_t count{};
  /// @brief Whether this unit starts a slice.
  bool start{};
  /// @brief Whether this unit ends a slice.
  bool end{};

  /// @brief Compares every field.
  ///
  /// @return Whether both descriptors are the same.
  constexpr bool operator==(const VideoSliceExtension&) const noexcept =
      default;
};

/// @brief A decoded video or audio unit.
struct AvPacketValues {
  /// @brief The type byte.
  PacketType type{};
  /// @brief Whether bit 0x10 was set.
  bool uses_nalu_info_structs{};
  /// @brief Packet index.
  std::uint16_t packet_index{};
  /// @brief Frame index.
  std::uint16_t frame_index{};
  /// @brief Unit index within the frame.
  std::uint16_t unit_index{};
  /// @brief Units in the frame. Versions 22 to 25 mask it to 11 bits for video,
  /// so a full 2048 reads as 0.
  std::uint16_t units_in_frame_total{};
  /// @brief FEC units in the frame for video; for audio, the unit shape field
  /// that audio_unit_shape() reads.
  std::uint16_t units_in_frame_fec{};
  /// @brief Codec byte.
  std::uint8_t codec{};
  /// @brief Video word.
  std::uint16_t video_word{};
  /// @brief Adaptive stream index.
  std::uint8_t adaptive_stream_index{};
  /// @brief Unit data after the header; the codec leaves it encrypted if the
  /// session is, and the client decrypts it unless `encrypted` says not.
  std::vector<std::byte> data{};
  /// @brief Whether an audio unit carries haptics: its lane tag is 2 to 5.
  bool is_haptics{};
  /// @brief The lane tag of an audio unit, from version 11; the low nibble only
  /// from version 15.
  std::optional<std::uint8_t> av_tag{};
  /// @brief Audio mode from the unit word, from version 15.
  std::uint8_t audio_mode{};
  /// @brief Audio profile from the tag byte's high nibble, from version 15.
  std::uint8_t audio_profile{};
  /// @brief Slice descriptor of a video unit with bit 0x10, from version 9.
  std::optional<VideoSliceExtension> slice{};
  /// @brief Dynamic-range flag of a video unit, from version 15.
  bool dynamic_range_flag{};
  /// @brief Whether a video unit asks not to be presented, from version 24.
  bool suppress_presentation{};
  /// @brief Whether data is still ciphertext in an encrypted session: the
  /// client's VideoUnitGate called the unit's frame stale, so it was not
  /// decrypted and must be refused as stale.
  bool encrypted{};
};

/// @brief A video or audio unit's header fields beside a view of its data, for
/// a reader that fills AvPacketValues::data itself, as decryption straight
/// out of the received packet does.
struct AvPacketHeader {
  /// @brief Every field of the unit; its data is left empty.
  AvPacketValues values;
  /// @brief The unit data after the header, a view of the packet's payload.
  std::span<const std::byte> data;
};

/// @brief How the units of one audio frame are laid out.
struct AudioUnitShape {
  /// @brief Units carrying audio.
  std::uint16_t source{};
  /// @brief Units carrying redundancy.
  std::uint16_t redundant{};
  /// @brief Bytes per unit.
  std::uint16_t size{};
};

/// @brief An encoded Senkusha probe ready to post.
struct EncodedSenkushaProbe {
  /// @brief video or audio_and_haptics.
  PacketFamily family{};
  /// @brief The bytes after the type byte.
  std::vector<std::byte> encoded_av_payload;
  /// @brief The datagram's full size including type byte and envelope.
  std::size_t datagram_size{};
};

/// @brief A congestion report's fields.
struct CongestionValues {
  /// @brief The first field; always 0 from the library.
  std::uint16_t word{};
  /// @brief Packets the console sent in the period, as the client counts them:
  /// received plus lost, at most 65535.
  std::uint16_t server_sent{};
  /// @brief Packets lost in the period, capped at the configured loss ceiling.
  std::uint16_t lost{};
};

/// @brief An encoded congestion report.
struct EncodedCongestion {
  /// @brief The 14 bytes after the type byte: the three fields (u16 each) and 8
  /// zero bytes.
  std::vector<std::byte> encoded_payload;
  /// @brief The datagram's full size including type byte and envelope.
  std::size_t datagram_size{};
};

/// @brief Bytes of a version 9 controller state payload.
inline constexpr std::size_t feedback_state_v9_payload_size = 0x19;
/// @brief Bytes of a version 12 controller state payload, with trigger states.
inline constexpr std::size_t feedback_state_v12_payload_size = 0x1c;
/// @brief Largest controller event record, in bytes.
inline constexpr std::size_t feedback_record_max_size = 50;

/// @brief First version whose button records may carry id 20.
inline constexpr std::uint32_t feedback_button_id_20_version = 12;
/// @brief First version whose button records may carry id 21.
inline constexpr std::uint32_t feedback_button_id_21_version = 16;
/// @brief First version whose button records may set the record flag.
inline constexpr std::uint32_t feedback_button_flag_version = 16;
/// @brief First version whose button records end with a trailing byte.
inline constexpr std::uint32_t feedback_button_trailing_byte_version = 19;
/// @brief First version whose state payload carries the second motion flag.
inline constexpr std::uint32_t feedback_motion_flag_bits_version = 16;
/// @brief First version with kind 0 records.
inline constexpr std::uint32_t feedback_kind0_version = 14;
/// @brief First version with legacy (7-byte) kind 1 records, used up to
/// version 16.
inline constexpr std::uint32_t feedback_kind1_legacy_version = 14;
/// @brief First version with moded kind 1 records.
inline constexpr std::uint32_t feedback_kind1_mode_version = 17;

/// @brief The controller state a feedback state packet reports.
///
/// Non-finite gyro values are sent as 0, non-finite acceleration as (0, 1, 0)
/// and a non-finite orientation as the identity quaternion.
struct FeedbackStateValues {
  /// @brief Controller slot, 0 to 3.
  std::uint8_t controller_id{};
  /// @brief Angular velocity about x, in radians per second; clamped to ±30.
  float gyro_x{};
  /// @brief Angular velocity about y, in radians per second; clamped to ±30.
  float gyro_y{};
  /// @brief Angular velocity about z, in radians per second; clamped to ±30.
  float gyro_z{};
  /// @brief Acceleration along x, in g; clamped to ±5.
  float accel_x{};
  /// @brief Acceleration along y, in g; clamped to ±5.
  float accel_y{};
  /// @brief Acceleration along z, in g; clamped to ±5.
  float accel_z{};
  /// @brief Orientation quaternion, x.
  float orient_x{};
  /// @brief Orientation quaternion, y.
  float orient_y{};
  /// @brief Orientation quaternion, z.
  float orient_z{};
  /// @brief Orientation quaternion, w.
  float orient_w{};
  /// @brief Left stick x.
  std::int16_t left_x{};
  /// @brief Left stick y.
  std::int16_t left_y{};
  /// @brief Right stick x.
  std::int16_t right_x{};
  /// @brief Right stick y.
  std::int16_t right_y{};
  /// @brief Left trigger effect state byte, from version 12.
  std::uint8_t left_trigger_state{};
  /// @brief Right trigger effect state byte, from version 12.
  std::uint8_t right_trigger_state{};
  /// @brief Whether the trigger states are emulated rather than read from a
  /// DualSense; bit 0 of the flags byte from version 12.
  bool trigger_states_emulated{true};
  /// @brief A second motion flag, bit 1 of the flags byte, from version 16.
  bool second_motion_flag{};
};

/// @brief An encoded controller state payload.
struct EncodedFeedbackState {
  /// @brief The payload: 25 bytes for version 9, 28 from version 12.
  std::vector<std::byte> encoded_payload;
  /// @brief The datagram's size once framed.
  std::size_t datagram_size{};
};

/// @brief What goes in front of a controller state payload.
struct FeedbackStatePacketHeaderInput {
  /// @brief Feedback sequence number.
  std::uint16_t sequence{};
  /// @brief The extended header, when the version needs one.
  std::optional<ExtendedPacketHeader> extended_header{};
};

/// @brief A whole feedback state packet.
struct EncodedFeedbackStatePacket {
  /// @brief The packet from its type byte.
  std::vector<std::byte> encoded_packet;
  /// @brief Its size once framed.
  std::size_t datagram_size{};
  /// @brief Where the state payload starts: 12, or 20 with the extended header.
  std::size_t state_payload_offset{};
  /// @brief The payload's size.
  std::size_t state_payload_size{};
};

/// @brief The digital controls a button record can report.
enum class FeedbackDigitalButton : std::uint8_t {
  /// @brief Cross (wire id 8).
  cross,
  /// @brief Circle (wire id 9).
  moon,
  /// @brief Square (wire id 10).
  box,
  /// @brief Triangle (wire id 11).
  pyramid,
  /// @brief D-pad left (wire id 2).
  dpad_left,
  /// @brief D-pad right (wire id 3).
  dpad_right,
  /// @brief D-pad up (wire id 0).
  dpad_up,
  /// @brief D-pad down (wire id 1).
  dpad_down,
  /// @brief L1 (wire id 4).
  l1,
  /// @brief R1 (wire id 5).
  r1,
  /// @brief L3 (wire id 15).
  l3,
  /// @brief R3 (wire id 16).
  r3,
  /// @brief Options (wire id 12).
  options,
  /// @brief Share or Create (wire id 13).
  share,
  /// @brief Touchpad click (wire id 17).
  touchpad,
  /// @brief PS (wire id 14).
  ps,
  /// @brief Wire id 18, a control the library does not name.
  digital_18,
  /// @brief Wire id 19, a control the library does not name.
  digital_19,
  /// @brief Wire id 20, from version 12.
  digital_20,
  /// @brief Wire id 21, from version 16.
  digital_21,
};

/// @brief The analogue triggers a button record can report.
enum class FeedbackAnalogButton : std::uint8_t {
  /// @brief L2 (wire id 6).
  l2,
  /// @brief R2 (wire id 7).
  r2,
};

/// @brief A digital control was pressed or released.
struct FeedbackDigitalButtonEvent {
  /// @brief Which control.
  FeedbackDigitalButton button{};
  /// @brief Whether it is now down. Ids below 12 are sent as 0xff or 0x00.
  bool pressed{};
};

/// @brief A trigger moved.
struct FeedbackAnalogButtonEvent {
  /// @brief Which trigger.
  FeedbackAnalogButton button{};
  /// @brief Its new position, 0 to 255.
  std::uint8_t value{};
};

/// @brief A touchpad contact went down or up.
struct FeedbackTouchEvent {
  /// @brief Whether it went down; the record's flag bit.
  bool down{};
  /// @brief Contact id; 7 bits.
  std::uint8_t pointer_id{};
  /// @brief X position, clamped to 12 bits.
  std::uint16_t x{};
  /// @brief Y position, clamped to 12 bits.
  std::uint16_t y{};

  /// @brief Compares every field.
  ///
  /// @return Whether both events are the same.
  bool operator==(const FeedbackTouchEvent&) const noexcept = default;
};

/// @brief The controller came or went.
struct FeedbackPresenceEvent {
  /// @brief Whether it is now present (id 31, bit 5).
  bool present{};
};

/// @brief One id and value of a button record.
struct FeedbackButtonEntry {
  /// @brief Wire id: below 12 a byte-valued control, 12 to 21 a one-bit one.
  std::uint8_t id{};
  /// @brief Value: 0 to 255 below id 12, 0 or 1 from id 12.
  std::uint8_t value{};

  /// @brief Compares both fields.
  ///
  /// @return Whether both entries are the same.
  bool operator==(const FeedbackButtonEntry&) const noexcept = default;
};

/// @brief A button record (kind 4) with any number of entries.
///
/// Encoded as the header byte, then one-bit entries (id | value << 5) before
/// byte-valued ones (id, value), each in id order, the presence entry first
/// when present; the last entry's first byte has bit 0x80 set. From version
/// 19 a trailing byte follows.
struct FeedbackButtonRecord {
  /// @brief Presence to report, if any.
  std::optional<bool> presence{};
  /// @brief The entries; at least one unless presence is given, no id twice.
  std::vector<FeedbackButtonEntry> entries{};
  /// @brief The record's flag bit, from version 16.
  bool flag{};
  /// @brief The trailing byte, from version 19; must be 0 before.
  std::uint8_t trailing{};

  /// @brief Compares every field.
  ///
  /// @return Whether both records are the same.
  bool operator==(const FeedbackButtonRecord&) const = default;
};

/// @brief Most entries a kind 0 record holds.
inline constexpr std::size_t feedback_kind0_max_entries = 16;

/// @brief A kind 0 record (version 14 on), passed through as raw bytes: the
/// header, `raw_1`, `raw_2`, a byte of `upper_3 << 5 | entry count`, then the
/// two-byte entries. Its meaning is not known.
struct FeedbackKind0Record {
  /// @brief First raw byte.
  std::uint8_t raw_1{};
  /// @brief Second raw byte.
  std::uint8_t raw_2{};
  /// @brief Three bits stored above the entry count; at most 7.
  std::uint8_t upper_3{};
  /// @brief Up to 16 two-byte entries.
  std::vector<std::array<std::uint8_t, 2>> entries{};

  /// @brief Compares every field.
  ///
  /// @return Whether both records are the same.
  bool operator==(const FeedbackKind0Record&) const = default;
};

/// @brief A legacy kind 1 record (versions 14 to 16): the header and seven
/// raw bytes. Its meaning is not known.
struct FeedbackKind1LegacyRecord {
  /// @brief The record's flag bit.
  bool flag{};
  /// @brief The seven bytes.
  std::array<std::uint8_t, 7> payload{};

  /// @brief Compares every field.
  ///
  /// @return Whether both records are the same.
  bool operator==(const FeedbackKind1LegacyRecord&) const noexcept = default;
};

/// @brief Fields a moded kind 1 record's mask can select.
inline constexpr std::size_t feedback_kind1_field_count = 16;

/// @brief A moded kind 1 record (version 17 on): header, `raw_1`, `raw_2`,
/// mode, eight common bytes, then for mode 1 eight more bytes, for mode 2 a
/// little-endian u16 mask and the selected fields in bit order (fields 12 and
/// 13 four bytes, the rest two). Its meaning is not known.
struct FeedbackKind1Record {
  /// @brief The record's flag bit.
  bool flag{};
  /// @brief First raw byte.
  std::uint8_t raw_1{};
  /// @brief Second raw byte.
  std::uint8_t raw_2{};
  /// @brief 1 or 2.
  std::uint8_t mode{1};
  /// @brief Bytes common to both modes.
  std::array<std::uint8_t, 8> common{};
  /// @brief Mode 1 tail; must be zero in mode 2.
  std::array<std::uint8_t, 8> mode_1_tail{};
  /// @brief Mode 2 fields, empty when absent; must be empty in mode 1.
  std::array<std::vector<std::uint8_t>, feedback_kind1_field_count>
      mode_2_fields{};

  /// @brief Compares every field.
  ///
  /// @return Whether both records are the same.
  bool operator==(const FeedbackKind1Record&) const = default;
};

/// @brief Index of the first four-byte kind 1 field.
inline constexpr std::size_t feedback_kind1_first_wide_field = 12;
/// @brief Index of the second four-byte kind 1 field.
inline constexpr std::size_t feedback_kind1_second_wide_field = 13;
/// @brief Bytes of a wide kind 1 field.
inline constexpr std::size_t feedback_kind1_wide_field_size = 4;
/// @brief Bytes of every other kind 1 field.
inline constexpr std::size_t feedback_kind1_narrow_field_size = 2;

/// @brief Any controller event a feedback history packet can carry.
using FeedbackHistoryEvent =
    std::variant<FeedbackDigitalButtonEvent, FeedbackAnalogButtonEvent,
                 FeedbackTouchEvent, FeedbackPresenceEvent,
                 FeedbackButtonRecord, FeedbackKind0Record,
                 FeedbackKind1LegacyRecord, FeedbackKind1Record>;

/// @brief The events of one feedback history packet.
struct FeedbackHistoryValues {
  /// @brief Feedback sequence number.
  std::uint16_t sequence{};
  /// @brief Events in order; at most 255.
  std::vector<FeedbackHistoryEvent> events;
  /// @brief The extended header, when the version needs one.
  std::optional<ExtendedPacketHeader> extended_header{};
  /// @brief Controller slot, 0 to 3, for every event unless given per event.
  std::uint8_t controller_id{};
  /// @brief One slot per event, or empty to use `controller_id` for all.
  std::vector<std::uint8_t> event_controller_ids{};
};

/// @brief A whole feedback history packet.
struct EncodedFeedbackHistoryPacket {
  /// @brief The packet from its type byte: header, the newest event's
  /// sequence (u16), the event count less one (u8; the receiver adds one
  /// back), 8 zero bytes, then the records.
  std::vector<std::byte> encoded_packet;
  /// @brief Its size once framed.
  std::size_t datagram_size{};
  /// @brief Where the records start: 12, or 20 with the extended header.
  std::size_t event_payload_offset{};
  /// @brief Bytes of records.
  std::size_t event_payload_size{};
  /// @brief Records in the packet.
  std::uint8_t event_count{};
};

/// @brief Takion's binary packet codec. Stateless; every function is static
/// and safe from any thread.
class PacketCodec {
 public:
  /// @brief Works out an audio frame's unit layout from a unit's header.
  ///
  /// @param total Units in the frame.
  /// @param packed The unit shape field.
  /// @param data_size Unit data bytes in this packet.
  /// @param count_only Whether the version encodes only the source count
  ///     (feature av_unit_count_only_encoding); the size then comes from
  ///     `data_size / total`.
  /// @param temporal_redundancy Whether the stream carries temporal audio
  ///     redundancy; with count_only the field then counts the redundant
  ///     units and the rest are sources.
  /// @return The layout; nothing when the counts are zero, inconsistent, or do
  ///     not divide the data evenly.
  [[nodiscard]] static constexpr auto audio_unit_shape(
      std::uint16_t total, std::uint16_t packed, std::size_t data_size,
      bool count_only, bool temporal_redundancy = false) noexcept
      -> std::optional<AudioUnitShape> {
    if (total == 0 || data_size == 0 || data_size % total != 0)
      return std::nullopt;
    const bool temporal = count_only && temporal_redundancy;
    const std::size_t count =
        packed & (temporal ? av_audio_packed_field_mask
                           : av_unit_count_only_source_mask);
    if (temporal && count >= total) return std::nullopt;
    const std::size_t source = count_only ? (temporal ? total - count : count)
                                          : packed & av_unit_source_mask;
    if (source == 0 || source > total) return std::nullopt;
    const std::size_t redundant =
        count_only
            ? total - source
            : (packed >> av_unit_redundant_shift) & av_unit_redundant_mask;
    const std::size_t size =
        count_only ? data_size / total : packed >> av_unit_size_shift;
    if (source + redundant != total || size == 0 || size > av_unit_size_max ||
        size * total != data_size)
      return std::nullopt;
    return AudioUnitShape{static_cast<std::uint16_t>(source),
                          static_cast<std::uint16_t>(redundant),
                          static_cast<std::uint16_t>(size)};
  }

  /// @brief Tells the size of a moded kind 1 field.
  ///
  /// @param bit The field's index.
  /// @return 4 for fields 12 and 13, 2 otherwise.
  static constexpr std::size_t feedback_kind1_field_size(
      std::size_t bit) noexcept {
    return bit == feedback_kind1_first_wide_field ||
                   bit == feedback_kind1_second_wide_field
               ? feedback_kind1_wide_field_size
               : feedback_kind1_narrow_field_size;
  }

  /// @brief Encodes a packet from its parts.
  ///
  /// @param packet The type, extended header and payload.
  /// @param profile The version's layout.
  /// @return The packet bytes.
  /// @retval ParseFailureReason See the overload taking the parts.
  [[nodiscard]] static auto encode_plain_packet(const PlainPacket& packet,
                                                const WireProfile& profile)
      -> Expected<SecretBytes, ParseFailure>;

  /// @brief Encodes a packet from its parts.
  ///
  /// @param type The type byte.
  /// @param extended_header The extended header; must be present exactly when
  ///     the version and family need it.
  /// @param payload The rest.
  /// @param profile The version's layout.
  /// @return The packet bytes.
  /// @retval ParseFailureReason::unknown_packet_family The type names no
  /// family.
  /// @retval ParseFailureReason::unsupported_layout Extended messages before
  /// the
  ///     version supports them.
  /// @retval ParseFailureReason::invalid_field The extended header is present
  ///     when it must not be, or absent when it must.
  [[nodiscard]] static auto encode_plain_packet(
      PacketType type,
      const std::optional<ExtendedPacketHeader>& extended_header,
      std::span<const std::byte> payload, const WireProfile& profile)
      -> Expected<SecretBytes, ParseFailure>;

  /// @brief Describes a sealed AV unit for a source tap.
  ///
  /// @param unit The decoded unit.
  /// @param profile The version's layout.
  /// @param key_position The unit's key position.
  /// @param bytes The whole packet; the description refers to it.
  /// @return The description.
  static auto describe_source_datagram(
      const AvPacketValues& unit, const WireProfile& profile,
      std::uint64_t key_position, std::span<const std::byte> bytes) noexcept
      -> SourceDatagram;

  /// @brief Splits a received packet into type, extended header and payload.
  ///
  /// @param bytes The packet from its type byte.
  /// @param profile The version's layout.
  /// @return The parts; the payload is a view of `bytes`.
  /// @retval ParseFailureReason::truncated It is empty or its extended header
  /// is
  ///     short.
  /// @retval ParseFailureReason::unknown_packet_family The type names no
  /// family.
  /// @retval ParseFailureReason::unsupported_layout Extended messages before
  /// the
  ///     version supports them.
  [[nodiscard]] static auto decode_plain_packet(
      std::span<const std::byte> bytes, const WireProfile& profile)
      -> Expected<PlainPacket, ParseFailure>;

  /// @brief Reads a video or audio unit's header for the version's layout.
  ///
  /// @param packet The split packet.
  /// @param profile The version's layout.
  /// @return The unit.
  /// @retval ParseFailureReason::unsupported_layout Not an AV family, or audio
  ///     with bit 0x10 before version 15.
  /// @retval ParseFailureReason::truncated The header is short; from version 9
  ///     the payload must exceed the reference header size.
  [[nodiscard]] static auto decode_av_packet(const PlainPacket& packet,
                                             const WireProfile& profile)
      -> Expected<AvPacketValues, ParseFailure>;

  /// @brief Reads a video or audio unit's header as decode_av_packet() does,
  /// leaving its data in the packet instead of copying it.
  ///
  /// @param packet The split packet; the data view borrows its payload.
  /// @param profile The version's layout.
  /// @return The unit with empty data, and a view of the data.
  /// @retval ParseFailureReason The failures of decode_av_packet().
  [[nodiscard]] static auto decode_av_header(const PlainPacket& packet,
                                             const WireProfile& profile)
      -> Expected<AvPacketHeader, ParseFailure>;

  /// @brief Encodes a Senkusha probe in the version 7 AV layout.
  ///
  /// @param values The header fields and payload.
  /// @param profile The layout to encode with; must use the version 7 AV
  /// layout.
  /// @param envelope Framing, for the size reported.
  /// @return The bytes after the type byte and the datagram size.
  /// @retval ParseFailureReason::unsupported_layout The profile is not
  /// version 7.
  /// @retval ParseFailureReason::invalid_field A count or index is out of
  /// range.
  [[nodiscard]] static auto encode_senkusha_probe(
      const SenkushaProbeValues& values, const WireProfile& profile,
      const PacketEnvelope& envelope)
      -> Expected<EncodedSenkushaProbe, ParseFailure>;

  /// @brief Encodes a congestion report.
  ///
  /// @param values The fields.
  /// @param profile The version's layout.
  /// @param envelope Framing, for the size reported.
  /// @return The 14 bytes after the type byte and the datagram size.
  /// @retval ParseFailureReason::unsupported_layout The profile's congestion
  ///     layout is unknown.
  [[nodiscard]] static auto encode_congestion(const CongestionValues& values,
                                              const WireProfile& profile,
                                              const PacketEnvelope& envelope)
      -> Expected<EncodedCongestion, ParseFailure>;

  /// @brief Encodes a controller state payload.
  ///
  /// Layout: `0xa0 | controller`; gyro x, y, z as u16 little-endian scaled from
  /// ±30 to 0..65535; acceleration likewise from ±5; the quaternion packed into
  /// a u32 little-endian (index of the largest component << 1 | its sign, then
  /// the other three, each clamped to ±1/√2 and scaled to 9 bits, from bit 3);
  /// the four stick axes as i16 big-endian; from version 12 the two trigger
  /// states and the flags byte.
  ///
  /// @param values The state.
  /// @param profile The version's layout.
  /// @param envelope Framing, for the size reported.
  /// @return The payload and the datagram size.
  /// @retval ParseFailureReason::invalid_field The controller slot exceeds 3.
  /// @retval ParseFailureReason::unsupported_layout The second motion flag
  ///     before version 16.
  [[nodiscard]] static auto encode_feedback_state(
      const FeedbackStateValues& values, const WireProfile& profile,
      const PacketEnvelope& envelope)
      -> Expected<EncodedFeedbackState, ParseFailure>;

  /// @brief Wraps a controller state payload in its packet header.
  ///
  /// @param state The payload.
  /// @param header The sequence and extended header.
  /// @param profile The version's layout.
  /// @param envelope Framing, for the size reported.
  /// @return The packet and where its payload lies.
  /// @retval ParseFailureReason::invalid_length The payload has the wrong size
  ///     for the version.
  /// @retval ParseFailureReason::invalid_field The payload does not start with
  ///     0xa0 to 0xa3, or the extended header disagrees with the version.
  [[nodiscard]] static auto assemble_feedback_state_packet(
      const EncodedFeedbackState& state,
      const FeedbackStatePacketHeaderInput& header, const WireProfile& profile,
      const PacketEnvelope& envelope)
      -> Expected<EncodedFeedbackStatePacket, ParseFailure>;

  /// @brief Encodes a feedback history packet.
  ///
  /// @param values The events and header fields.
  /// @param profile The version's layout; decides which records and ids exist.
  /// @param envelope Framing, for the size reported.
  /// @return The packet and where its records lie.
  /// @retval ParseFailureReason::invalid_field A slot exceeds 3, the per-event
  ///     slots do not match the events, the extended header disagrees with the
  ///     version, or a record is malformed.
  /// @retval ParseFailureReason::invalid_length More than 255 events.
  /// @retval ParseFailureReason::unsupported_layout A record or id the version
  ///     does not have.
  [[nodiscard]] static auto encode_feedback_history_packet(
      const FeedbackHistoryValues& values, const WireProfile& profile,
      const PacketEnvelope& envelope)
      -> Expected<EncodedFeedbackHistoryPacket, ParseFailure>;

 private:
  /// @brief Tells whether a family may be used with a version.
  ///
  /// @param profile The version's layout.
  /// @param type The type byte.
  /// @return Whether the family is known, and for extended messages supported.
  static constexpr bool family_enabled(const WireProfile& profile,
                                       PacketType type) noexcept;
  /// @brief Describes an extended header that is present when it must not be,
  /// or absent when it must.
  ///
  /// @param type The type byte.
  /// @param required Whether the version needs one.
  /// @param header What was given.
  /// @return The failure.
  static auto extended_header_failure(
      PacketType type, bool required,
      const std::optional<ExtendedPacketHeader>& header) noexcept
      -> ParseFailure;
  /// @brief Writes the type byte and any extended header.
  ///
  /// @param output Where to append.
  /// @param type The type byte.
  /// @param extended_header The extended header, if any.
  static void append_header(
      bytes::Appender& output, PacketType type,
      const std::optional<ExtendedPacketHeader>& extended_header);
  /// @brief Scales a clamped value linearly onto 0..65535.
  ///
  /// @param value The value.
  /// @param minimum The low clamp.
  /// @param maximum The high clamp.
  /// @return The quantised value.
  static auto scaled_motion(float value, float minimum, float maximum) noexcept
      -> std::uint16_t;
  /// @brief Packs a unit quaternion as smallest-three in 32 bits.
  ///
  /// @param quaternion x, y, z, w.
  /// @return The packed word.
  static auto pack_quaternion(const std::array<float, 4>& quaternion) noexcept
      -> std::uint32_t;
  /// @brief Reads a video slice descriptor.
  ///
  /// @param extension The 3 descriptor bytes.
  /// @return The descriptor.
  static auto slice_extension(std::span<const std::byte> extension) noexcept
      -> VideoSliceExtension;

  /// @brief Bytes before a feedback packet's records: type, sequence, count and
  /// 8 reserved bytes.
  static constexpr std::size_t feedback_plain_header_size = 12;
  /// @brief The same with the 8-byte extended header.
  static constexpr std::size_t feedback_extended_header_size = 20;
  /// @brief Reserved zero bytes in a feedback packet's header.
  static constexpr std::size_t feedback_reserved_size = 8;
  /// @brief Reserved bytes in an AV header.
  static constexpr std::size_t av_reserved_size = 8;
  /// @brief Reserved zero bytes in a congestion report.
  static constexpr std::size_t congestion_reserved_size = 8;
  /// @brief Largest quantised motion value.
  static constexpr float motion_quantisation_max = 0xffff;
  /// @brief Largest 9-bit quaternion component.
  static constexpr std::uint32_t quaternion_component_max = 0x1ff;
};

}  // namespace libchiaking::net::protocol::takion
