/// @file
/// @brief Declares PublicServers, the source of public STUN servers: the
/// fetched
/// community lists, the fixed fallbacks and their resolution.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/http/provider.hpp"
#include "libchiaking/platform/net/dns.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::net::nat::stun {

/// @brief Longest DNS name accepted for a STUN server, in bytes.
inline constexpr std::size_t fqdn_max_bytes = 253;

/// @brief A STUN server named in a host list or the fixed fallback list.
struct StunHostListEntry {
  /// @brief A host name, or an IPv6 literal without brackets.
  std::string host;
  /// @brief The UDP port; never 0 once parsed.
  std::uint16_t port{};
};

/// @brief Why a host list could not be parsed.
enum class StunHostListError : std::uint8_t {
  /// @brief The list exceeds 64 KiB.
  input_too_large,
  /// @brief A line exceeds the longest a valid entry can be.
  line_too_long
};

/// @brief Why a host list could not be fetched.
enum class StunHostListFetchError : std::uint8_t {
  /// @brief The stop token fired.
  cancelled,
  /// @brief No HTTP response arrived.
  transport,
  /// @brief The response was not 2xx.
  http_status,
  /// @brief The list was too large or held an overlong line.
  parse,
  /// @brief The list held no usable server.
  empty,
};

/// @brief A built-in STUN server.
struct FixedStunHost {
  /// @brief The host name.
  std::string_view host;
  /// @brief The UDP port.
  std::uint16_t port;
};

/// @brief A STUN server to resolve, by view.
struct StunServerHost {
  /// @brief The host name; empty entries are skipped.
  std::string_view host{};
  /// @brief The UDP port; 0 entries are skipped.
  std::uint16_t port{};
};

/// @brief A STUN server's resolved address.
struct StunServerEndpoint {
  /// @brief Index of the server in the list given to
  /// resolve_server_endpoints().
  std::size_t server_index{};
  /// @brief Its first address of the asked family, with its port.
  platform::SocketEndpoint endpoint{};
};

/// @brief Why no STUN server could be resolved.
enum class StunServerEndpointsError : std::uint8_t {
  /// @brief No server resolved to an address of the asked family.
  no_servers
};
/// @brief Most servers resolve_server_endpoints() looks at.
inline constexpr std::size_t max_stun_servers = 21U;

/// @brief Where public STUN servers come from: the community list at
/// `pradt2/always-online-stun`, fetched over HTTPS, then a fixed fallback list
/// led by Moonlight's and Google's servers.
///
/// The host list is one `host:port` per line; the IPv6 list one
/// `[address]:port` per line. Only the first 10 usable entries of each are
/// kept. Stateless; every member is static and may be called from any thread.
class PublicServers {
 public:
  /// @brief Limit on connecting to fetch a list: 10 seconds.
  static constexpr std::chrono::seconds host_list_connect_timeout{10};

  /// @brief Limit on fetching a whole list: 10 seconds.
  static constexpr std::chrono::seconds host_list_total_timeout{10};

  /// @brief Largest list accepted: 64 KiB.
  static constexpr std::size_t max_host_list_bytes = 64U * 1024U;

  /// @brief Longest line in the host list: a 253-byte name, colon and port.
  static constexpr std::size_t max_host_line_bytes = 259U;

  /// @brief Most entries kept from a list.
  static constexpr std::size_t max_host_entries = 10U;

  /// @brief Longest line in the IPv6 list: a bracketed address, colon and
  /// port.
  static constexpr std::size_t max_ipv6_host_line_bytes = 47U;

  /// @brief Reads the host list.
  ///
  /// Lines end with LF or CRLF. Lines that are not `host:port` with a visible
  /// ASCII host of at most 253 bytes and a port from 1 to 65535 are skipped,
  /// as
  /// are lines past the tenth entry.
  ///
  /// @param input The list.
  /// @return Up to 10 entries, in list order.
  /// @retval StunHostListError::input_too_large The list exceeds 64 KiB.
  /// @retval StunHostListError::line_too_long A line exceeds 259 bytes.
  [[nodiscard]] static auto parse_host_list(std::string_view input)
      -> Expected<std::vector<StunHostListEntry>, StunHostListError>;

  /// @brief Whether the host list looks whole enough to also fetch the IPv6
  /// list.
  ///
  /// Looks at the first 10 non-empty lines.
  ///
  /// @param input The list.
  /// @return False when one of them lacks a host or a port after a colon.
  static auto reference_host_list_complete(std::string_view input) noexcept
      -> bool;

  /// @brief Reads the IPv6 list.
  ///
  /// Lines that are not `[address]:port` with a port from 1 to 65535 are
  /// skipped. Reading stops at the tenth entry.
  ///
  /// @param input The list.
  /// @return Up to 10 entries, hosts without brackets.
  /// @retval StunHostListError::input_too_large The list exceeds 64 KiB.
  /// @retval StunHostListError::line_too_long A line before the tenth entry
  ///     exceeds 47 bytes.
  [[nodiscard]] static auto parse_ipv6_host_list(std::string_view input)
      -> Expected<std::vector<StunHostListEntry>, StunHostListError>;

  /// @brief Where the host list is fetched from.
  static constexpr std::string_view host_list_url =
      "https://raw.githubusercontent.com/pradt2/always-online-stun/master/"
      "valid_hosts.txt";

