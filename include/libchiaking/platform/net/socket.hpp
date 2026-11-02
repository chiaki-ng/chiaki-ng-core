/// @file
/// @brief Declares the non-blocking socket ports: Pollable, the UDP, TCP
/// and listener interfaces, and SocketProvider.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/net/error.hpp>
#include <libchiaking/platform/net/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace libchiaking::platform {

class SocketProvider;
class NetworkProvider;

/// @brief On/off options accepted by UdpSocket::set_boolean_option.
enum class SocketOption : std::uint8_t {
  /// @brief Allows sending to a broadcast address. Console search needs it.
  /// Available only where NetworkCapabilities::broadcast says so.
  broadcast,
  /// @brief Allows binding a local address that is still in use or was
  /// recently used.
  reuse_address,
  /// @brief Forbids IP fragmentation of sent datagrams, so an oversized
  /// datagram fails instead of being split. Available only where
  /// NetworkCapabilities says so.
  dont_fragment,
};

/// @brief Describes one datagram returned by UdpSocket::receive_from.
struct UdpReceiveResult {
  /// @brief Number of bytes written to the caller's buffer.
  std::size_t size{0};
  /// @brief Address and port the datagram came from.
  SocketEndpoint source;
  /// @brief The IPv4 type-of-service or IPv6 traffic-class byte of the
  /// datagram. Present only after UdpSocket::receive_traffic_class
  /// enabled reporting and the system supplied the value.
  std::optional<std::uint8_t> traffic_class;
};

/// @brief Base of every handle a Poller can wait on.
///
/// It lets a poller check that a socket was created by the same NetworkProvider
/// before it waits on it. Handles are neither copyable nor movable; they are
/// held through the `std::unique_ptr` that created them.
class Pollable {
 public:
  /// @brief Releases the handle.
  virtual ~Pollable() = default;
  /// @brief Returns the NetworkProvider that created this handle. A poller
  /// compares it with its own owner and refuses a mismatch with
  /// NetError::wrong_provider.
  ///
  /// @return The creating provider; it outlives the handle.
  virtual const NetworkProvider& owner() const noexcept = 0;

 protected:
  /// @brief Constructs the base; only implementations create handles.
  Pollable() = default;
  /// @brief Not copyable; a handle has one owner.
  Pollable(const Pollable&) = delete;
  /// @brief Not copy-assignable; a handle has one owner.
  Pollable& operator=(const Pollable&) = delete;
  /// @brief Not movable; pollers refer to handles by address.
  Pollable(Pollable&&) = delete;
  /// @brief Not move-assignable; pollers refer to handles by address.
  Pollable& operator=(Pollable&&) = delete;
};

