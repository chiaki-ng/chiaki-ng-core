/// @file
/// @brief Declares CandidatePolicy, the address classes peer-link ICE
/// candidates may use.
#pragma once

#include "libchiaking/platform/net/types.hpp"

#include <cstdint>

namespace libchiaking::net::nat::ice {

/// @brief Which addresses may appear in a peer-link ICE candidate (RCP WIRE
/// section 3).
///
/// Unspecified, loopback, link-local (including 169.254.0.0/16), IPv4-mapped
/// IPv6, scoped IPv6, IPv4 0.0.0.0/8 and multicast addresses are refused;
/// private unicast addresses are allowed. Stateless.
class CandidatePolicy {
 public:
  /// @brief Whether an address may be offered or accepted as a candidate.
  ///
  /// @param address The address.
  /// @return False for the refused classes above.
  static constexpr auto permits(const platform::IpAddress& address) noexcept
      -> bool {
    if (address.is_unspecified() || address.is_loopback() ||
        address.is_link_local() || address.is_ipv4_mapped() ||
        address.scope_id() != 0)
      return false;
    if (address.is_ipv4()) {
      const auto bytes = address.ipv4_bytes();
      if (bytes[0] == ipv4_link_local_first_octet &&
          bytes[1] == ipv4_link_local_second_octet)
        return false;
      return bytes[0] != ipv4_this_network_octet &&
             bytes[0] < ipv4_multicast_first_octet;
    }
    return address.ipv6_bytes()[0] != ipv6_multicast_first_byte;
  }

 private:
  /// @brief First octet of 0.0.0.0/8, "this network".
  static constexpr std::uint8_t ipv4_this_network_octet = 0;
  /// @brief First octet of 169.254.0.0/16.
  static constexpr std::uint8_t ipv4_link_local_first_octet = 169;
  /// @brief Second octet of 169.254.0.0/16.
  static constexpr std::uint8_t ipv4_link_local_second_octet = 254;
  /// @brief First octet from which IPv4 is multicast or reserved: 224.
  static constexpr std::uint8_t ipv4_multicast_first_octet = 224;
  /// @brief First byte of IPv6 multicast, ff00::/8.
  static constexpr std::uint8_t ipv6_multicast_first_byte = 0xff;
};

}  // namespace libchiaking::net::nat::ice
