#include "src/core/net/nat/stun/public_servers.hpp"
#include "src/core/net/host_port.hpp"
#include "src/core/utils/text/integer.hpp"

#include <algorithm>

#include "src/core/net/ports.hpp"

namespace libchiaking::net::nat::stun {

auto PublicServers::parse_host_list(std::string_view input)
    -> Expected<std::vector<StunHostListEntry>, StunHostListError> {
  if (input.size() > max_host_list_bytes)
    return unexpected(StunHostListError::input_too_large);
  std::vector<StunHostListEntry> entries;
  entries.reserve(max_host_entries);
  std::size_t offset{};
  while (offset < input.size()) {
    const auto newline = input.find('\n', offset);
    const auto end = newline == std::string_view::npos ? input.size() : newline;
    auto line = input.substr(offset, end - offset);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.size() > max_host_line_bytes)
      return unexpected(StunHostListError::line_too_long);
    if (entries.size() < max_host_entries)
      if (auto entry = listed_host(line, false))
        entries.push_back(std::move(*entry));
    offset = end == input.size() ? input.size() : end + 1U;
  }
  return entries;
}

auto PublicServers::listed_host(std::string_view line, bool bracketed)
    -> std::optional<StunHostListEntry> {
  const auto written = HostPort::split(line);
  if (!written || written->bracketed() != bracketed || !written->port_text())
    return std::nullopt;
  const auto host = written->host();
  const auto port = written->port();
  if (host.empty() || (!bracketed && host.size() > fqdn_max_bytes) ||
      !std::ranges::all_of(host, text::ascii_visible) || !port || *port == 0)
    return std::nullopt;
  return StunHostListEntry{std::string{host}, *port};
}

auto PublicServers::reference_host_list_complete(
    std::string_view input) noexcept -> bool {
  std::size_t lines{};
  std::size_t offset{};
  while (offset < input.size() && lines < max_host_entries) {
    const auto newline = input.find('\n', offset);
    const auto end = newline == std::string_view::npos ? input.size() : newline;
    const auto line = input.substr(offset, end - offset);
    offset = end == input.size() ? input.size() : end + 1U;
    if (line.empty()) continue;
    ++lines;
    const auto host = line.find_first_not_of(':');
    if (host == std::string_view::npos) return false;
    const auto colon = line.find(':', host);
    if (colon == std::string_view::npos ||
        line.find_first_not_of(':', colon) == std::string_view::npos)
      return false;
  }
  return true;
}

auto PublicServers::parse_ipv6_host_list(std::string_view input)
    -> Expected<std::vector<StunHostListEntry>, StunHostListError> {
  if (input.size() > max_host_list_bytes)
    return unexpected(StunHostListError::input_too_large);
  std::vector<StunHostListEntry> entries;
  entries.reserve(max_host_entries);
  std::size_t offset{};
  while (offset < input.size() && entries.size() < max_host_entries) {
    const auto newline = input.find('\n', offset);
    const auto end = newline == std::string_view::npos ? input.size() : newline;
    auto line = input.substr(offset, end - offset);
    offset = end == input.size() ? input.size() : end + 1U;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.size() > max_ipv6_host_line_bytes)
      return unexpected(StunHostListError::line_too_long);
    if (auto entry = listed_host(line, true))
      entries.push_back(std::move(*entry));
  }
  return entries;
}

auto PublicServers::fetch_list_body(platform::HttpProvider& http,
                                    const platform::StopToken& stop,
                                    std::string_view url)
    -> Expected<std::vector<std::byte>, StunHostListFetchError> {
  if (stop.stop_requested())
    return unexpected(StunHostListFetchError::cancelled);
  platform::HttpRequest request;
  request.method = platform::HttpMethod::get;
  request.url = SecretString{url};
  request.connect_timeout = host_list_connect_timeout;
  request.total_timeout = host_list_total_timeout;
  request.redirect_policy = platform::HttpRedirectPolicy::follow_https;
  request.maximum_response_bytes = max_host_list_bytes;
  auto response = http.perform(request, stop);
  if (!response) {
    if (response.error() == platform::HttpError::cancelled)
      return unexpected(StunHostListFetchError::cancelled);
    return unexpected(StunHostListFetchError::transport);
  }
  if (!net::HttpStatus{response.value().status_code}.is_success())
    return unexpected(StunHostListFetchError::http_status);
  const auto body = response.value().body.bytes();
  return std::vector<std::byte>(body.begin(), body.end());
}