/// @brief A non-blocking UDP socket.
///
/// Implemented by the host frontend, or by the stock socket that
/// DefaultSocketProvider creates. The library owns each socket through the
/// `std::unique_ptr` returned by SocketProvider::create_udp_socket.
///
/// No call blocks. A call that would have to wait returns
/// NetError::would_block, and the caller waits with a Poller before
/// trying again. Buffers passed in are borrowed for the duration of the call
/// only. Once close() has succeeded every call returns NetError::closed.
///
/// Virtuals with a default body are optional: the default returns
/// NetError::not_supported, and the library then does without the feature or
/// reports that the operation that needed it failed.
class UdpSocket : public Pollable {
 public:
  /// @brief Releases the socket. The stock socket closes itself if close()
  /// was not called.
  ~UdpSocket() override = default;
  /// @brief Binds the socket to a local address and port.
  ///
  /// @param endpoint Local endpoint. Port 0 lets the system choose; read the
  ///     choice back with local_endpoint().
  /// @return Nothing on success.
  /// @retval NetError::address_in_use Another socket holds the endpoint.
  /// @retval NetError::address_not_available The address is not assigned to
  ///     this host.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto bind(const SocketEndpoint& endpoint)
      -> Expected<void, NetError> = 0;
  /// @brief Sends one datagram to a destination.
  ///
  /// @param datagram Payload to send; read during the call only.
  /// @param destination Where to send it.
  /// @return The number of bytes sent, which is the whole datagram on
  ///     success.
  /// @retval NetError::would_block The send buffer is full; wait for
  ///     PollEvent::writable.
  /// @retval NetError::message_too_large The datagram exceeds what the path
  ///     accepts, for example with SocketOption::dont_fragment set.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto send_to(std::span<const std::byte> datagram,
                                     const SocketEndpoint& destination)
      -> Expected<std::size_t, NetError> = 0;
  /// @brief Receives one queued datagram, if there is one.
  ///
  /// @param buffer Where the payload is written. It must be large enough for
  ///     the largest datagram the caller expects.
  /// @return The payload size, the sender and, when enabled, the traffic
  ///     class.
  /// @retval NetError::would_block No datagram is queued; wait for
  ///     PollEvent::readable.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto receive_from(std::span<std::byte> buffer)
      -> Expected<UdpReceiveResult, NetError> = 0;
  /// @brief Fixes the remote endpoint so that send() can be used and only
  /// datagrams from that peer are received. Optional.
  ///
  /// @param endpoint The peer.
  /// @return Nothing on success.
  /// @retval NetError::not_supported The provider does not implement
  ///     connected UDP sockets; this is the default.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto connect(const SocketEndpoint& endpoint)
      -> Expected<void, NetError> {
    (void)endpoint;
    return unexpected(NetError::not_supported);
  }
  /// @brief Sends one datagram to the peer set by connect(). Optional.
  ///
  /// @param datagram Payload to send; read during the call only.
  /// @return The number of bytes sent.
  /// @retval NetError::not_supported The provider does not implement
  ///     connected UDP sockets; this is the default.
  /// @retval NetError::would_block The send buffer is full.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto send(std::span<const std::byte> datagram)
      -> Expected<std::size_t, NetError> {
    (void)datagram;
    return unexpected(NetError::not_supported);
  }
  /// @brief Turns a socket option on or off.
  ///
  /// @param option The option to change.
  /// @param enabled True to turn it on.
  /// @return Nothing on success.
  /// @retval NetError::not_supported SocketOption::dont_fragment is not
  ///     available for this socket's address family. It must succeed exactly
  ///     when NetworkCapabilities advertises it.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto set_boolean_option(SocketOption option,
                                                bool enabled)
      -> Expected<void, NetError> = 0;
  /// @brief Lets several sockets bind the same local port at the same time,
  /// which hole punching relies on.
  ///
  /// The default forwards to SocketOption::reuse_address. The stock POSIX
  /// socket sets the system's port-sharing option instead where one exists.
  /// Call it before bind().
  ///
  /// @param enabled True to allow sharing.
  /// @return Nothing on success. The library gives up creating its hole-punch
  ///     sockets when this fails.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto set_shared_port_binding(bool enabled)
      -> Expected<void, NetError> {
    return set_boolean_option(SocketOption::reuse_address, enabled);
  }
  /// @brief Asks the system for a receive buffer of a given size.
  ///
  /// The system may grant a different size; read the result with
  /// receive_buffer_size(). The stock socket caps the request at 64 MiB.
  ///
  /// @param bytes Requested size in bytes.
  /// @return Nothing on success.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto set_receive_buffer_size(std::size_t bytes)
      -> Expected<void, NetError> = 0;
  /// @brief Returns the receive buffer size the system actually applied, in
  /// bytes. Optional.
  ///
  /// @return The size in bytes.
  /// @retval NetError::not_supported The provider cannot report it; this is
  ///     the default.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto receive_buffer_size()
      -> Expected<std::size_t, NetError> {
    return unexpected(NetError::not_supported);
  }
  /// @brief Sets the time-to-live of sent IPv4 datagrams. Optional.
  ///
  /// Hole punching sets a low value on the probes it sends to open a mapping
  /// in the local NAT.
  ///
  /// @param ttl Hop limit from 1 to 255.
  /// @return Nothing on success.
  /// @retval NetError::not_supported The provider does not implement it (the
  ///     default), or the socket is not IPv4.
  /// @retval NetError::invalid_argument ttl is 0.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto set_ipv4_ttl(std::uint8_t ttl)
      -> Expected<void, NetError> {
    (void)ttl;
    return unexpected(NetError::not_supported);
  }
  /// @brief Turns reporting of each received datagram's traffic-class byte on
  /// or off. Optional.
  ///
  /// When on, receive_from() fills UdpReceiveResult::traffic_class, which the
  /// streaming transport reads after each datagram.
  ///
  /// @param enabled True to report the byte.
  /// @return Nothing on success.
  /// @retval NetError::not_supported The provider or the system cannot report
  ///     it; this is the default.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto receive_traffic_class(bool enabled)
      -> Expected<void, NetError> {
    (void)enabled;
    return unexpected(NetError::not_supported);
  }
  /// @brief Returns the local address and port the socket is bound to.
  ///
  /// @return The bound endpoint, including the port the system chose when
  ///     bind() was given port 0.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto local_endpoint()
      -> Expected<SocketEndpoint, NetError> = 0;
  /// @brief Closes the socket and releases its system handle.
  ///
  /// @return Nothing on the first call.
  /// @retval NetError::closed The socket was already closed. The conformance
  ///     suite requires this result for a second call.
  [[nodiscard]] virtual auto close() -> Expected<void, NetError> = 0;
};

