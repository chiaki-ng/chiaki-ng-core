#include "src/core/net/ntp/packet.hpp"

#include "src/core/utils/bytes/byte_io.hpp"

namespace libchiaking::net::ntp {

auto Packet::request(Timestamp nonce) noexcept -> Datagram {
  Datagram datagram{};
  bytes::Writer writer{datagram};
  static_cast<void>(
      writer.u8(static_cast<std::uint8_t>(client_version << 3U | client_mode)));
  static_cast<void>(writer.zeros(size - sizeof(std::uint64_t) - 1U));
  static_cast<void>(writer.u64_be(nonce.wire()));
  return datagram;
}

auto Packet::reply(std::span<const std::byte> datagram, Timestamp nonce)
    -> Expected<Packet, Rejection> {
  if (datagram.size() < size) return unexpected(Rejection::short_packet);
  bytes::Reader reader{datagram.first(size)};
  const auto first = reader.u8().value();
  const auto stratum = reader.u8().value();
  static_cast<void>(reader.skip(6));
  const auto root_dispersion = reader.u32_be().value();
  static_cast<void>(reader.skip(12));
  const Timestamp origin{reader.u64_be().value()};
  const Timestamp received{reader.u64_be().value()};
  const Timestamp transmitted{reader.u64_be().value()};

  const auto leap = static_cast<std::uint8_t>(first >> 6U);
  const auto version = static_cast<std::uint8_t>((first >> 3U) & 0x07U);
  const auto mode = static_cast<std::uint8_t>(first & 0x07U);
  if (origin != nonce) return unexpected(Rejection::origin_mismatch);
  if (mode != server_mode) return unexpected(Rejection::not_a_server);
  if (version != 3U && version != 4U)
    return unexpected(Rejection::unsupported_version);
  if (leap == unsynchronised_li) return unexpected(Rejection::unsynchronised);
  if (stratum == 0U) return unexpected(Rejection::kiss_of_death);
  if (stratum > max_stratum) return unexpected(Rejection::stratum_out_of_range);
  if (transmitted.is_zero()) return unexpected(Rejection::no_transmit_time);
  if (root_dispersion_of(root_dispersion) >= max_root_dispersion)
    return unexpected(Rejection::dispersed);
  return Packet{received, transmitted};
}

}  // namespace libchiaking::net::ntp
