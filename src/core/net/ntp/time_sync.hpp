/// @file
/// @brief Declares TimeSync, which learns trustworthy UTC from public NTP
/// providers without relying on the device clock.
#pragma once

#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/ntp/query.hpp"
#include "src/core/net/ntp/trusted_time.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace libchiaking::net::ntp {

/// @brief Learns UTC from public NTP servers, trusting a time only when most of
/// one provider's servers agree on it.
///
/// Providers are tried in order, Cloudflare then Google. For each, every host
/// is
/// resolved, up to eight distinct addresses are kept (IPv4 first), and each
/// address is asked once over its own socket with a 1 second timeout. The
/// samples
/// are projected to the latest arrival instant; the largest group lying within
/// 1
/// second of each other must hold at least two servers and more than half of
/// the
/// samples, and its median becomes the time. Holds references to the providers
/// given, which must outlive it. Holds no state between calls; sync() may run
/// on
/// any thread, and concurrent calls do not share work.
class TimeSync {
 public:
  /// @brief The cancellation token every wait watches.
  using Stop = platform::StopToken;
  /// @brief One server's answer.
  using Sample = Query::Sample;
  /// @brief Told the host name of each NTP server as the sync turns to it.
  using ServerObserver = std::function<void(std::string_view host)>;

  /// @brief An NTP provider and the host names of its servers.
  struct Provider {
    /// @brief Which provider this is.
    TimeSource source;
    /// @brief Its servers' host names, resolved in order.
    std::span<const std::string_view> hosts;
  };

  /// @brief A server address to ask, with the host name it was resolved
  /// from.
  struct Server {
    /// @brief The host name the address was resolved from.
    std::string_view host;
    /// @brief The address to ask.
    platform::IpAddress address;
  };

  /// @brief The NTP UDP port.
  static constexpr std::uint16_t port = 123;
  /// @brief The NTP port as a service name for DNS resolution.
  static constexpr std::string_view service = "123";
  /// @brief Cloudflare's NTP host.
  static constexpr std::array<std::string_view, 1> cloudflare_hosts{
      "time.cloudflare.com"};
  /// @brief Google's public NTP hosts.
  static constexpr std::array<std::string_view, 4> google_hosts{
      "time1.google.com", "time2.google.com", "time3.google.com",
      "time4.google.com"};
  /// @brief The providers in the order they are tried.
  static constexpr std::array<Provider, 2> providers{{
      {TimeSource::cloudflare, cloudflare_hosts},
      {TimeSource::google, google_hosts},
  }};

  /// @brief Widest spread of projected samples counted as agreeing: 1
  /// second.
  static constexpr std::chrono::microseconds agreement{std::chrono::seconds{1}};
  /// @brief How long each server is waited for: 1 second.
  static constexpr std::chrono::microseconds query_timeout{
      std::chrono::seconds{1}};
  /// @brief Most server addresses asked per provider.
  static constexpr std::size_t max_addresses = 8;

  /// @brief Prepares to sync through the platform's network.
  ///
  /// @param network Resolves host names and makes the sockets and pollers;
  ///     must
  ///     outlive this object.
  /// @param monotonic The monotonic clock samples are timed and anchored on;
  ///     must
  ///     outlive this object.
  /// @param random Draws each request's nonce; must outlive this object.
  TimeSync(platform::NetworkProvider& network,
           const platform::ClockProvider& monotonic,
           platform::SecureRandomProvider& random) noexcept;

  /// @brief Learns UTC from the first provider whose servers agree.
  ///
  /// Blocks for DNS resolution and up to 1 second per server asked: at worst
  /// about 16 seconds plus resolution time over both providers.
  ///
  /// @param stop Cancels the sync between and during queries.
  /// @param asking Told each provider's first host before its hosts are
  ///     resolved, then the host of each server before it is asked; never
  ///     told the same host twice in a row. May be empty.
  /// @return The agreed time; nothing when stopped or when no provider's
  ///     servers
  ///     agree.
  [[nodiscard]] auto sync(const Stop& stop, const ServerObserver& asking = {})
      -> std::optional<TrustedTime>;

  /// @brief Finds the time a majority of one provider's samples agree on.
  ///
  /// Only the first sample from each server counts. Each is moved forward to
  /// the
  /// latest arrival instant by the monotonic time between them; the largest
  /// window of projected times no wider than `agreement` must hold at least
  /// two
  /// and more than half of the samples, and its median is the answer.
  ///
  /// @param source The provider the samples came from.
  /// @param samples The answers, in any order.
  /// @return The median of the agreeing samples, anchored to the latest
  ///     arrival;
  ///     nothing when fewer than two distinct servers answered or no majority
  ///     agrees.
  [[nodiscard]] static auto agreed(TimeSource source,
                                   std::span<const Sample> samples)
      -> std::optional<TrustedTime>;

 private:
  /// @brief Asks every address of one provider and judges their agreement.
  ///
  /// Blocks for resolution and up to 1 second per address.
  ///
  /// @param provider The provider to ask.
  /// @param stop Cancels between and during queries.
  /// @param asking Told the host of each server as it is turned to.
  /// @return The agreed time; nothing when stopped or the servers disagree.
  [[nodiscard]] auto from(const Provider& provider, const Stop& stop,
                          const ServerObserver& asking)
      -> std::optional<TrustedTime>;
  /// @brief Resolves host names to the servers to ask.
  ///
  /// Blocks for each DNS lookup. Hosts that fail to resolve are skipped.
  ///
  /// @param hosts The host names.
  /// @param stop Cancels the lookups.
  /// @return Up to max_addresses servers with distinct, specified
  ///     addresses, each under the first host that resolved to it, IPv4
  ///     before IPv6, otherwise in resolution order; empty when none
  ///     resolved.
  auto servers_of(std::span<const std::string_view> hosts, const Stop& stop)
      -> std::vector<Server>;
  /// @brief Asks one server for the time on a fresh socket bound to the
  /// wildcard
  /// address of its family.
  ///
  /// Blocks for up to query_timeout.
  ///
  /// @param server The server's address.
  /// @param stop Cancels the wait.
  /// @return The sample; nothing when a nonce could not be drawn, the socket
  ///     or
  ///     poller could not be made or bound, or the query failed.
  [[nodiscard]] auto sample(const platform::IpAddress& server, const Stop& stop)
      -> std::optional<Sample>;
  /// @brief Draws a random request nonce.
  ///
  /// @return Eight random bytes read big-endian; nothing when the random
  ///     source
  ///     failed or drew zero.
  [[nodiscard]] auto nonce() -> std::optional<Timestamp>;

  /// @brief The borrowed network provider.
  platform::NetworkProvider& network_;
  /// @brief The borrowed monotonic clock.
  const platform::ClockProvider& monotonic_;
  /// @brief The borrowed source of nonces.
  platform::SecureRandomProvider& random_;
};

}  // namespace libchiaking::net::ntp
