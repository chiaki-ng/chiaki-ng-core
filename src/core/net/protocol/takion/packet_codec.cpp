#include "src/core/net/protocol/takion/detail/packet_codec.hpp"

#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/detail/feedback_record_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_types.hpp"
#include "src/core/net/protocol/takion/result.hpp"
#include "src/core/utils/bytes/byte_io.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::takion {

constexpr bool PacketCodec::family_enabled(const WireProfile& profile,
                                           PacketType type) noexcept {
  return type.is_known() &&
         (type.family() != PacketFamily::extended_message ||
          profile.features().contains(Feature::extended_messages));
}

auto PacketCodec::extended_header_failure(
    PacketType type, bool required,
    const std::optional<ExtendedPacketHeader>& header) noexcept
    -> ParseFailure {
  return ParseFailure::at(type, ParseFailureReason::invalid_field, 1,
                          required ? extended_header_size : 0,
                          header ? extended_header_size : 0);
}

void PacketCodec::append_header(
    bytes::Appender& output, PacketType type,
    const std::optional<ExtendedPacketHeader>& extended_header) {
  output.u8(type.raw);
  if (extended_header) {
    output.u32_be(extended_header->timestamp);
    output.u32_be(extended_header->counter);
  }
}

auto PacketCodec::encode_plain_packet(const PlainPacket& packet,
                                      const WireProfile& profile)
    -> Expected<SecretBytes, ParseFailure> {
  return encode_plain_packet(packet.type, packet.extended_header,
                             packet.payload, profile);
}

auto PacketCodec::encode_plain_packet(
    PacketType type, const std::optional<ExtendedPacketHeader>& extended_header,
    std::span<const std::byte> payload, const WireProfile& profile)
    -> Expected<SecretBytes, ParseFailure> {
  if (!type.is_known())
    return unexpected(
        ParseFailure::of(type, ParseFailureReason::unknown_packet_family));
  if (!family_enabled(profile, type))
    return unexpected(
        ParseFailure::of(type, ParseFailureReason::unsupported_layout));
  const bool requires_extended_header = profile.uses_extended_header(type);
  if (requires_extended_header != extended_header.has_value())
    return unexpected(extended_header_failure(type, requires_extended_header,
                                              extended_header));

  SecretBytes bytes;
  bytes.reserve(profile.packet_header_size(type) + payload.size());
  bytes.push_back(std::byte{type.raw});
  if (extended_header) {
    bytes::append_be(bytes, extended_header->timestamp);
    bytes::append_be(bytes, extended_header->counter);
  }
  bytes.append(payload);
  return bytes;
}

auto PacketCodec::decode_plain_packet(std::span<const std::byte> bytes,
                                      const WireProfile& profile)
    -> Expected<PlainPacket, ParseFailure> {
  bytes::Reader reader{bytes};
  const auto raw_type = reader.u8();
  if (!raw_type)
    return unexpected(
        ParseFailure::at(PacketType{}, ParseFailureReason::truncated, 0, 1, 0));
  const PacketType type{raw_type.value()};
  if (!type.is_known())
    return unexpected(
        ParseFailure::of(type, ParseFailureReason::unknown_packet_family));
  if (!family_enabled(profile, type))
    return unexpected(
        ParseFailure::of(type, ParseFailureReason::unsupported_layout));

  std::optional<ExtendedPacketHeader> header;
  if (profile.uses_extended_header(type)) {
    const auto timestamp = reader.u32_be();
    if (!timestamp)
      return unexpected(ParseFailure::short_by(type, timestamp.error()));
    const auto counter = reader.u32_be();
    if (!counter)
      return unexpected(ParseFailure::short_by(type, counter.error()));
    header = ExtendedPacketHeader{timestamp.value(), counter.value()};
  }

  return PlainPacket{type, header, bytes.subspan(reader.offset())};
}

auto PacketCodec::scaled_motion(float value, float minimum,
                                float maximum) noexcept -> std::uint16_t {
  return static_cast<std::uint16_t>(
      motion_quantisation_max *
      (std::clamp(value, minimum, maximum) - minimum) / (maximum - minimum));
}