/// @brief A non-blocking TCP connection.
///
/// Implemented by the host frontend, or by the stock socket that
/// DefaultSocketProvider creates. The library owns each socket through the
/// `std::unique_ptr` returned by SocketProvider::create_tcp_socket or
/// TcpListener::accept.
///
/// No call blocks. A call that would have to wait returns NetError::would_block
/// and the caller waits with a Poller. Buffers are borrowed for the
/// duration of the call only. Once close() has succeeded every call returns
/// NetError::closed.
class TcpSocket : public Pollable {
 public:
  /// @brief Releases the socket. The stock socket closes itself if close()
  /// was not called.
  ~TcpSocket() override = default;
  /// @brief Binds the socket to a local address and port before connecting.
  ///
  /// @param endpoint Local endpoint. Port 0 lets the system choose.
  /// @return Nothing on success.
  /// @retval NetError::address_in_use Another socket holds the endpoint.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto bind(const SocketEndpoint& endpoint)
      -> Expected<void, NetError> = 0;
  /// @brief Starts connecting to a remote endpoint.
  ///
  /// @param endpoint The peer to connect to.
  /// @return Nothing when the connection is already established, which can
  ///     happen for a local peer. The stock socket also returns success when
  ///     the system reports that the socket is connected already.
  /// @retval NetError::would_block The connection is in progress. Wait for
  ///     PollEvent::writable, then call complete_connect().
  /// @retval NetError::connection_refused The peer refused at once.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto connect(const SocketEndpoint& endpoint)
      -> Expected<void, NetError> = 0;
  /// @brief Collects the result of a connect() that returned
  /// NetError::would_block.
  ///
  /// Call it once the poller reports the socket writable.
  ///
  /// @return Nothing when the connection is established.
  /// @retval NetError::connection_refused The peer refused the connection.
  /// @retval NetError::timed_out The peer did not answer.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto complete_connect() -> Expected<void, NetError> = 0;
  /// @brief Sends bytes on the connection.
  ///
  /// @param data Bytes to send; read during the call only.
  /// @return The number of bytes accepted, which may be fewer than offered.
  ///     Send the remainder after the socket is writable again.
  /// @retval NetError::would_block Nothing could be accepted without waiting.
  /// @retval NetError::closed The socket has been closed, or the peer or the
  ///     local stack has reset or aborted the connection.
  [[nodiscard]] virtual auto send(std::span<const std::byte> data)
      -> Expected<std::size_t, NetError> = 0;
  /// @brief Receives bytes that have arrived on the connection.
  ///
  /// @param buffer Where the bytes are written.
  /// @return The number of bytes written, always at least one.
  /// @retval NetError::would_block Nothing has arrived; wait for
  ///     PollEvent::readable.
  /// @retval NetError::closed The peer closed, reset or aborted the
  ///     connection, or the socket has been closed locally. End of stream is
  ///     reported this way,
  ///     never as a zero-length success.
  [[nodiscard]] virtual auto receive(std::span<std::byte> buffer)
      -> Expected<std::size_t, NetError> = 0;
  /// @brief Returns the local address and port of the socket.
  ///
  /// @return The local endpoint.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto local_endpoint()
      -> Expected<SocketEndpoint, NetError> = 0;
  /// @brief Returns the address and port of the connected peer.
  ///
  /// @return The remote endpoint.
  /// @retval NetError::not_connected No connection is established.
  /// @retval NetError::closed The socket has been closed.
  [[nodiscard]] virtual auto peer_endpoint()
      -> Expected<SocketEndpoint, NetError> = 0;
  /// @brief Closes the connection and releases its system handle.
  ///
  /// @return Nothing on the first call.
  /// @retval NetError::closed The socket was already closed.
  [[nodiscard]] virtual auto close() -> Expected<void, NetError> = 0;
};

