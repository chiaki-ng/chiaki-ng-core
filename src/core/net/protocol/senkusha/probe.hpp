/// @file
/// @brief Declares ProbeCodec, which builds Senkusha's probe packets
/// and recognises the far end's answers to them.
///
/// A probe is a Takion audio packet (type 0x03) in the version 7 AV layout,
/// whatever version the session committed:
///
/// | Offset | Size | Field |
/// |-------:|-----:|-------|
/// | 0 | 1 | packet type 0x03 |
/// | 1 | 2 | packet index, big-endian, 0 |
/// | 3 | 2 | frame index, big-endian: the test index (0 for round-trip and
/// outbound MTU probes) | | 5 | 4 | big-endian word: unit index << 21, (units
/// in frame - 1) << 10, FEC units; units in frame is 0x800 | | 9 | 1 | codec
/// 0xff | | 10 | 8 | zero | | 18 | n | payload: 4 bytes, then the tag as 4
/// big-endian bytes, then padding |
///
/// A round-trip probe's payload is 530 bytes of zero apart from the tag, so
/// the UDP payload is exactly 548 bytes (plus any envelope). An outbound MTU
/// probe fills `candidate - 28` bytes, padding from byte 8 of the payload
/// with "CHIAKI" repeated. The far end echoes probes back as audio; an
/// inbound MTU answer is a video packet whose frame index is the request id.
#pragma once

#include "src/core/net/protocol/senkusha/types.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"

#include <cstdint>
#include <span>

namespace libchiaking::net::protocol::senkusha {

/// @brief Whether a received unit answers the probe being waited for.
enum class ProbeStatus : std::uint8_t {
  /// @brief It answers the awaited probe.
  matched,
  /// @brief It is some other packet, or answers another probe.
  unrelated,
};

/// @brief What a received unit was judged to be.
struct PongClassification {
  /// @brief Whether it matched.
  ProbeStatus status{ProbeStatus::unrelated};
  /// @brief The tag it carried, for a round-trip echo; the frame index, for
  /// an inbound MTU answer; zero when the unit was of the wrong medium.
  std::uint32_t tag{};
};

/// @brief A classification, or why the unit could not be read.
using PongExpected = Expected<PongClassification, Error>;

/// @brief Encodes Senkusha probes and classifies the far end's answers.
///
/// Stateless; every member is a static function safe to call from any
/// thread.
class ProbeCodec {
 public:
  /// @brief Builds a 548-byte round-trip probe.
  ///
  /// @param test_index Frame index to put in the header.
  /// @param unit_index Unit index to put in the header; the probe's number
  ///     within the measurement.
  /// @param tag Random tag the echo must carry back.
  /// @param profile The session's bootstrap profile; its audio packets must
  ///     have no extended header.
  /// @param envelope Framing that will wrap the datagram.
  /// @return The encoded AV payload (without the type byte) and the full
  ///     datagram size.
  /// @retval Error::invalid_configuration The profile frames audio
  ///     with an extended header (version 20 and later).
  /// @retval Error::malformed_response The packet codec refused the
  ///     values.
  /// @retval Error::arithmetic_overflow The datagram did not come
  ///     out at 548 bytes plus the envelope.
  [[nodiscard]] static auto build_local_rtt_probe(
      std::uint16_t test_index, std::uint16_t unit_index, std::uint32_t tag,
      const takion::WireProfile& profile,
      const takion::PacketEnvelope& envelope)
      -> Expected<takion::EncodedSenkushaProbe, Error>;