auto PacketCodec::pack_quaternion(
    const std::array<float, 4>& quaternion) noexcept -> std::uint32_t {
  const auto largest = std::ranges::max_element(
      quaternion, {}, [](float component) { return std::fabs(component); });
  const auto largest_index =
      static_cast<std::uint32_t>(std::distance(quaternion.begin(), largest));
  std::uint32_t packed = largest_index << 1U;
  if (std::signbit(*largest)) packed |= 1U;
  constexpr double component_limit = 0.70710678118654752440;
  std::uint32_t component_shift = 3;
  for (std::size_t index = 0; index < quaternion.size(); ++index) {
    if (index == largest_index) continue;
    float component = quaternion[index];
    if (component < -component_limit)
      component = static_cast<float>(-component_limit);
    if (component > component_limit)
      component = static_cast<float>(component_limit);
    component = static_cast<float>(component + component_limit);
    component = static_cast<float>(
        component * (static_cast<float>(quaternion_component_max) /
                     (2.0F * component_limit)));
    packed |= static_cast<std::uint32_t>(component) << component_shift;
    component_shift += 9;
  }
  return packed;
}

auto PacketCodec::encode_feedback_state(const FeedbackStateValues& input,
                                        const WireProfile& profile,
                                        const PacketEnvelope& envelope)
    -> Expected<EncodedFeedbackState, ParseFailure> {
  constexpr auto type =
      PacketType{static_cast<std::uint8_t>(PacketFamily::feedback_state)};
  if (input.controller_id > 3)
    return unexpected(
        ParseFailure::of(type, ParseFailureReason::invalid_field));
  const bool independent_motion_flags =
      profile.version() >= feedback_motion_flag_bits_version;
  if (input.second_motion_flag && !independent_motion_flags)
    return unexpected(
        ParseFailure::of(type, ParseFailureReason::unsupported_layout));

  auto values = input;
  for (auto* gyro : {&values.gyro_x, &values.gyro_y, &values.gyro_z})
    if (!std::isfinite(*gyro)) *gyro = 0.0F;
  if (!std::isfinite(values.accel_x) || !std::isfinite(values.accel_y) ||
      !std::isfinite(values.accel_z)) {
    values.accel_x = 0.0F;
    values.accel_y = 1.0F;
    values.accel_z = 0.0F;
  }
  if (!std::isfinite(values.orient_x) || !std::isfinite(values.orient_y) ||
      !std::isfinite(values.orient_z) || !std::isfinite(values.orient_w)) {
    values.orient_x = 0.0F;
    values.orient_y = 0.0F;
    values.orient_z = 0.0F;
    values.orient_w = 1.0F;
  }

  constexpr float gyro_min = -30.0F;
  constexpr float gyro_max = 30.0F;
  constexpr float accel_min = -5.0F;
  constexpr float accel_max = 5.0F;
  const bool version_12 =
      profile.feedback_state_layout() == FeedbackStateLayout::version_12;

  std::vector<std::byte> encoded;
  encoded.reserve(version_12 ? feedback_state_v12_payload_size
                             : feedback_state_v9_payload_size);
  bytes::Appender output{encoded};
  output.u8(static_cast<std::uint8_t>(0xa0U | values.controller_id));
  for (const auto value : {values.gyro_x, values.gyro_y, values.gyro_z})
    output.u16_le(scaled_motion(value, gyro_min, gyro_max));
  for (const auto value : {values.accel_x, values.accel_y, values.accel_z})
    output.u16_le(scaled_motion(value, accel_min, accel_max));
  output.u32_le(pack_quaternion(
      {values.orient_x, values.orient_y, values.orient_z, values.orient_w}));
  for (const auto value :
       {values.left_x, values.left_y, values.right_x, values.right_y})
    output.i16_be(value);
  if (version_12) {
    constexpr std::uint8_t trigger_states_emulated_bit = 0x01;
    constexpr std::uint8_t second_motion_flag_bit = 0x02;
    output.u8(values.left_trigger_state);
    output.u8(values.right_trigger_state);
    output.u8(static_cast<std::uint8_t>(
        (values.trigger_states_emulated ? trigger_states_emulated_bit : 0U) |
        (independent_motion_flags && values.second_motion_flag
             ? second_motion_flag_bit
             : 0U)));
  }

  const auto datagram_size = EnvelopeCodec::overhead(envelope) +
                             profile.packet_header_size(type) + encoded.size();
  return EncodedFeedbackState{std::move(encoded), datagram_size};
}

