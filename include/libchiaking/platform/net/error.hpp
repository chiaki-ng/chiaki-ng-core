/// @file
/// @brief Declares NetError, the failure vocabulary of the socket, poller, DNS
/// and interface ports.
#pragma once

#include <cstdint>

namespace libchiaking::platform {

/// @brief Reason a networking port call failed.
///
/// A host that implements the networking ports maps its native error codes onto
/// these values. The descriptions say when the stock implementation produces
/// each one.
enum class NetError : std::uint8_t {
  /// @brief The call could not finish without waiting. Nothing is wrong: wait
  /// for readiness with a Poller and call again. Also returned by a TCP
  /// connect that is still in progress.
  would_block,
  /// @brief The handle was closed with close(), or a TCP peer closed or reset
  /// the connection.
  closed,
  /// @brief The peer actively refused the connection.
  connection_refused,
  /// @brief The peer reset the connection.
  connection_reset,
  /// @brief The connection was aborted before it was accepted.
  connection_aborted,
  /// @brief The socket is not connected to a peer.
  not_connected,
  /// @brief The socket already has a connection.
  already_connected,
  /// @brief The local address or port is already bound by another socket.
  address_in_use,
  /// @brief The requested local address does not exist on this host.
  address_not_available,
  /// @brief No route leads to the destination network.
  network_unreachable,
  /// @brief The destination host cannot be reached. The stock DNS provider
  /// also returns this when a name does not resolve to any usable address.
  host_unreachable,
  /// @brief The local network is down.
  network_down,
  /// @brief The operating system denied the operation.
  permission_denied,
  /// @brief An argument was rejected, for example unparsable address text or
  /// more poll requests than the poller accepts.
  invalid_argument,
  /// @brief The datagram is larger than the network path can carry.
  message_too_large,
  /// @brief The system ran out of buffer space or memory for the operation.
  no_buffers,
  /// @brief A deadline passed. DNS providers return it when the resolver
  /// reports a temporary failure, when the lookup outlives its limit, or when
  /// the stop token's own deadline expired.
  timed_out,
  /// @brief The call was interrupted. DNS providers return it when the stop
  /// token was stopped for a reason other than its deadline.
  interrupted,
  /// @brief The provider does not implement this optional operation, or the
  /// option does not exist for this socket's address family.
  not_supported,
  /// @brief A Poller was given a null pollable or one created by a
  /// different NetworkProvider.
  wrong_provider,
  /// @brief A failure that fits none of the other values.
  system_failure,
  /// @brief The process or system has no free descriptors left to create the
  /// socket.
  resource_exhausted,
};

}  // namespace libchiaking::platform
