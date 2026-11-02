#include "libchiaking/detail/consoles/network_search.hpp"
#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/text/integer.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <span>

namespace libchiaking::consoles {

auto Subnet::parse(std::string_view cidr) -> std::optional<Subnet> {
  constexpr std::uint32_t loopback_octet = 127;
  constexpr std::uint32_t first_multicast_octet = 224;
  constexpr unsigned first_octet_shift = 24;
  if (cidr.empty() || cidr.size() > text_limit) return std::nullopt;
  const auto slash = cidr.find('/');
  if (slash == std::string_view::npos || slash == 0 || slash + 1 >= cidr.size())
    return std::nullopt;
  const auto prefix = text::parse_integer<unsigned>(cidr.substr(slash + 1));
  if (!prefix || *prefix < shortest_prefix || *prefix > longest_prefix)
    return std::nullopt;
  const auto address = platform::parse_ip_address(cidr.substr(0, slash));
  if (!address || !address.value().is_ipv4()) return std::nullopt;
  const auto prefix_length = static_cast<std::uint8_t>(*prefix);
  const auto network = number_of(address.value()) & mask_of(prefix_length);
  const auto first_octet = network >> first_octet_shift;
  if (first_octet == 0 || first_octet == loopback_octet ||
      first_octet >= first_multicast_octet)
    return std::nullopt;
  return Subnet{network, prefix_length};
}

auto Subnet::network() const noexcept -> platform::IpAddress {
  return address_of(network_);
}

auto Subnet::host_count() const noexcept -> std::size_t {
  const auto size = std::size_t{1} << (longest_prefix - prefix_length_);
  return size > 2 ? size - 2 : size;
}

auto Subnet::host(std::size_t index) const noexcept -> platform::IpAddress {
  const auto first = prefix_length_ >= longest_prefix - 1 ? 0U : 1U;
  return address_of(network_ + first + static_cast<std::uint32_t>(index));
}

auto Subnet::contains(platform::IpAddress address) const noexcept -> bool {
  return address.is_ipv4() &&
         (number_of(address) & mask_of(prefix_length_)) == network_;
}

auto Subnet::text() const -> std::string {
  return platform::to_string(network()) + "/" + std::to_string(prefix_length_);
}

auto Subnet::mask_of(std::uint8_t prefix_length) noexcept -> std::uint32_t {
  return prefix_length == 0 ? 0U
                            : std::numeric_limits<std::uint32_t>::max()
                                  << (longest_prefix - prefix_length);
}

auto Subnet::number_of(platform::IpAddress address) noexcept -> std::uint32_t {
  const auto octets = address.ipv4_bytes();
  return bytes::load_be<std::uint32_t>(std::as_bytes(std::span{octets}));
}

auto Subnet::address_of(std::uint32_t number) noexcept -> platform::IpAddress {
  std::array<std::uint8_t, 4> octets{};
  bytes::store_be<std::uint32_t>(std::as_writable_bytes(std::span{octets}),
                                 number);
  return platform::IpAddress::ipv4(octets);
}

}  // namespace libchiaking::consoles
