#include "src/core/net/transport/http/strict_url.hpp"

#include "src/core/net/host_port.hpp"
#include "src/core/net/ports.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/hex.hpp"
#include "src/core/utils/text/integer.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace libchiaking::net::http {

auto StrictUrl::address_literal(std::string_view address) noexcept -> bool {
  const auto literal = [](char value) {
    return text::hex_digit(value) || value == ':' || value == '.';
  };
  return address.find(':') != std::string_view::npos &&
         std::ranges::all_of(address, literal);
}

auto StrictUrl::dns_name(std::string_view host) noexcept -> bool {
  if (host.empty() || !text::ascii_alnum(host.front()) ||
      !text::ascii_alnum(host.back()))
    return false;
  const auto label = [](char value) {
    return text::ascii_alnum(value) || value == '-' || value == '.';
  };
  return std::ranges::all_of(host, label) &&
         host.find("..") == std::string_view::npos;
}

auto StrictUrl::port_number(std::string_view digits,
                            Strictness strictness) noexcept
    -> std::optional<unsigned> {
  const auto port = text::parse_integer<unsigned>(digits);
  if (!port || *port > max_port) return std::nullopt;
  if (strictness == Strictness::request_target && *port == 0)
    return std::nullopt;
  return port;
}

auto StrictUrl::read(std::string_view url, Strictness strictness) noexcept
    -> std::optional<StrictUrl> {
  const auto separator = url.find(scheme_separator);
  if (separator == std::string_view::npos || separator == 0)
    return std::nullopt;
  const auto rest = url.substr(separator + scheme_separator.size());
  const auto authority = rest.substr(0, rest.find_first_of(authority_end));
  if (authority.empty() || authority.find('@') != std::string_view::npos)
    return std::nullopt;
  StrictUrl result;
  result.scheme_ = url.substr(0, separator);
  result.authority_ = authority;
  result.tail_ = rest.substr(authority.size());
  const auto written = HostPort::split(authority);
  if (!written || written->bare_ipv6()) return std::nullopt;
  result.host_ = written->written_host();
  if (written->bracketed()) {
    const auto minimum = strictness == Strictness::request_target
                             ? request_address_minimum
                             : shown_address_minimum;
    if (written->host().size() < minimum || !address_literal(written->host()))
      return std::nullopt;
  }
  if (result.host_.empty() || result.host_.find('%') != std::string_view::npos)
    return std::nullopt;
  if (strictness == Strictness::request_target && !written->bracketed() &&
      !dns_name(result.host_))
    return std::nullopt;
  if (const auto port = written->port_text()) {
    result.port_ = port_number(*port, strictness);
    if (!result.port_) return std::nullopt;
  }
  return result;
}

auto StrictUrl::only_secret_value(std::string_view query, std::string_view name,
                                  std::string_view refused_name,
                                  PercentEncoding::DecodeLimits limits)
    -> Expected<SecretString, QueryFault> {
  std::optional<std::string_view> found;
  while (!query.empty()) {
    const auto end = query.find('&');
    const auto item = query.substr(0, end);
    const auto equals = item.find('=');
    const auto key =
        PercentEncoding::decode_visible(item.substr(0, equals), limits);
    if (!key) return unexpected(QueryFault::undecodable);
    if (key->view() == refused_name) return unexpected(QueryFault::refused);
    if (key->view() == name) {
      if (found || equals == std::string_view::npos)
        return unexpected(QueryFault::ambiguous);
      found = item.substr(equals + 1);
    }
    if (end == std::string_view::npos) break;
    query.remove_prefix(end + 1);
  }
  auto decoded =
      found ? PercentEncoding::decode_visible(*found, limits) : std::nullopt;
  if (!decoded) return unexpected(QueryFault::unusable);
  return std::move(*decoded);
}

auto StrictUrl::redirect_query(std::string_view location,
                               std::string_view address,
                               std::size_t max_bytes) noexcept
    -> Expected<std::string_view, RedirectFault> {
  if (location.size() > max_bytes || !location.starts_with(address) ||
      location.find('#') != std::string_view::npos)
    return unexpected(RedirectFault::elsewhere);
  auto rest = location.substr(address.size());
  const auto separator = address.find(scheme_separator);
  const bool no_path =
      separator != std::string_view::npos &&
      address.substr(separator + scheme_separator.size())
              .find_first_of(authority_end) == std::string_view::npos;
  if (no_path && rest.starts_with('/')) rest.remove_prefix(1);
  if (!rest.starts_with('?')) return unexpected(RedirectFault::no_query);
  return rest.substr(1);
}

auto StrictUrl::component_start(char value) noexcept -> bool {
  return value == '?' || value == '&' || value == '#';
}

auto StrictUrl::first_secret_value(std::string_view location,
                                   std::string_view name, std::size_t capacity)
    -> SecretString {
  if (name.empty() || capacity < 2) return {};
  location = text::c_string(location);
  const auto pattern = std::string{name} + "=";
  auto position = location.find(pattern);
  while (position != std::string_view::npos && position != 0 &&
         !component_start(location[position - 1]))
    position = location.find(pattern, position + 1);
  if (position == std::string_view::npos) return {};
  auto value = location.substr(position + pattern.size());
  value = value.substr(0, value.find_first_of(verbatim_value_end));
  return SecretString{value.substr(0, capacity - 1)};
}

}  // namespace libchiaking::net::http
