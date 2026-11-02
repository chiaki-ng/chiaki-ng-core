#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"

#include "src/core/net/protocol/takion/result.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <algorithm>
#include <iterator>

namespace libchiaking::net::protocol::takion {

auto EnvelopeCodec::encode(std::span<const std::byte> plain_packet,
                           const PacketEnvelope& envelope)
    -> Expected<SecretBytes, ParseFailure> {
  SecretBytes result;
  result.reserve(overhead(envelope) + plain_packet.size());
  if (const auto* psn = std::get_if<PsnEnvelope>(&envelope))
    bytes::append_be<std::uint32_t>(result, psn->type);
  result.append(plain_packet);
  return result;
}

auto EnvelopeCodec::decode(std::span<const std::byte> datagram,
                           const PacketEnvelope& envelope)
    -> Expected<std::span<const std::byte>, ParseFailure> {
  const auto* psn = std::get_if<PsnEnvelope>(&envelope);
  if (!psn) return datagram;
  if (datagram.size() < psn_envelope_size)
    return unexpected(ParseFailure::at(PacketType{},
                                       ParseFailureReason::truncated, 0,
                                       psn_envelope_size, datagram.size()));
  const auto padding = datagram.first(psn_envelope_size - 1);
  if (const auto nonzero = std::ranges::find_if(
          padding, [](std::byte value) { return value != std::byte{}; });
      nonzero != padding.end())
    return unexpected(ParseFailure::of(
        PacketType{}, ParseFailureReason::invalid_field,
        static_cast<std::size_t>(std::distance(padding.begin(), nonzero))));
  if (std::to_integer<std::uint8_t>(datagram[psn_envelope_size - 1]) !=
      psn->type)
    return unexpected(ParseFailure::of(PacketType{},
                                       ParseFailureReason::invalid_field,
                                       psn_envelope_size - 1));
  return datagram.subspan(psn_envelope_size);
}

}  // namespace libchiaking::net::protocol::takion
