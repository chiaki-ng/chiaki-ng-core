/// @file
/// @brief Declares ProviderUdpTransport, the DatagramTransport over a UDP
/// socket
/// made by the platform's NetworkProvider, and the socket buffer sizes it uses.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/net/poller.hpp>
#include <libchiaking/platform/net/provider.hpp>
#include <libchiaking/platform/net/socket.hpp>
#include <libchiaking/platform/net/types.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include "src/core/net/transport/datagram/transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::transport::datagram {

/// @brief The receive window Takion advertises in its handshake, in bytes:
/// 0x19000 (102400).
inline constexpr std::uint32_t socket_receive_window = 0x19000;
/// @brief The kernel receive buffer every transport asks for, in bytes: 1 MiB.
/// A socket already at least this large is left alone.
inline constexpr std::uint32_t socket_receive_buffer = 0x100000;

/// @brief How ProviderUdpTransport::create() makes and binds its socket.
struct UdpTransportConfig {
  /// @brief The local address and port to bind; port 0 lets the system
  /// choose.
  platform::SocketEndpoint local{};
  /// @brief The peer datagrams go to and are accepted from; nothing leaves
  /// send() failing until set_peer().
  std::optional<platform::SocketEndpoint> peer{};
  /// @brief The don't-fragment flag to set before binding; nothing leaves the
  /// system default. Failing to set it fails creation only when it was asked
  /// for as true.
  std::optional<bool> dont_fragment{};
  /// @brief Whether to connect the socket to `peer` so the system filters
  /// other
  /// sources; ignored without a peer, and a platform that cannot connect UDP
  /// sockets is accepted.
  bool connect_peer{false};
};

/// @brief A DatagramTransport over one platform UDP socket, exchanging
/// datagrams with a single peer.
///
/// Owns the socket and closes it on destruction. Datagrams from any source
/// other
/// than the peer are dropped. A socket that was connected through
/// adopt_connected() also ignores ICMP-driven refusals (connection refused or
/// reset), retrying up to 64 times per call, because hole punching provokes
/// them
/// while the far side's mapping opens. Every new socket's kernel receive buffer
/// is raised to socket_receive_buffer. Not copyable or movable; it is handed
/// out
/// in a unique_ptr. Use from one thread at a time.
class ProviderUdpTransport final : public DatagramTransport {
 public:
  /// @brief Closes the socket unless close() already did.
  ~ProviderUdpTransport() override;

  /// @brief Not copyable: the transport owns its socket.
  ProviderUdpTransport(const ProviderUdpTransport&) = delete;
  /// @brief Not copyable: the transport owns its socket.
  ///
  /// @return Never returns.
  ProviderUdpTransport& operator=(const ProviderUdpTransport&) = delete;
  /// @brief Not movable: callers hold it through a pointer.
  ProviderUdpTransport(ProviderUdpTransport&&) = delete;
  /// @brief Not movable: callers hold it through a pointer.
  ///
  /// @return Never returns.
  ProviderUdpTransport& operator=(ProviderUdpTransport&&) = delete;

  [[nodiscard]] static auto
  /// @brief Makes a socket for the local address's family, configures and
  /// binds
  /// it, and optionally connects it to the peer.
  ///
  /// Does not block.
  ///
  /// @param network Makes the socket and, later, the poller; must outlive the
  ///     transport.
  /// @param config How to make the socket.
  /// @return The transport.
  /// @retval TransportError The socket could not be made, the requested
  ///     don't-fragment flag could not be set, the bind failed, or connecting
  ///     failed for a reason other than being unsupported.
  create(platform::NetworkProvider& network, const UdpTransportConfig& config)
      -> Expected<std::unique_ptr<ProviderUdpTransport>, TransportError>;

  /// @brief Wraps a socket that is already bound, such as one used for hole
  /// punching, without connecting it.
  ///
  /// @param network Makes the poller later; must outlive the transport.
  /// @param socket The socket; ownership passes to the transport.
  /// @param peer The peer, or nothing to set one later.
  /// @return The transport.
  /// @retval TransportError::io_failure `socket` is null.
  [[nodiscard]] static auto adopt(platform::NetworkProvider& network,
                                  std::unique_ptr<platform::UdpSocket> socket,
                                  std::optional<platform::SocketEndpoint> peer)
      -> Expected<std::unique_ptr<ProviderUdpTransport>, TransportError>;