  /// @brief Builds an outbound MTU probe whose IP packet is exactly the
  /// candidate size.
  ///
  /// @param candidate_network_mtu Candidate size, in bytes of IP packet.
  /// @param attempt_index Unit index to put in the header: the attempt's
  ///     try number minus one, at most 2047.
  /// @param tag Random tag the echo must carry back.
  /// @param profile The session's bootstrap profile; its audio packets must
  ///     have no extended header.
  /// @param envelope Framing that will wrap the datagram.
  /// @param mtu_policy Supplies the network overhead and the upper bound.
  /// @return The encoded AV payload and the full datagram size.
  /// @retval Error::arithmetic_overflow The index is above 2047, the
  ///     candidate is below the overhead, or the datagram size came out
  ///     wrong.
  /// @retval Error::invalid_configuration The profile frames audio
  ///     with an extended header, the candidate is above the upper bound,
  ///     or too small for the header and the 8-byte minimum payload.
  /// @retval Error::malformed_response The packet codec refused the
  ///     values.
  [[nodiscard]] static auto build_local_outbound_mtu_probe(
      std::uint32_t candidate_network_mtu, std::uint16_t attempt_index,
      std::uint32_t tag, const takion::WireProfile& profile,
      const takion::PacketEnvelope& envelope, const MtuPolicy& mtu_policy)
      -> Expected<takion::EncodedSenkushaProbe, Error>;

  /// @brief Decides whether an audio unit is the echo of a probe.
  ///
  /// It matches when the frame index, the unit index (if the policy
  /// requires it) and the tag at `policy.tag_offset` past the AV header
  /// all equal what was sent.
  ///
  /// @param media The medium the unit arrived on; anything but audio is
  ///     unrelated.
  /// @param encoded_av_payload The unit's bytes after the packet type byte.
  /// @param uses_nalu_info_structs Whether the type byte had bit 0x10 set,
  ///     adding the 3-byte NALU-info descriptor to the header.
  /// @param profile The profile to read with; only the version 7 AV layout
  ///     is understood.
  /// @param policy The payload size and tag position rules.
  /// @param expected_test_index The frame index sent.
  /// @param expected_unit_index The unit index sent.
  /// @param expected_tag The tag sent.
  /// @return matched or unrelated, with the tag read.
  /// @retval Error::malformed_response The profile is not the
  ///     version 7 layout, or the unit is too short for its header and tag.
  [[nodiscard]] static auto classify_local_rtt_pong(
      takion::SenkushaProbeMedia media,
      std::span<const std::byte> encoded_av_payload,
      bool uses_nalu_info_structs, const takion::WireProfile& profile,
      const ProbePolicy& policy, std::uint16_t expected_test_index,
      std::uint16_t expected_unit_index, std::uint32_t expected_tag)
      -> PongExpected;

  /// @brief Decides whether a video unit answers an inbound MTU request.
  ///
  /// It matches when its frame index equals the request id.
  ///
  /// @param media The medium the unit arrived on; anything but video is
  ///     unrelated.
  /// @param encoded_av_payload The unit's bytes after the packet type byte.
  /// @param uses_nalu_info_structs Whether the 3-byte NALU-info descriptor
  ///     follows the 20-byte video header.
  /// @param profile The profile to read with; only the version 7 AV layout
  ///     is understood.
  /// @param expected_request_id The id of the outstanding request; at most
  ///     65535.
  /// @return matched with the frame index as its tag, or unrelated.
  /// @retval Error::malformed_response The profile is not the
  ///     version 7 layout, or the unit is shorter than its header.
  /// @retval Error::arithmetic_overflow The id does not fit 16 bits.
  [[nodiscard]] static auto classify_local_inbound_mtu_response(
      takion::SenkushaProbeMedia media,
      std::span<const std::byte> encoded_av_payload,
      bool uses_nalu_info_structs, const takion::WireProfile& profile,
      std::uint32_t expected_request_id) -> PongExpected;

 private:
  /// @brief The codec byte every probe carries, 0xff, which no real audio
  /// codec uses.
  static constexpr std::uint8_t probe_codec_marker = 0xff;
  /// @brief Where the tag is written in a probe's payload.
  static constexpr std::size_t probe_tag_offset = 4;
  /// @brief The audio packet type, 0x03, with no flags.
  static constexpr takion::PacketType audio_packet_type{
      static_cast<std::uint8_t>(takion::PacketFamily::audio_and_haptics)};
  /// @brief Payload size of a round-trip probe: 548 - 1 - 17 = 530 bytes.
  static constexpr std::size_t local_probe_payload_size =
      minimum_datagram_size - takion::packet_type_size -
      takion::audio_av_header_size_v7;
};

}  // namespace libchiaking::net::protocol::senkusha
