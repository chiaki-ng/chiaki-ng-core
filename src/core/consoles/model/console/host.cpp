#include "libchiaking/detail/consoles/host.hpp"

#include "libchiaking/platform/net/types.hpp"

#include <algorithm>

namespace libchiaking::consoles {

auto Host::create(std::string_view text, Generation generation)
    -> Expected<Host, Invalid> {
  if (text.empty()) return unexpected(Invalid::empty);
  if (text.size() > limit) return unexpected(Invalid::too_long);
  if (!std::ranges::all_of(text, [](char value) {
        return value >= first_printable && value <= last_printable;
      }))
    return unexpected(Invalid::not_printable);
  Host host{std::string{text}, ipv6_literal(text)};
  if (!host.reaches(generation)) return unexpected(Invalid::ipv6_needs_ps5);
  return host;
}

auto Host::ipv6_literal(std::string_view text) -> bool {
  text = without_brackets(text);
  text = text.substr(0, text.find('%'));
  const auto address = platform::parse_ip_address(text);
  return address && address.value().is_ipv6();
}

}  // namespace libchiaking::consoles