  /// @brief Wraps a bound socket and connects it to the peer, ignoring the
  /// refusals hole punching provokes.
  ///
  /// @param network Makes the poller later; must outlive the transport.
  /// @param socket The socket; ownership passes to the transport.
  /// @param peer The peer to connect to.
  /// @return The transport; when the platform cannot connect UDP sockets it
  ///     sends with an explicit destination and does not ignore refusals.
  /// @retval TransportError::io_failure `socket` is null.
  /// @retval TransportError Connecting failed for a reason other than being
  ///     unsupported.
  [[nodiscard]] static auto adopt_connected(
      platform::NetworkProvider& network,
      std::unique_ptr<platform::UdpSocket> socket,
      platform::SocketEndpoint peer)
      -> Expected<std::unique_ptr<ProviderUdpTransport>, TransportError>;

  /// @brief Sends one datagram to the peer without blocking.
  ///
  /// @param datagram The whole datagram.
  /// @return The bytes sent, equal to the datagram's size.
  /// @retval TransportError::closed The transport is closed.
  /// @retval TransportError::io_failure No peer is set, the send was short,
  ///     or
  ///     it failed (after up to 64 retries of an ignored refusal).
  /// @retval TransportError::would_block The socket cannot take it now.
  [[nodiscard]] auto send(std::span<const std::byte> datagram)
      -> Expected<std::size_t, TransportError> override;

  /// @brief Reads one datagram without blocking and keeps it if it came from
  /// the peer.
  ///
  /// Records the datagram's traffic class for last_traffic_class(). The
  /// datagram is read into the transport's own buffer and returned as a view
  /// of it, valid until the next receive() or close().
  ///
  /// @return The datagram; nothing when none is waiting, it came from another
  ///     source, or only ignored refusals were read.
  /// @retval TransportError::closed The transport is closed.
  /// @retval TransportError::io_failure The read failed or reported more
  ///     bytes
  ///     than the buffer holds.
  [[nodiscard]] auto receive()
      -> Expected<std::optional<Datagram>, TransportError> override;

  /// @brief Closes the socket; later calls fail with TransportError::closed.
  ///
  /// Closing twice succeeds, and a socket the platform already closed counts
  /// as
  /// closed.
  ///
  /// @return Nothing on success.
  /// @retval TransportError The platform's close failed.
  [[nodiscard]] auto close() -> Expected<void, TransportError> override;

  /// @brief The socket, for a poller to wait on.
  ///
  /// @return The socket; null once closed.
  auto pollable() const noexcept -> const platform::Pollable* override;

  /// @brief Reports that the socket's readiness is the transport's: nothing
  /// is buffered above it.
  ///
  /// @return Always true.
  auto readiness_exact() const noexcept -> bool override { return true; }

  /// @brief The receive buffer the system gave the socket when it was
  /// adopted.
  ///
  /// @return Its size in bytes; nothing when it could not be read.
  auto kernel_receive_buffer() const noexcept
      -> std::optional<std::size_t> override;

  /// @brief Sets or clears the don't-fragment flag on the socket.
  ///
  /// @param enabled True to forbid fragmentation.
  /// @return Nothing on success.
  /// @retval TransportError::closed The transport is closed.
  /// @retval TransportError The platform refused the option.
  [[nodiscard]] auto set_dont_fragment(bool enabled)
      -> Expected<void, TransportError> override;

  /// @brief Waits until the socket can take a datagram.
  ///
  /// Blocks for up to `timeout`; cannot be cancelled. Makes the transport's
  /// poller on first use.
  ///
  /// @param timeout Longest wait.
  /// @return Whether the socket became writable.
  /// @retval TransportError::closed The transport is closed.
  /// @retval TransportError The poller could not be made or the wait failed.
  [[nodiscard]] auto wait_writable(std::chrono::microseconds timeout)
      -> Expected<bool, TransportError> override;

  /// @brief Asks the platform to report each received datagram's traffic
  /// class.
  ///
  /// @return Nothing on success.
  /// @retval TransportError::closed The transport is closed.
  /// @retval TransportError The platform refused.
  [[nodiscard]] auto enable_traffic_class()
      -> Expected<void, TransportError> override;

  /// @brief The traffic class of the datagram last returned by receive().
  ///
  /// @return The byte; nothing when the last receive returned no datagram or
  ///     the platform did not report it.
  auto last_traffic_class() const noexcept
      -> std::optional<std::uint8_t> override;

  /// @brief Changes the peer, reconnecting the socket when it is connected.
  ///
  /// @param peer The new peer.
  /// @return Nothing on success.
  /// @retval TransportError::closed The transport is closed.
  /// @retval TransportError Reconnecting failed; the peer is unchanged.
  [[nodiscard]] auto set_peer(platform::SocketEndpoint peer)
      -> Expected<void, TransportError>;

