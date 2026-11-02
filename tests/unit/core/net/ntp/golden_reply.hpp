#pragma once

#include "src/core/net/ntp/packet.hpp"
#include "src/core/net/ntp/timestamp.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace ntp_test {

class GoldenReply {
 public:
  using Datagram = libchiaking::net::ntp::Packet::Datagram;
  using Rejection = libchiaking::net::ntp::Packet::Rejection;
  using Time = libchiaking::net::ntp::Timestamp::Time;

  static constexpr libchiaking::net::ntp::Timestamp nonce{
      0x0123'4567'89ab'cdefU};
  static constexpr Time received{std::chrono::seconds{1'693'961'104} +
                                 std::chrono::milliseconds{500}};
  static constexpr Time transmitted{std::chrono::seconds{1'693'961'104} +
                                    std::chrono::microseconds{500'250}};

  static constexpr auto of(std::initializer_list<std::uint8_t> octets)
      -> Datagram {
    Datagram datagram{};
    std::size_t at = 0;
    for (const auto octet : octets)
      datagram[at++] = static_cast<std::byte>(octet);
    return datagram;
  }

  static constexpr auto valid() -> Datagram {
    return of({
        0x24, 0x01, 0x03, 0xe7, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x20,
        0x47, 0x4f, 0x4f, 0x47, 0xe8, 0xa2, 0x4a, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0xe8, 0xa2, 0x4a, 0x10,
        0x80, 0x00, 0x00, 0x00, 0xe8, 0xa2, 0x4a, 0x10, 0x80, 0x10, 0x62, 0x4e,
    });
  }

  static auto changed(std::size_t at, std::uint8_t octet) -> Datagram {
    auto datagram = valid();
    datagram[at] = static_cast<std::byte>(octet);
    return datagram;
  }

  static auto reply(std::span<const std::byte> datagram) {
    return libchiaking::net::ntp::Packet::reply(datagram, nonce);
  }
};

}  // namespace ntp_test
