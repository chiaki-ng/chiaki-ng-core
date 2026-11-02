/// @file
/// @brief Declares NetworkStack, the operating system's sockets as the stock
/// network provider uses them, and the limits that provider applies.
///
/// posix.cpp and win32.cpp each define it, over BSD sockets and Winsock
/// respectively; only one is built.
#pragma once

#include <libchiaking/platform/net/interfaces.hpp>
#include <libchiaking/platform/net/poller.hpp>
#include <libchiaking/platform/net/provider.hpp>
#include <libchiaking/platform/net/socket.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include "libchiaking/platform/byte_units.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::platform::detail {

using libchiaking::bytes_per_kib;
using libchiaking::bytes_per_mib;
/// @brief The largest UDP receive buffer requested from the system, 64 MiB;
/// larger requests are clamped to it.
inline constexpr std::size_t max_receive_buffer_bytes =
    64 * bytes_per_kib * bytes_per_kib;
/// @brief The longest single wait handed to the system's poll call, one
/// hour.
inline constexpr std::chrono::milliseconds max_poll_timeout =
    std::chrono::hours{1};
/// @brief The buffer size, in bytes, used to drain wake-up signals from a
/// poller's wake socket.
inline constexpr std::size_t wake_drain_buffer_size = 64;
/// @brief The most sockets one poller wait may watch.
inline constexpr std::size_t max_requests = 8192;
/// @brief The longest a poller sleeps between checks of its stop token,
/// 10 ms; bounds how late a wait notices cancellation.
inline constexpr std::chrono::milliseconds wait_quantum{10};

/// @brief The system's sockets, address resolution and interface listing,
/// behind DefaultNetworkProvider.
///
/// All members are static. Sockets are made non-blocking and not inherited
/// by child processes. Every member may be called from any thread; the
/// objects they create follow the threading rules of their public
/// interfaces.
class NetworkStack {
 public:
  /// @brief Makes sure the socket library is ready.
  ///
  /// On Windows starts Winsock once for the process; elsewhere does
  /// nothing.
  ///
  /// @return True when sockets can be used.
  [[nodiscard]] static bool ensure() noexcept;

  /// @brief Looks up a network interface's index by name, as IPv6 scope
  /// identifiers need.
  ///
  /// @param name The interface name, such as `en0`.
  /// @return The index, or 0 when there is no such interface.
  static std::uint32_t interface_index_from_name(
      const std::string& name) noexcept;

  /// @brief Spells an IPv6 scope for an address's `%zone` suffix.
  ///
  /// @param scope_id The scope identifier.
  /// @param link_local Whether the address is link-local, so that the
  ///     scope names an interface.
  /// @return The interface name for a link-local scope where the system
  ///     knows it (not on Windows); otherwise the number in decimal.
  static std::string scope_zone_name(std::uint32_t scope_id, bool link_local);

  /// @brief Opens a UDP socket.
  ///
  /// @param network The provider the socket reports as its owner; must
  ///     outlive the socket.
  /// @param family_of An address whose family, IPv4 or IPv6, the socket
  ///     uses.
  /// @return The socket, open and not yet bound.
  /// @retval NetError Opening failed, such as not_supported for an
  ///     unavailable family or resource_exhausted when out of descriptors.
  [[nodiscard]] static auto create_udp_socket(NetworkProvider& network,
                                              const IpAddress& family_of)
      -> Expected<std::unique_ptr<UdpSocket>, NetError>;

  /// @brief Opens a TCP socket for connecting out.
  ///
  /// @param network The provider the socket reports as its owner; must
  ///     outlive the socket.
  /// @param family_of An address whose family the socket uses.
  /// @return The socket, open and not yet connected.
  /// @retval NetError Opening failed.
  [[nodiscard]] static auto create_tcp_socket(NetworkProvider& network,
                                              const IpAddress& family_of)
      -> Expected<std::unique_ptr<TcpSocket>, NetError>;

  /// @brief Opens a TCP socket for accepting connections.
  ///
  /// @param network The provider the listener and accepted sockets report
  ///     as their owner; must outlive them.
  /// @param family_of An address whose family the listener uses.
  /// @return The listener, open and not yet bound.
  /// @retval NetError Opening failed.
  [[nodiscard]] static auto create_tcp_listener(NetworkProvider& network,
                                                const IpAddress& family_of)
      -> Expected<std::unique_ptr<TcpListener>, NetError>;

  /// @brief Makes a poller that waits on this stack's sockets.
  ///
  /// @param network The provider the poller reports as its owner; only its
  ///     sockets can be waited on.
  /// @param clock The monotonic clock the poller times its waits on; must
  ///     outlive the poller.
  /// @return The poller.
  /// @retval NetError::no_buffers Its wake-up socket pair could not be
  ///     made, or memory ran out.
  /// @retval NetError::system_failure Anything else went wrong.
  [[nodiscard]] static auto create_poller(NetworkProvider& network,
                                          const ClockProvider& clock)
      -> Expected<std::unique_ptr<Poller>, NetError>;

  /// @brief Resolves a host name with the system resolver.
  ///
  /// Blocks for as long as the resolver takes, which may be seconds; it
  /// cannot be cancelled. Only address families configured on the machine
  /// are returned.
  ///
  /// @param host A name or numeric address.
  /// @param service A port number or service name, or empty for none.
  /// @return Every address found, in the resolver's order; never empty.
  /// @retval NetError::timed_out The resolver could not answer for now.
  /// @retval NetError::no_buffers The resolver ran out of memory.
  /// @retval NetError::host_unreachable The name does not resolve to any
  ///     IPv4 or IPv6 address.
  [[nodiscard]] static auto resolve(std::string_view host,
                                    std::string_view service)
      -> Expected<std::vector<IpAddress>, NetError>;

  /// @brief Lists the machine's network interfaces and their addresses.
  ///
  /// @return The interfaces.
  /// @retval NetError The system refused to list them.
  [[nodiscard]] static auto enumerate()
      -> Expected<std::vector<InterfaceInfo>, NetError>;

  /// @brief Reports what the system's sockets can do.
  ///
  /// On POSIX the don't-fragment support is probed once per process, on
  /// first call, by opening a test socket per family; Windows always
  /// supports it.
  ///
  /// @return The capabilities.
  static auto capabilities() noexcept -> NetworkCapabilities;
};

}  // namespace libchiaking::platform::detail