  /// @brief The address and port the socket is bound to.
  ///
  /// @return The local endpoint.
  /// @retval TransportError::closed The transport is closed.
  /// @retval TransportError The platform could not report it.
  [[nodiscard]] auto local_endpoint() const
      -> Expected<platform::SocketEndpoint, TransportError>;

  /// @brief Waits until a datagram is waiting to be read.
  ///
  /// Blocks for up to `timeout` or until `stop_token` stops. Makes the
  /// transport's poller on first use.
  ///
  /// @param timeout Longest wait.
  /// @param stop_token Ends the wait early.
  /// @return Whether the socket became readable; false on timeout or stop.
  /// @retval TransportError::closed The transport is closed.
  /// @retval TransportError The poller could not be made or the wait failed.
  [[nodiscard]] auto wait_readable(std::chrono::nanoseconds timeout,
                                   const platform::StopToken& stop_token)
      -> Expected<bool, TransportError> override;

 private:
  /// @brief Size of the receive buffer, in bytes: the largest UDP payload
  /// over IPv4.
  static constexpr std::size_t default_receive_buffer_size = 65535;
  /// @brief Size of the receive buffer, in bytes, on platforms whose socket
  /// calls are costly: room for any datagram that fits an Ethernet MTU.
  static constexpr std::size_t compact_receive_buffer_size = 2048;
  /// @brief Most consecutive ignored refusals retried within one send() or
  /// receive().
  static constexpr unsigned max_refusals_per_call = 64;

  /// @brief Whether an error is an ICMP-driven refusal by the peer.
  ///
  /// @param error The platform error.
  /// @return True for connection refused or reset.
  static bool peer_refusal(platform::NetError error) noexcept;
  /// @brief Translates a platform error into the transport's vocabulary.
  ///
  /// @param error The platform error.
  /// @return would_block and closed for their counterparts, io_failure for
  ///     the
  ///     rest.
  static TransportError map_error(platform::NetError error) noexcept;
  /// @brief Raises the socket's kernel receive buffer to
  /// socket_receive_buffer unless it is already that large, falling back to
  /// socket_receive_window when the system refuses the larger size.
  ///
  /// @param socket The socket.
  /// @return The size the socket reports afterwards; nothing when it cannot
  ///     be read.
  static auto raise_receive_window(platform::UdpSocket& socket) noexcept
      -> std::optional<std::size_t>;

  /// @brief Takes ownership of a ready socket.
  ///
  /// @param network Makes the poller later.
  /// @param socket The socket.
  /// @param peer The peer, if known.
  ProviderUdpTransport(platform::NetworkProvider& network,
                       std::unique_ptr<platform::UdpSocket> socket,
                       std::optional<platform::SocketEndpoint> peer) noexcept;

  /// @brief Waits for one readiness event on the socket.
  ///
  /// Blocks for up to `timeout` or until `stop_token` stops.
  ///
  /// @param event The event to wait for.
  /// @param timeout Longest wait.
  /// @param stop_token Ends the wait early.
  /// @return Whether the event became ready.
  /// @retval TransportError::closed The transport is closed.
  /// @retval TransportError The poller could not be made or the wait failed.
  [[nodiscard]] auto wait_for(platform::PollEvent event,
                              std::chrono::nanoseconds timeout,
                              const platform::StopToken& stop_token)
      -> Expected<bool, TransportError>;

  /// @brief The provider that makes the poller; never null.
  platform::NetworkProvider* network_;
  /// @brief The kernel receive buffer the socket reported after it was
  /// raised; nothing when it could not be read.
  std::optional<std::size_t> kernel_receive_buffer_;
  /// @brief The owned socket; never null.
  std::unique_ptr<platform::UdpSocket> socket_;
  /// @brief The peer datagrams go to and are accepted from.
  std::optional<platform::SocketEndpoint> peer_;
  /// @brief The buffer each datagram is read into and lent out from by
  /// receive(); its size is the largest accepted.
  std::vector<std::byte> receive_buffer_;
  /// @brief The poller for wait_readable() and wait_writable(); made on first
  /// wait.
  std::unique_ptr<platform::Poller> poller_;
  /// @brief Whether close() has run.
  bool closed_{false};
  /// @brief Whether the socket is connected to the peer, so sends need no
  /// destination.
  bool connected_{false};
  /// @brief Whether refusals are retried and swallowed; set only by
  /// adopt_connected().
  bool ignore_peer_refusals_{false};
  /// @brief Traffic class of the datagram last returned by receive().
  std::optional<std::uint8_t> last_traffic_class_;
};

}  // namespace libchiaking::net::transport::datagram