/// @brief A non-blocking TCP listening socket.
///
/// Implemented by the host frontend, or by the stock listener that
/// DefaultSocketProvider creates. Once close() has succeeded every call returns
/// NetError::closed.
class TcpListener : public Pollable {
 public:
  /// @brief Releases the listener. The stock listener closes itself if
  /// close() was not called.
  ~TcpListener() override = default;
  /// @brief Binds the listener to a local address and port.
  ///
  /// @param endpoint Local endpoint. Port 0 lets the system choose; read the
  ///     choice back with local_endpoint().
  /// @return Nothing on success.
  /// @retval NetError::address_in_use Another socket holds the endpoint.
  /// @retval NetError::closed The listener has been closed.
  [[nodiscard]] virtual auto bind(const SocketEndpoint& endpoint)
      -> Expected<void, NetError> = 0;
  /// @brief Starts accepting incoming connections.
  ///
  /// @param backlog Number of connections the system may queue before
  ///     accept() takes them.
  /// @return Nothing on success.
  /// @retval NetError::closed The listener has been closed.
  [[nodiscard]] virtual auto listen(int backlog)
      -> Expected<void, NetError> = 0;
  /// @brief Takes one queued incoming connection, if there is one.
  ///
  /// @return A connected, non-blocking socket owned by the caller and by the
  ///     same NetworkProvider as the listener.
  /// @retval NetError::would_block No connection is waiting; wait for
  ///     PollEvent::readable.
  /// @retval NetError::closed The listener has been closed.
  [[nodiscard]] virtual auto accept()
      -> Expected<std::unique_ptr<TcpSocket>, NetError> = 0;
  /// @brief Returns the local address and port the listener is bound to.
  ///
  /// @return The bound endpoint.
  /// @retval NetError::closed The listener has been closed.
  [[nodiscard]] virtual auto local_endpoint()
      -> Expected<SocketEndpoint, NetError> = 0;
  /// @brief Stops listening and releases the system handle.
  ///
  /// @return Nothing on the first call.
  /// @retval NetError::closed The listener was already closed.
  [[nodiscard]] virtual auto close() -> Expected<void, NetError> = 0;
};

/// @brief Port that creates sockets.
///
/// Implemented by the host frontend, or by DefaultSocketProvider in this tree.
/// The library reaches it through NetworkProvider::sockets. Every socket it
/// returns must already be in non-blocking mode and must report the enclosing
/// NetworkProvider as its owner.
class SocketProvider {
 public:
  /// @brief Allows destruction through the interface.
  virtual ~SocketProvider() = default;
  /// @brief Creates an unbound UDP socket.
  ///
  /// @param family_of Any address of the wanted family; only its family is
  ///     used.
  /// @return A socket owned by the caller.
  /// @retval NetError::not_supported The host has no support for that family.
  ///     The conformance suite also accepts NetError::address_not_available
  ///     and NetError::network_unreachable from a host without IPv6.
  /// @retval NetError::resource_exhausted No descriptors are left.
  /// @retval NetError::no_buffers Memory for the socket could not be
  ///     allocated.
  [[nodiscard]] virtual auto create_udp_socket(const IpAddress& family_of)
      -> Expected<std::unique_ptr<UdpSocket>, NetError> = 0;
  /// @brief Creates an unconnected TCP socket.
  ///
  /// @param family_of Any address of the wanted family; only its family is
  ///     used.
  /// @return A socket owned by the caller.
  /// @retval NetError::resource_exhausted No descriptors are left.
  /// @retval NetError::no_buffers Memory for the socket could not be
  ///     allocated.
  [[nodiscard]] virtual auto create_tcp_socket(const IpAddress& family_of)
      -> Expected<std::unique_ptr<TcpSocket>, NetError> = 0;
  /// @brief Creates a TCP listening socket that is not yet bound.
  ///
  /// @param family_of Any address of the wanted family; only its family is
  ///     used.
  /// @return A listener owned by the caller.
  /// @retval NetError::resource_exhausted No descriptors are left.
  /// @retval NetError::no_buffers Memory for the listener could not be
  ///     allocated.
  [[nodiscard]] virtual auto create_tcp_listener(const IpAddress& family_of)
      -> Expected<std::unique_ptr<TcpListener>, NetError> = 0;
};

}  // namespace libchiaking::platform