auto PacketCodec::assemble_feedback_state_packet(
    const EncodedFeedbackState& state,
    const FeedbackStatePacketHeaderInput& header, const WireProfile& profile,
    const PacketEnvelope& envelope)
    -> Expected<EncodedFeedbackStatePacket, ParseFailure> {
  constexpr auto type =
      PacketType{static_cast<std::uint8_t>(PacketFamily::feedback_state)};
  const auto expected_payload_size =
      profile.feedback_state_layout() == FeedbackStateLayout::version_12
          ? feedback_state_v12_payload_size
          : feedback_state_v9_payload_size;
  if (state.encoded_payload.size() != expected_payload_size)
    return unexpected(ParseFailure::at(type, ParseFailureReason::invalid_length,
                                       0, expected_payload_size,
                                       state.encoded_payload.size()));
  if ((std::to_integer<std::uint8_t>(state.encoded_payload.front()) & 0xfcU) !=
      0xa0U)
    return unexpected(
        ParseFailure::of(type, ParseFailureReason::invalid_field));
  const bool requires_extended_header = profile.uses_extended_header(type);
  if (requires_extended_header != header.extended_header.has_value())
    return unexpected(extended_header_failure(type, requires_extended_header,
                                              header.extended_header));

  constexpr std::uint8_t feedback_state_event_count = 1;
  constexpr std::uint8_t feedback_state_count_byte =
      feedback_state_event_count - 1;
  const std::size_t state_payload_offset = requires_extended_header
                                               ? feedback_extended_header_size
                                               : feedback_plain_header_size;
  std::vector<std::byte> packet;
  packet.reserve(state_payload_offset + state.encoded_payload.size());
  bytes::Appender output{packet};
  append_header(output, type, header.extended_header);
  output.u16_be(header.sequence);
  output.u8(feedback_state_count_byte);
  output.zeros(feedback_reserved_size);
  output.bytes(state.encoded_payload);

  const auto datagram_size = packet.size() + EnvelopeCodec::overhead(envelope);
  return EncodedFeedbackStatePacket{
      std::move(packet),
      datagram_size,
      state_payload_offset,
      state.encoded_payload.size(),
  };
}

auto PacketCodec::encode_feedback_history_packet(
    const FeedbackHistoryValues& values, const WireProfile& profile,
    const PacketEnvelope& envelope)
    -> Expected<EncodedFeedbackHistoryPacket, ParseFailure> {
  constexpr auto type =
      PacketType{static_cast<std::uint8_t>(PacketFamily::feedback_history)};
  const auto valid_controller = [](std::uint8_t id) { return id <= 3; };
  if (!valid_controller(values.controller_id) ||
      (!values.event_controller_ids.empty() &&
       (values.event_controller_ids.size() != values.events.size() ||
        !std::ranges::all_of(values.event_controller_ids, valid_controller))))
    return unexpected(
        ParseFailure::of(type, ParseFailureReason::invalid_field));
  const bool requires_extended_header = profile.uses_extended_header(type);
  if (requires_extended_header != values.extended_header.has_value())
    return unexpected(extended_header_failure(type, requires_extended_header,
                                              values.extended_header));
  if (values.events.size() > std::numeric_limits<std::uint8_t>::max())
    return unexpected(ParseFailure::at(
        type, ParseFailureReason::invalid_length, 3,
        std::numeric_limits<std::uint8_t>::max(), values.events.size()));

  const std::size_t event_payload_offset = requires_extended_header
                                               ? feedback_extended_header_size
                                               : feedback_plain_header_size;
  std::vector<std::byte> packet;
  bytes::Appender output{packet};
  append_header(output, type, values.extended_header);
  output.u16_be(values.sequence);
  output.u8(static_cast<std::uint8_t>(
      std::max<std::size_t>(values.events.size(), 1) - 1));
  output.zeros(feedback_reserved_size);
  for (std::size_t index = 0; index < values.events.size(); ++index) {
    const auto controller_id = values.event_controller_ids.empty()
                                   ? values.controller_id
                                   : values.event_controller_ids[index];
    const auto record = FeedbackRecordCodec::encode(values.events[index],
                                                    controller_id, profile);
    if (!record)
      return unexpected(ParseFailure::at(
          type, record.error().reason, feedback_plain_header_size,
          record.error().needed, record.error().available));
    output.bytes(record.value());
  }

  const auto event_payload_size = packet.size() - event_payload_offset;
  const auto datagram_size = packet.size() + EnvelopeCodec::overhead(envelope);
  return EncodedFeedbackHistoryPacket{
      std::move(packet),
      datagram_size,
      event_payload_offset,
      event_payload_size,
      static_cast<std::uint8_t>(values.events.size()),
  };
}