  /// @brief Where the IPv6 list is fetched from.
  static constexpr std::string_view ipv6_host_list_url =
      "https://raw.githubusercontent.com/pradt2/always-online-stun/master/"
      "valid_ipv6s.txt";

  /// @brief Fetches a list's body, following HTTPS redirects.
  ///
  /// Blocks for up to 10 seconds.
  ///
  /// @param http Performs the request.
  /// @param stop Cancels it.
  /// @param url The list's URL.
  /// @return The body, at most 64 KiB.
  /// @retval StunHostListFetchError::cancelled The stop token fired.
  /// @retval StunHostListFetchError::transport No response arrived.
  /// @retval StunHostListFetchError::http_status The status was not 2xx.
  [[nodiscard]] static auto fetch_list_body(platform::HttpProvider& http,
                                            const platform::StopToken& stop,
                                            std::string_view url)
      -> Expected<std::vector<std::byte>, StunHostListFetchError>;

  /// @brief Fetches and reads the host list.
  ///
  /// Blocks for up to 10 seconds.
  ///
  /// @param http Performs the request.
  /// @param stop Cancels it.
  /// @param ipv6_follows When not null, set to whether the list looks whole
  ///     enough to fetch the IPv6 list too; false on a failed fetch.
  /// @return Up to 10 entries.
  /// @retval StunHostListFetchError::cancelled The stop token fired.
  /// @retval StunHostListFetchError::transport No response arrived.
  /// @retval StunHostListFetchError::http_status The status was not 2xx.
  /// @retval StunHostListFetchError::parse The list could not be parsed.
  /// @retval StunHostListFetchError::empty No usable entry.
  [[nodiscard]] static auto fetch_host_list(platform::HttpProvider& http,
                                            const platform::StopToken& stop,
                                            bool* ipv6_follows = nullptr)
      -> Expected<std::vector<StunHostListEntry>, StunHostListFetchError>;

  /// @brief Fetches and reads the IPv6 list.
  ///
  /// Blocks for up to 10 seconds.
  ///
  /// @param http Performs the request.
  /// @param stop Cancels it.
  /// @return Up to 10 entries; may be empty.
  /// @retval StunHostListFetchError::cancelled The stop token fired.
  /// @retval StunHostListFetchError::transport No response arrived.
  /// @retval StunHostListFetchError::http_status The status was not 2xx.
  /// @retval StunHostListFetchError::parse The list could not be parsed.
  [[nodiscard]] static auto fetch_ipv6_host_list(
      platform::HttpProvider& http, const platform::StopToken& stop)
      -> Expected<std::vector<StunHostListEntry>, StunHostListFetchError>;

  /// @brief The STUN server PSN's party voice uses, in the production
  /// environment.
  static constexpr FixedStunHost psn_party_voice{
      "stn.np.community.playstation.net", 3478};

  /// @brief The fallback servers, tried after the fetched ones; Moonlight's
  /// is
  /// always first.
  static constexpr std::array<FixedStunHost, 11> fixed_hosts{{
      {"stun.moonlight-stream.org", 3478},
      {"stun.l.google.com", 19302},
      {"stun.l.google.com", 19305},
      {"stun1.l.google.com", 19302},
      {"stun1.l.google.com", 19305},
      {"stun2.l.google.com", 19302},
      {"stun2.l.google.com", 19305},
      {"stun3.l.google.com", 19302},
      {"stun3.l.google.com", 19305},
      {"stun4.l.google.com", 19302},
      {"stun4.l.google.com", 19305},
  }};

  /// @brief The order servers are asked in: up to 10 valid fetched entries,
  /// then every fixed host.
  ///
  /// Fetched entries with an empty or overlong host, port 0, or a host
  /// holding
  /// `:` or invisible characters are skipped.
  ///
  /// @param preferred The fetched entries.
  /// @return The ordered servers; the last 11 are always the fixed hosts.
  static auto build_host_order(const std::vector<StunHostListEntry>& preferred)
      -> std::vector<StunHostListEntry>;

  [[nodiscard]] static auto
  /// @brief Resolves servers to addresses of one family.
  ///
  /// Blocks for one DNS lookup per server, at most 21 servers. Servers that
  /// fail to resolve or have no address of the family are skipped.
  ///
  /// @param servers The servers, in order.
  /// @param family The address family wanted.
  /// @param dns Resolves the names.
  /// @param stop Stops before the next lookup.
  /// @return One endpoint per resolved server, in order.
  /// @retval StunServerEndpointsError::no_servers None resolved.
  resolve_server_endpoints(std::span<const StunServerHost> servers,
                           platform::IpFamily family,
                           platform::DnsProvider& dns,
                           const platform::StopToken& stop = {})
      -> Expected<std::vector<StunServerEndpoint>, StunServerEndpointsError>;

 private:
  /// @brief Reads one list line.
  ///
  /// @param line The line without its end.
  /// @param bracketed Whether the host must be a bracketed address.
  /// @return The entry; nothing when the line is not `host:port` of the asked
  ///     shape with a visible host (at most 253 bytes unbracketed) and a port
  ///     from 1 to 65535.
  static auto listed_host(std::string_view line, bool bracketed)
      -> std::optional<StunHostListEntry>;
};

}  // namespace libchiaking::net::nat::stun
