#include "src/core/net/protocol/senkusha/probe.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/limits.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::senkusha {
using takion::audio_av_header_size_v7;
using takion::nalu_info_structs_size;
using takion::packet_type_size;
using takion::video_av_header_size_v7;
auto ProbeCodec::build_local_rtt_probe(std::uint16_t test_index,
                                       std::uint16_t unit_index,
                                       std::uint32_t tag,
                                       const takion::WireProfile& profile,
                                       const takion::PacketEnvelope& envelope)
    -> Expected<takion::EncodedSenkushaProbe, Error> {
  if (profile.packet_header_size(audio_packet_type) != packet_type_size)
    return unexpected(Error::invalid_configuration);

  takion::SenkushaProbeValues values;
  values.frame_index = test_index;
  values.unit_index = unit_index;
  values.units_in_frame_total = takion::max_units_in_frame;
  values.codec = probe_codec_marker;
  values.payload.resize(local_probe_payload_size);
  bytes::store_be<std::uint32_t>(
      std::span{values.payload}.subspan(probe_tag_offset, sizeof(tag)), tag);

  const auto outgoing =
      takion::resolve_takion_wire_profile(takion::takion_bootstrap_version);
  auto encoded = takion::PacketCodec::encode_senkusha_probe(
      values, outgoing.value(), envelope);
  if (!encoded) return unexpected(Error::malformed_response);
  if (encoded.value().datagram_size !=
      minimum_datagram_size + takion::EnvelopeCodec::overhead(envelope))
    return unexpected(Error::arithmetic_overflow);
  return std::move(encoded).value();
}

auto ProbeCodec::build_local_outbound_mtu_probe(
    std::uint32_t candidate_network_mtu, std::uint16_t attempt_index,
    std::uint32_t tag, const takion::WireProfile& profile,
    const takion::PacketEnvelope& envelope, const MtuPolicy& mtu_policy)
    -> Expected<takion::EncodedSenkushaProbe, Error> {
  if (attempt_index > takion::max_unit_index)
    return unexpected(Error::arithmetic_overflow);
  if (profile.packet_header_size(audio_packet_type) != packet_type_size)
    return unexpected(Error::invalid_configuration);
  if (candidate_network_mtu < mtu_policy.network_overhead)
    return unexpected(Error::arithmetic_overflow);
  if (candidate_network_mtu > mtu_policy.upper_bound)
    return unexpected(Error::invalid_configuration);

  const auto datagram_size =
      candidate_network_mtu - mtu_policy.network_overhead;
  if (datagram_size < mtu_av_header_size + probe_minimum_payload_size)
    return unexpected(Error::invalid_configuration);
  const auto payload_size =
      static_cast<std::size_t>(datagram_size - mtu_av_header_size);

  takion::SenkushaProbeValues values;
  values.frame_index = 0;
  values.unit_index = attempt_index;
  values.units_in_frame_total = takion::max_units_in_frame;
  values.codec = probe_codec_marker;
  values.payload.assign(payload_size, std::byte{0});
  bytes::store_be<std::uint32_t>(
      std::span{values.payload}.subspan(probe_tag_offset, sizeof(tag)), tag);
  constexpr std::array padding{std::byte{'C'}, std::byte{'H'}, std::byte{'I'},
                               std::byte{'A'}, std::byte{'K'}, std::byte{'I'}};
  for (std::size_t index = probe_minimum_payload_size; index < payload_size;
       ++index) {
    values.payload[index] =
        padding[(index - probe_minimum_payload_size) % padding.size()];
  }

  const auto outgoing =
      takion::resolve_takion_wire_profile(takion::takion_bootstrap_version);
  auto encoded = takion::PacketCodec::encode_senkusha_probe(
      values, outgoing.value(), envelope);
  if (!encoded) return unexpected(Error::malformed_response);
  if (encoded.value().datagram_size !=
      datagram_size + takion::EnvelopeCodec::overhead(envelope))
    return unexpected(Error::arithmetic_overflow);
  return std::move(encoded).value();
}

auto ProbeCodec::classify_local_rtt_pong(
    takion::SenkushaProbeMedia media,
    std::span<const std::byte> encoded_av_payload, bool uses_nalu_info_structs,
    const takion::WireProfile& profile, const ProbePolicy& policy,
    std::uint16_t expected_test_index, std::uint16_t expected_unit_index,
    std::uint32_t expected_tag) -> PongExpected {
  if (profile.av_layout() != takion::AvLayout::version_7)
    return unexpected(Error::malformed_response);
  if (media != takion::SenkushaProbeMedia::audio) return PongClassification{};
  const auto header_size =
      audio_av_header_size_v7 +
      (uses_nalu_info_structs ? nalu_info_structs_size : 0);
  if (encoded_av_payload.size() < header_size)
    return unexpected(Error::malformed_response);

  const auto payload_size = encoded_av_payload.size() - header_size;
  if (payload_size < policy.minimum_payload_size ||
      policy.tag_offset > payload_size ||
      payload_size - policy.tag_offset < sizeof(std::uint32_t))
    return unexpected(Error::malformed_response);

  const auto frame_index =
      bytes::load_be<std::uint16_t>(encoded_av_payload.subspan(2));
  const auto packed =
      bytes::load_be<std::uint32_t>(encoded_av_payload.subspan(4));
  const auto unit_index =
      static_cast<std::uint16_t>(packed >> takion::av_unit_index_shift);
  const auto tag = bytes::load_be<std::uint32_t>(
      encoded_av_payload.subspan(header_size + policy.tag_offset));
  if (frame_index != expected_test_index ||
      (policy.require_unit_index && unit_index != expected_unit_index) ||
      tag != expected_tag) {
    return PongClassification{ProbeStatus::unrelated, tag};
  }
  return PongClassification{ProbeStatus::matched, tag};
}

auto ProbeCodec::classify_local_inbound_mtu_response(
    takion::SenkushaProbeMedia media,
    std::span<const std::byte> encoded_av_payload, bool uses_nalu_info_structs,
    const takion::WireProfile& profile, std::uint32_t expected_request_id)
    -> PongExpected {
  if (profile.av_layout() != takion::AvLayout::version_7)
    return unexpected(Error::malformed_response);
  if (expected_request_id > std::numeric_limits<std::uint16_t>::max())
    return unexpected(Error::arithmetic_overflow);
  if (media != takion::SenkushaProbeMedia::video) return PongClassification{};
  if (encoded_av_payload.size() <
      video_av_header_size_v7 +
          (uses_nalu_info_structs ? nalu_info_structs_size : 0))
    return unexpected(Error::malformed_response);

  const auto frame_index =
      bytes::load_be<std::uint16_t>(encoded_av_payload.subspan(2));
  if (frame_index != expected_request_id) return PongClassification{};
  return PongClassification{
      ProbeStatus::matched,
      static_cast<std::uint32_t>(frame_index),
  };
}

}  // namespace libchiaking::net::protocol::senkusha