auto PacketCodec::decode_av_packet(const PlainPacket& packet,
                                   const WireProfile& profile)
    -> Expected<AvPacketValues, ParseFailure> {
  return decode_av_header(packet, profile).transform([](AvPacketHeader header) {
    header.values.data.assign(header.data.begin(), header.data.end());
    return std::move(header.values);
  });
}

auto PacketCodec::decode_av_header(const PlainPacket& packet,
                                   const WireProfile& profile)
    -> Expected<AvPacketHeader, ParseFailure> {
  const auto family = packet.type.family();
  if (family != PacketFamily::video &&
      family != PacketFamily::audio_and_haptics)
    return unexpected(
        ParseFailure::of(packet.type, ParseFailureReason::unsupported_layout));
  const bool video = family == PacketFamily::video;
  const auto layout = profile.av_layout();
  const bool version7 = layout == AvLayout::version_7;
  const bool has_audio_tag =
      !video && !version7 && layout != AvLayout::versions_9_to_10;
  const bool packed_audio_modes = layout == AvLayout::versions_15_to_19 ||
                                  layout == AvLayout::versions_20_to_21 ||
                                  layout == AvLayout::versions_22_to_23 ||
                                  layout == AvLayout::versions_24_to_25;
  const bool video_dynamic_range = video && packed_audio_modes;
  const bool video_presentation =
      video && layout == AvLayout::versions_24_to_25;
  const bool masked_video_total =
      video && (layout == AvLayout::versions_22_to_23 ||
                layout == AvLayout::versions_24_to_25);

  if (!version7) {
    const std::size_t minimum =
        (video           ? reference_video_header_size
         : has_audio_tag ? reference_tagged_audio_header_size
                         : reference_audio_header_size) +
        1U;
    if (packet.payload.size() < minimum)
      return unexpected(ParseFailure::at(
          packet.type, ParseFailureReason::truncated, packet.payload.size(),
          minimum, packet.payload.size()));
  }
  bytes::Reader reader{packet.payload};
  const auto truncated = [&packet](bytes::Shortfall failure) {
    return unexpected(ParseFailure::short_by(packet.type, failure));
  };
  const auto packet_index = reader.u16_be();
  if (!packet_index) return truncated(packet_index.error());
  const auto frame_index = reader.u16_be();
  if (!frame_index) return truncated(frame_index.error());
  const auto packed = reader.u32_be();
  if (!packed) return truncated(packed.error());
  const auto codec = reader.u8();
  if (!codec) return truncated(codec.error());
  if (auto reserved = reader.skip(av_reserved_size); !reserved)
    return truncated(reserved.error());

  std::uint16_t video_word{};
  std::uint8_t adaptive_stream_index{};
  bool dynamic_range_flag{};
  bool suppress_presentation{};
  std::optional<VideoSliceExtension> slice;
  const bool uses_nalu = (packet.type.flags() & nalu_info_structs_flag) != 0;
  if (video) {
    const auto word = reader.u16_be();
    if (!word) return truncated(word.error());
    video_word = word.value();
    const auto adaptive = reader.u8();
    if (!adaptive) return truncated(adaptive.error());
    adaptive_stream_index = adaptive.value() >> adaptive_stream_index_shift;
    dynamic_range_flag = video_dynamic_range &&
                         (adaptive.value() & video_dynamic_range_flag_bit) != 0;
    suppress_presentation =
        video_presentation &&
        (adaptive.value() & video_suppress_presentation_bit) != 0;
    if (uses_nalu) {
      const auto extension = reader.take(nalu_info_structs_size);
      if (!extension) return truncated(extension.error());
      slice = slice_extension(extension.value());
    }
  } else if (version7) {
    if (uses_nalu) {
      if (auto skipped = reader.skip(nalu_info_structs_size); !skipped)
        return truncated(skipped.error());
    }
  } else {
    if (uses_nalu && !packed_audio_modes)
      return unexpected(ParseFailure::of(
          packet.type, ParseFailureReason::unsupported_layout));
    if (auto skipped = reader.skip(uses_nalu ? 2 : 1); !skipped)
      return truncated(skipped.error());
  }
  std::optional<std::uint8_t> av_tag;
  std::uint8_t audio_profile{};
  if (has_audio_tag) {
    const auto tag = reader.u8();
    if (!tag) return truncated(tag.error());
    av_tag =
        packed_audio_modes
            ? static_cast<std::uint8_t>(tag.value() & av_audio_metadata_mask)
            : tag.value();
    if (packed_audio_modes)
      audio_profile =
          static_cast<std::uint8_t>(tag.value() >> av_audio_profile_shift);
  }
  const auto data = packet.payload.subspan(reader.offset());

  const auto word = packed.value();
  const bool packed_video = video || version7;
  const auto unit_index =
      packed_video ? (word >> av_unit_index_shift) & av_unit_index_mask
                   : (word >> 24) & 0xffU;
  const auto video_total =
      ((word >> av_units_total_shift) & av_units_total_mask) + 1U;
  const auto total =
      packed_video ? (masked_video_total ? video_total & av_units_total_mask
                                         : video_total)
                   : ((word >> 16) & 0xffU) + 1U;
  const auto fec = packed_video         ? word & av_units_fec_mask
                   : packed_audio_modes ? word & av_audio_packed_field_mask
                                        : word & 0xffffU;
  const auto audio_mode =
      !packed_video && packed_audio_modes
          ? (word >> av_audio_mode_shift) & av_audio_mode_mask
          : 0U;

  return AvPacketHeader{AvPacketValues{packet.type,
                                       uses_nalu,
                                       packet_index.value(),
                                       frame_index.value(),
                                       static_cast<std::uint16_t>(unit_index),
                                       static_cast<std::uint16_t>(total),
                                       static_cast<std::uint16_t>(fec),
                                       codec.value(),
                                       video_word,
                                       adaptive_stream_index,
                                       {},
                                       av_tag && *av_tag >= 2 && *av_tag < 6,
                                       av_tag,
                                       static_cast<std::uint8_t>(audio_mode),
                                       audio_profile,
                                       slice,
                                       dynamic_range_flag,
                                       suppress_presentation},
                        data};
}

