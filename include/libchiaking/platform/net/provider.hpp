/// @file
/// @brief Declares NetworkProvider, the root networking port that groups
/// sockets, pollers, DNS and interface enumeration.
#pragma once

#include <libchiaking/platform/net/dns.hpp>
#include <libchiaking/platform/net/interfaces.hpp>
#include <libchiaking/platform/net/poller.hpp>
#include <libchiaking/platform/net/socket.hpp>

namespace libchiaking::platform {

/// @brief Optional socket features a NetworkProvider supports.
///
/// The values must agree with the PlatformCapability::socket_dontfrag_v4 and
/// PlatformCapability::socket_dontfrag_v6 bits of PlatformFactory::info, and
/// with whether UdpSocket::set_boolean_option accepts
/// SocketOption::dont_fragment; the conformance suite checks both.
struct NetworkCapabilities {
  /// @brief True when SocketOption::dont_fragment can be set on IPv4 UDP
  /// sockets.
  bool socket_dont_fragment_ipv4{false};
  /// @brief True when SocketOption::dont_fragment can be set on IPv6 UDP
  /// sockets.
  bool socket_dont_fragment_ipv6{false};
  /// @brief True when UDP sockets accept SocketOption::broadcast and can send
  /// to broadcast addresses.
  ///
  /// A provider over a tunnel or a VPN that carries only unicast reports
  /// false. Console search and wake then leave broadcast addresses out and
  /// send only to the console's own address.
  bool broadcast{true};
  /// @brief True when every socket call is expensive, such as an IPC round
  /// trip to a system service, as on the Nintendo Switch.
  ///
  /// The library then takes its lean streaming path: it skips reads a poll
  /// found nothing for, wakes the stream loop only for controller input that
  /// changes what is sent, waits until the stream's next timer instead of a
  /// fixed protocol tick, keeps a stopping stream open up to a second for
  /// its DISCONNECT to be acknowledged, and coalesces poller wakes. False
  /// keeps every one of those as it was.
  bool costly_socket_calls{false};
};

/// @brief Root networking port: the one object through which the library
/// creates sockets and pollers, resolves names and lists interfaces.
///
/// Implemented by the host frontend when it must supply its own network stack,
/// otherwise by DefaultNetworkProvider. A host passes its implementation in
/// PlatformProviders::network; the library obtains it from
/// PlatformFactory::network.
///
/// The provider must outlive every socket and poller it created, because each
/// of them reports it as its owner. Sockets and pollers from different
/// NetworkProvider instances cannot be mixed in one Poller::wait.
///
/// @see net_conformance::run
/// @ingroup api_platform
class NetworkProvider {
 public:
  /// @brief Allows destruction through the interface.
  virtual ~NetworkProvider() = default;
  /// @brief Returns the socket factory. The reference stays valid for the
  /// lifetime of this provider.
  ///
  /// @return The socket factory. Callable from any thread without blocking.
  virtual SocketProvider& sockets() noexcept = 0;
  /// @brief Returns the poller factory. The reference stays valid for the
  /// lifetime of this provider.
  ///
  /// @return The poller factory. Callable from any thread without blocking.
  virtual PollerProvider& pollers() noexcept = 0;
  /// @brief Returns the name resolver. The reference stays valid for the
  /// lifetime of this provider.
  ///
  /// @return The resolver. Callable from any thread without blocking.
  virtual DnsProvider& dns() noexcept = 0;
  /// @brief Returns the interface lister. The reference stays valid for the
  /// lifetime of this provider.
  ///
  /// @return The interface lister. Callable from any thread without blocking.
  virtual InterfacesProvider& interfaces() noexcept = 0;
  /// @brief Reports which optional socket features this provider supports.
  /// The library reads it before deciding whether to set
  /// SocketOption::dont_fragment on its streaming sockets, and whether a
  /// console search or wake may broadcast.
  ///
  /// @return The optional features; the answer must not change during the
  ///     provider's lifetime. Callable from any thread.
  virtual NetworkCapabilities capabilities() const noexcept = 0;
};

}  // namespace libchiaking::platform