auto PublicServers::fetch_host_list(platform::HttpProvider& http,
                                    const platform::StopToken& stop,
                                    bool* ipv6_follows)
    -> Expected<std::vector<StunHostListEntry>, StunHostListFetchError> {
  if (ipv6_follows) *ipv6_follows = false;
  const auto body = fetch_list_body(http, stop, host_list_url);
  if (!body) return unexpected(body.error());
  const auto text = std::string_view{
      reinterpret_cast<const char*>(body.value().data()), body.value().size()};
  if (ipv6_follows) *ipv6_follows = reference_host_list_complete(text);
  if (text.empty()) return unexpected(StunHostListFetchError::empty);
  auto entries = parse_host_list(text);
  if (!entries) return unexpected(StunHostListFetchError::parse);
  if (entries.value().empty()) return unexpected(StunHostListFetchError::empty);
  return std::move(entries.value());
}

auto PublicServers::fetch_ipv6_host_list(platform::HttpProvider& http,
                                         const platform::StopToken& stop)
    -> Expected<std::vector<StunHostListEntry>, StunHostListFetchError> {
  const auto body = fetch_list_body(http, stop, ipv6_host_list_url);
  if (!body) return unexpected(body.error());
  auto entries = parse_ipv6_host_list(std::string_view{
      reinterpret_cast<const char*>(body.value().data()), body.value().size()});
  if (!entries) return unexpected(StunHostListFetchError::parse);
  return std::move(entries.value());
}

auto PublicServers::build_host_order(
    const std::vector<StunHostListEntry>& preferred)
    -> std::vector<StunHostListEntry> {
  std::vector<StunHostListEntry> ordered;
  ordered.reserve(max_host_entries + fixed_hosts.size());
  for (const auto& entry : preferred) {
    if (ordered.size() == max_host_entries) break;
    if (entry.host.empty() || entry.host.size() > fqdn_max_bytes ||
        entry.port == 0)
      continue;
    bool valid = true;
    for (const char ch : entry.host)
      if (!text::ascii_visible(ch) || ch == ':') valid = false;
    if (valid) ordered.push_back(entry);
  }
  for (const auto& fallback : fixed_hosts)
    ordered.push_back({std::string{fallback.host}, fallback.port});
  return ordered;
}

auto PublicServers::resolve_server_endpoints(
    std::span<const StunServerHost> servers, platform::IpFamily family,
    platform::DnsProvider& dns, const platform::StopToken& stop)
    -> Expected<std::vector<StunServerEndpoint>, StunServerEndpointsError> {
  std::vector<StunServerEndpoint> endpoints;
  const std::size_t count =
      servers.size() < max_stun_servers ? servers.size() : max_stun_servers;
  endpoints.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    if (stop.stop_requested()) break;
    const auto& server = servers[index];
    if (server.host.empty() || server.port == 0) continue;
    char service[6]{};
    const auto [end, error] =
        std::to_chars(service, service + sizeof(service), server.port);
    if (error != std::errc{}) continue;
    const auto resolved = dns.resolve(
        server.host,
        std::string_view{service, static_cast<std::size_t>(end - service)},
        stop);
    if (!resolved) continue;
    for (const auto& address : resolved.value()) {
      if (address.family() != family || address.is_unspecified()) continue;
      endpoints.push_back({index, {address, server.port}});
      break;
    }
  }
  if (endpoints.empty())
    return unexpected(StunServerEndpointsError::no_servers);
  return endpoints;
}

}  // namespace libchiaking::net::nat::stun