auto PacketCodec::slice_extension(std::span<const std::byte> extension) noexcept
    -> VideoSliceExtension {
  std::array<std::byte, sizeof(std::uint32_t)> word{};
  std::ranges::copy(extension, std::next(word.begin()));
  const auto bits = bytes::load_be<std::uint32_t>(word);
  return VideoSliceExtension{static_cast<std::uint16_t>(bits >> 13U),
                             static_cast<std::uint16_t>((bits >> 2U) & 0x7ffU),
                             (bits & 0x2U) != 0, (bits & 0x1U) != 0};
}

auto PacketCodec::describe_source_datagram(
    const AvPacketValues& unit, const WireProfile& profile,
    std::uint64_t key_position, std::span<const std::byte> bytes) noexcept
    -> SourceDatagram {
  SourceDatagram described;
  described.key_position = key_position;
  described.bytes = bytes;
  described.sealed_size = unit.data.size();
  described.unit_index = unit.unit_index;
  described.codec = unit.codec;
  if (unit.type.family() == PacketFamily::video) {
    described.media = SourceMedia::video;
    described.unit_total = unit.units_in_frame_total;
    described.unit_redundant = unit.units_in_frame_fec;
    described.stream_index = unit.adaptive_stream_index;
    return described;
  }
  described.media = SourceMedia::audio;
  described.auxiliary = unit.is_haptics;
  const auto shape = audio_unit_shape(
      unit.units_in_frame_total, unit.units_in_frame_fec, unit.data.size(),
      profile.features().contains(Feature::av_unit_count_only_encoding),
      !unit.is_haptics && unit.audio_mode == 2);
  if (shape) {
    described.unit_total = unit.units_in_frame_total;
    described.unit_redundant = shape->redundant;
    described.unit_size = shape->size;
  }
  return described;
}

