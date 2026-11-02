/// @file
/// @brief Declares StunProbe, which asks STUN servers how one socket of the
/// host is mapped.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/nat_type.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"
#include "src/core/play/model/nat_mappings.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace libchiaking::platform {
class Poller;
class UdpSocket;
class ClockProvider;
class NetworkProvider;
class SecureRandomProvider;
}  // namespace libchiaking::platform

namespace libchiaking::play {

/// @brief Asks STUN servers how one UDP socket of the host is mapped.
///
/// One socket asks PSN's party voice STUN server for its public mapping, then
/// the first of the public STUN hosts hole punching uses that answers. PSN's
/// own NAT type server answers only a private request form, so it is not
/// asked. Holds references only; the providers must outlive it. Blocks while
/// it runs; not for concurrent use.
class StunProbe {
 public:
  /// @brief How long each server is waited for.
  static constexpr std::chrono::seconds reply_timeout{3};
  /// @brief How many times a server is asked before the next one.
  static constexpr int attempts = 2;
  /// @brief How many public STUN hosts are tried for the second mapping.
  static constexpr std::size_t most_second_servers = 4;

  /// @brief Prepares the probe.
  ///
  /// @param network Opens the socket, resolves the servers and lists the
  ///     host's addresses.
  /// @param clock The monotonic clock timing the waits.
  /// @param random Draws the STUN transaction ids.
  StunProbe(platform::NetworkProvider& network,
            const platform::ClockProvider& clock,
            platform::SecureRandomProvider& random) noexcept;

  /// @brief Asks the servers.
  ///
  /// Blocks for DNS and up to reply_timeout per server attempt.
  ///
  /// @param stop Cancels the probe.
  /// @return The mappings and the host's own IPv4 addresses.
  /// @retval NatTypeFailure::Reason::unanswered The party voice server did
  ///     not answer.
  /// @retval NatTypeFailure Why else the mappings could not be read.
  [[nodiscard]] auto probe(const platform::StopToken& stop)
      -> Expected<NatMappings, NatTypeFailure>;

 private:
  /// @brief A mapping and the server address that reported it.
  using Answer =
      std::optional<std::pair<platform::SocketEndpoint, platform::IpAddress>>;

  /// @brief Asks one server's addresses in turn until one answers.
  ///
  /// @param socket The socket.
  /// @param poller A poller of the same owner.
  /// @param host The server's name.
  /// @param port The server's port.
  /// @param skip An address not to ask again, or nothing.
  /// @param stop Cancels the queries.
  /// @return The mapping and the address that answered, or nothing.
  /// @retval NatTypeFailure The stop token fired or the name did not
  ///     resolve.
  auto ask(platform::UdpSocket& socket, platform::Poller& poller,
           std::string_view host, std::uint16_t port,
           const std::optional<platform::IpAddress>& skip,
           const platform::StopToken& stop) -> Expected<Answer, NatTypeFailure>;

  /// @brief Opens the socket and resolves.
  platform::NetworkProvider& network_;
  /// @brief Times the waits.
  const platform::ClockProvider& clock_;
  /// @brief Draws transaction ids.
  platform::SecureRandomProvider& random_;
};

}  // namespace libchiaking::play
