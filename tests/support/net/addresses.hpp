#pragma once

#include "libchiaking/platform/net/interfaces.hpp"
#include "libchiaking/platform/net/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace test_support::net {

class Addresses {
 public:
  static auto ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c,
                   std::uint8_t d) -> libchiaking::platform::IpAddress {
    return libchiaking::platform::IpAddress::ipv4({a, b, c, d});
  }

  static auto ipv6(std::uint16_t last) -> libchiaking::platform::IpAddress {
    std::array<std::uint8_t, 16> bytes{0x20, 0x01, 0x0d, 0xb8};
    bytes[14] = static_cast<std::uint8_t>(last >> 8U);
    bytes[15] = static_cast<std::uint8_t>(last & 0xffU);
    return libchiaking::platform::IpAddress::ipv6(bytes);
  }

  static auto at(const libchiaking::platform::IpAddress& address,
                 std::uint16_t port) -> libchiaking::platform::SocketEndpoint {
    return libchiaking::platform::SocketEndpoint{address, port};
  }

  static auto endpoint_key(const libchiaking::platform::SocketEndpoint& endpoint)
      -> std::string {
    return libchiaking::platform::to_string(endpoint.address) + "/" +
           std::to_string(endpoint.port);
  }

  static auto interface_on(std::string name,
                           const libchiaking::platform::IpAddress& address,
                           libchiaking::platform::InterfaceMedium medium =
                               libchiaking::platform::InterfaceMedium::ethernet)
      -> libchiaking::platform::InterfaceInfo {
    libchiaking::platform::InterfaceInfo info;
    info.name = std::move(name);
    info.address = address;
    info.operational = true;
    info.medium = medium;
    return info;
  }
};

}  // namespace test_support::net