auto PacketCodec::encode_senkusha_probe(const SenkushaProbeValues& values,
                                        const WireProfile& profile,
                                        const PacketEnvelope& envelope)
    -> Expected<EncodedSenkushaProbe, ParseFailure> {
  if (profile.av_layout() != AvLayout::version_7)
    return unexpected(
        ParseFailure::of(PacketType{}, ParseFailureReason::unsupported_layout));

  const auto family =
      values.video ? PacketFamily::video : PacketFamily::audio_and_haptics;
  const PacketType type{static_cast<std::uint8_t>(
      std::to_underlying(family) |
      (values.uses_nalu_info_structs ? nalu_info_structs_flag : 0U))};
  if (values.units_in_frame_total == 0 ||
      values.units_in_frame_total > max_units_in_frame ||
      values.units_in_frame_fec > av_units_fec_mask ||
      values.unit_index > max_unit_index ||
      values.adaptive_stream_index > max_adaptive_stream_index)
    return unexpected(
        ParseFailure::of(type, ParseFailureReason::invalid_field));

  const std::uint32_t packed =
      static_cast<std::uint32_t>(values.units_in_frame_fec) |
      (static_cast<std::uint32_t>(values.units_in_frame_total - 1)
       << av_units_total_shift) |
      (static_cast<std::uint32_t>(values.unit_index) << av_unit_index_shift);

  std::vector<std::byte> encoded;
  encoded.reserve(
      (values.video ? video_av_header_size_v7 : audio_av_header_size_v7) +
      (values.uses_nalu_info_structs ? nalu_info_structs_size : 0) +
      values.payload.size());
  bytes::Appender output{encoded};
  output.u16_be(values.packet_index);
  output.u16_be(values.frame_index);
  output.u32_be(packed);
  output.u8(values.codec);
  output.zeros(av_reserved_size);
  if (values.video) {
    output.u16_be(values.video_word);
    output.u8(static_cast<std::uint8_t>(values.adaptive_stream_index
                                        << adaptive_stream_index_shift));
  }
  if (values.uses_nalu_info_structs) output.zeros(nalu_info_structs_size);
  output.bytes(values.payload);

  const auto datagram_size = EnvelopeCodec::overhead(envelope) +
                             profile.packet_header_size(type) + encoded.size();
  return EncodedSenkushaProbe{family, std::move(encoded), datagram_size};
}

auto PacketCodec::encode_congestion(const CongestionValues& values,
                                    const WireProfile& profile,
                                    const PacketEnvelope& envelope)
    -> Expected<EncodedCongestion, ParseFailure> {
  constexpr auto type =
      PacketType{static_cast<std::uint8_t>(PacketFamily::congestion)};
  if (profile.congestion_layout() != CongestionLayout::standard)
    return unexpected(
        ParseFailure::of(type, ParseFailureReason::unsupported_layout));

  std::vector<std::byte> encoded;
  encoded.reserve(congestion_packet_size - packet_type_size);
  bytes::Appender output{encoded};
  output.u16_be(values.word);
  output.u16_be(values.server_sent);
  output.u16_be(values.lost);
  output.zeros(congestion_reserved_size);

  const auto datagram_size = EnvelopeCodec::overhead(envelope) +
                             profile.packet_header_size(type) + encoded.size();
  return EncodedCongestion{std::move(encoded), datagram_size};
}

}  // namespace libchiaking::net::protocol::takion
