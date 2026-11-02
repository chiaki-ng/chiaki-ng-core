/// @file
/// @brief Declares DatagramTransport, the non-blocking datagram pipe every
/// UDP-based protocol (Takion, Senkusha, RUDP, the Kakusan peer link) sends
/// and receives through.
#pragma once

#include <libchiaking/detail/expected.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::platform {
class Pollable;
class StopToken;
}  // namespace libchiaking::platform

namespace libchiaking::net::transport::datagram {

/// @brief Why a datagram operation failed.
enum class TransportError : std::uint8_t {
  /// @brief The operation could not proceed without waiting; try again once
  /// the transport is ready.
  would_block,
  /// @brief The transport or its socket has been closed.
  closed,
  /// @brief Any other failure: the socket reported an error, no peer is set,
  /// a
  /// datagram was sent short or received truncated, or the option is not
  /// supported.
  io_failure,
};

/// @brief One received datagram's payload, borrowed from the transport that
/// read it and valid until that transport's next receive() or close().
///
/// The bytes are writable so a caller may decode them in place; a caller that
/// keeps a datagram past the next read copies it into an OwnedDatagram.
using Datagram = std::span<std::byte>;

/// @brief A datagram copied out of its transport, owned by whoever keeps it.
using OwnedDatagram = std::vector<std::byte>;

/// @brief A non-blocking channel of whole datagrams to one peer.
///
/// Implementations never block in send(), receive() or close(); a caller that
/// needs to wait polls pollable() or uses wait_writable(). The optional
/// capabilities (don't-fragment, traffic class, writability) have defaults that
/// report them unsupported. A transport is used from one thread at a time.
class DatagramTransport {
 public:
  /// @brief Releases the transport and any socket it owns.
  virtual ~DatagramTransport() = default;
  /// @brief Sends one datagram to the peer without blocking.
  ///
  /// @param datagram The whole datagram.
  /// @return The bytes sent, which is the datagram's size.
  /// @retval TransportError::would_block The socket cannot take it now.
  /// @retval TransportError::closed The transport is closed.
  /// @retval TransportError::io_failure The send failed or was short.
  [[nodiscard]] virtual auto send(std::span<const std::byte> datagram)
      -> Expected<std::size_t, TransportError> = 0;
  /// @brief Takes the next datagram from the peer without blocking.
  ///
  /// The datagram stays in a buffer the transport owns and reuses, so it is
  /// valid only until the next receive() or close().
  ///
  /// @return The datagram; nothing when none is waiting or the one read was
  ///     not from the peer.
  /// @retval TransportError::closed The transport is closed.
  /// @retval TransportError::io_failure The read failed.
  [[nodiscard]] virtual auto receive()
      -> Expected<std::optional<Datagram>, TransportError> = 0;
  /// @brief Closes the transport; later calls fail with
  /// TransportError::closed. Closing twice succeeds.
  ///
  /// @return Nothing on success.
  /// @retval TransportError The underlying close failed.
  [[nodiscard]] virtual auto close() -> Expected<void, TransportError> = 0;
  /// @brief The object a poller waits on for this transport's readiness.
  ///
  /// @return The pollable; null when the transport cannot be polled, which is
  ///     the default, or is closed.
  virtual auto pollable() const noexcept -> const platform::Pollable* {
    return nullptr;
  }
  /// @brief Whether pollable() tells the whole truth about reading: when a
  /// poll finds it not readable, receive() would return nothing, because the
  /// transport holds no datagrams of its own above the socket.
  ///
  /// @return False by default, so callers keep reading regardless of polls.
  virtual auto readiness_exact() const noexcept -> bool { return false; }
  /// @brief Waits until a datagram is waiting to be read.
  ///
  /// May block for up to `timeout`.
  ///
  /// @param timeout Longest wait.
  /// @param stop_token Ends the wait early.
  /// @return Whether the transport became readable.
  /// @retval TransportError::io_failure By default, without waiting: the
  ///     transport cannot wait.
  [[nodiscard]] virtual auto wait_readable(
      [[maybe_unused]] std::chrono::nanoseconds timeout,
      [[maybe_unused]] const platform::StopToken& stop_token)
      -> Expected<bool, TransportError> {
    return unexpected(TransportError::io_failure);
  }
  /// @brief The receive buffer the operating system gave the transport's
  /// socket.
  ///
  /// @return Its size in bytes; nothing when the transport has no socket of
  ///     its own or the size could not be read, which is the default.
  virtual auto kernel_receive_buffer() const noexcept
      -> std::optional<std::size_t> {
    return std::nullopt;
  }
  /// @brief Sets or clears the IP don't-fragment flag on outgoing datagrams,
  /// used to probe the path MTU.
  ///
  /// @param enabled True to forbid fragmentation.
  /// @return Nothing on success; by default clearing always succeeds.
  /// @retval TransportError::io_failure By default when `enabled` is true:
  ///     the
  ///     transport cannot set the flag.
  [[nodiscard]] virtual auto set_dont_fragment(bool enabled)
      -> Expected<void, TransportError> {
    if (enabled) return unexpected(TransportError::io_failure);
    return {};
  }
  /// @brief Waits until a datagram can be sent without blocking.
  ///
  /// May block for up to `timeout`.
  ///
  /// @param timeout Longest wait.
  /// @return Whether the transport became writable; false by default, without
  ///     waiting.
  /// @retval TransportError The wait failed.
  [[nodiscard]] virtual auto wait_writable(std::chrono::microseconds timeout)
      -> Expected<bool, TransportError> {
    static_cast<void>(timeout);
    return false;
  }
  /// @brief Asks the socket to report the IP traffic class (TOS / DSCP and
  /// ECN
  /// byte) of each received datagram, read through last_traffic_class().
  ///
  /// @return Nothing on success.
  /// @retval TransportError::io_failure By default: not supported.
  [[nodiscard]] virtual auto enable_traffic_class()
      -> Expected<void, TransportError> {
    return unexpected(TransportError::io_failure);
  }
  /// @brief The traffic class byte of the datagram last returned by
  /// receive().
  ///
  /// @return The byte; nothing when not enabled, not reported, or by default.
  virtual auto last_traffic_class() const noexcept
      -> std::optional<std::uint8_t> {
    return std::nullopt;
  }
};

}  // namespace libchiaking::net::transport::datagram
