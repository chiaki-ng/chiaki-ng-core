#include <libchiaking/platform/net/types.hpp>

#include "network_stack.hpp"
#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/text/integer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>

namespace libchiaking::platform {
class IpAddressText {
  static constexpr std::size_t ipv4_octets = 4;
  static constexpr std::size_t ipv6_groups = 8;
  static constexpr std::size_t max_octet_digits = 3;
  static constexpr std::size_t max_group_digits = 4;
  static constexpr std::size_t max_zone_length = 255;
  static constexpr std::string_view elided_zeros = "::";
  static constexpr std::string_view decimal_digits = "0123456789";
  static constexpr int hexadecimal = 16;

  using Groups = std::array<std::uint16_t, ipv6_groups>;

  struct ZeroRun {
    std::size_t start{ipv6_groups};
    std::size_t length{};
  };

  [[nodiscard]] static auto octet(std::string_view text)
      -> std::optional<std::uint8_t> {
    if (text.empty() || text.size() > max_octet_digits) return std::nullopt;
    if (text.size() > 1 && text.front() == '0') return std::nullopt;
    return text::parse_integer<std::uint8_t>(text);
  }

  [[nodiscard]] static auto group(std::string_view text)
      -> std::optional<std::uint16_t> {
    if (text.empty() || text.size() > max_group_digits) return std::nullopt;
    return text::parse_integer<std::uint16_t>(text, hexadecimal);
  }

  [[nodiscard]] static auto parse_ipv4(std::string_view text)
      -> std::optional<IpAddress> {
    std::array<std::uint8_t, ipv4_octets> octets{};
    std::size_t count = 0;
    for (const auto part : std::views::split(text, '.')) {
      const auto value = octet(std::string_view{part.begin(), part.end()});
      if (!value || count == octets.size()) return std::nullopt;
      octets[count++] = *value;
    }
    if (count != octets.size()) return std::nullopt;
    return IpAddress::ipv4(octets);
  }

  [[nodiscard]] static auto groups_of(std::string_view text,
                                      std::span<std::uint16_t> output)
      -> std::optional<std::size_t> {
    if (text.empty()) return 0;
    std::size_t count = 0;
    for (const auto part : std::views::split(text, ':')) {
      const auto value = group(std::string_view{part.begin(), part.end()});
      if (!value || count == output.size()) return std::nullopt;
      output[count++] = *value;
    }
    return count;
  }

  [[nodiscard]] static auto parse_groups(std::string_view text)
      -> std::optional<Groups> {
    Groups groups{};
    const auto split = text.find(elided_zeros);
    if (split == std::string_view::npos) {
      const auto count = groups_of(text, groups);
      if (!count || *count != ipv6_groups) return std::nullopt;
      return groups;
    }
    const auto left = text.substr(0, split);
    const auto right = text.substr(split + elided_zeros.size());
    if (right.find(elided_zeros) != std::string_view::npos) return std::nullopt;
    Groups tail{};
    const auto head_count = groups_of(left, groups);
    const auto tail_count = groups_of(right, tail);
    if (!head_count || !tail_count || *head_count + *tail_count >= ipv6_groups)
      return std::nullopt;
    std::ranges::copy(std::span{tail}.first(*tail_count),
                      groups.end() - static_cast<std::ptrdiff_t>(*tail_count));
    return groups;
  }

  [[nodiscard]] static auto parse_ipv6(std::string_view text)
      -> std::optional<IpAddress> {
    const auto groups = parse_groups(text);
    if (!groups) return std::nullopt;
    std::array<std::uint8_t, 16> octets{};
    for (std::size_t index = 0; index < ipv6_groups; ++index) {
      octets[2 * index] = static_cast<std::uint8_t>((*groups)[index] >> 8U);
      octets[2 * index + 1] = static_cast<std::uint8_t>((*groups)[index]);
    }
    return IpAddress::ipv6(octets);
  }

  [[nodiscard]] static auto parse_zone(std::string_view zone)
      -> std::optional<std::uint32_t> {
    if (zone.empty() || zone.size() > max_zone_length ||
        zone.find_first_of(std::string_view{"\0%", 2}) !=
            std::string_view::npos)
      return std::nullopt;
    if (zone.find_first_not_of(decimal_digits) == std::string_view::npos)
      return text::parse_integer<std::uint32_t>(zone);
    const auto index =
        detail::NetworkStack::interface_index_from_name(std::string(zone));
    if (index == 0) return std::nullopt;
    return index;
  }

  static auto format_ipv4(const IpAddress& address) -> std::string {
    const auto octets = address.ipv4_bytes();
    return std::format("{}.{}.{}.{}", octets[0], octets[1], octets[2],
                       octets[3]);
  }

  static auto groups_of(const IpAddress& address) -> Groups {
    const auto octets = address.bytes();
    Groups groups{};
    for (std::size_t index = 0; index < ipv6_groups; ++index)
      groups[index] = bytes::load_be<std::uint16_t>(octets.subspan(2 * index));
    return groups;
  }

  static auto longest_zero_run(const Groups& groups) -> ZeroRun {
    ZeroRun best;
    ZeroRun current;
    for (std::size_t index = 0; index < groups.size(); ++index) {
      if (groups[index] != 0) {
        current.length = 0;
        continue;
      }
      if (current.length == 0) current.start = index;
      if (++current.length > best.length) best = current;
    }
    return best;
  }

  static auto format_ipv6(const IpAddress& address) -> std::string {
    const auto groups = groups_of(address);
    const auto run = longest_zero_run(groups);
    std::string text;
    for (std::size_t index = 0; index < groups.size();) {
      if (run.length > 1 && index == run.start) {
        text += index == 0 ? elided_zeros : elided_zeros.substr(1);
        index += run.length;
        continue;
      }
      std::format_to(std::back_inserter(text), "{:x}", groups[index]);
      if (index + 1 != groups.size()) text.push_back(':');
      ++index;
    }
    return text;
  }

 public:
  [[nodiscard]] static auto parse(std::string_view text)
      -> Expected<IpAddress, NetError> {
    std::uint32_t scope_id = 0;
    if (const auto percent = text.find('%');
        percent != std::string_view::npos) {
      if (text.find(':') == std::string_view::npos)
        return unexpected(NetError::invalid_argument);
      const auto zone = parse_zone(text.substr(percent + 1));
      if (!zone) return unexpected(NetError::invalid_argument);
      scope_id = *zone;
      text = text.substr(0, percent);
    }
    const auto address = text.find(':') != std::string_view::npos
                             ? parse_ipv6(text)
                             : parse_ipv4(text);
    if (!address) return unexpected(NetError::invalid_argument);
    return address->with_scope_id(scope_id);
  }

  static auto format(const IpAddress& address) -> std::string {
    return address.is_ipv4() ? format_ipv4(address) : format_ipv6(address);
  }

  static auto format_scoped(const IpAddress& address) -> std::string {
    auto text = format(address);
    if (address.scope_id() == 0) return text;
    text.push_back('%');
    text += detail::NetworkStack::scope_zone_name(address.scope_id(),
                                                  address.is_link_local());
    return text;
  }
};

auto parse_ip_address(std::string_view text) -> Expected<IpAddress, NetError> {
  return IpAddressText::parse(text);
}

auto to_string(const IpAddress& address) -> std::string {
  return IpAddressText::format(address);
}

auto to_scoped_string(const IpAddress& address) -> std::string {
  return IpAddressText::format_scoped(address);
}

}  // namespace libchiaking::platform
