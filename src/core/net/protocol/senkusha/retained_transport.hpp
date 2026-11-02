/// @file
/// @brief Declares RetainedTransport, the socket wrapper that lets a failed
/// connection test give its route back instead of closing it.
#pragma once

#include "src/core/net/transport/datagram/transport.hpp"

#include <chrono>
#include <memory>
#include <utility>

namespace libchiaking::net::protocol::senkusha {

/// @brief A datagram transport whose close() is ignored, so the Takion
/// session of a test can stop without closing the socket underneath.
///
/// Client wraps the socket in one when the failure handoff policy is
/// return_route, keeps a raw pointer to it, and either releases the socket
/// for the next stage or closes it for real with close_retained(). Once
/// really closed, every call fails with TransportError::closed. Owned by the
/// Takion session; used from the test's thread only. pollable(),
/// enable_traffic_class() and last_traffic_class() are not forwarded and
/// keep the base class answers.
class RetainedTransport final
    : public net::transport::datagram::DatagramTransport {
 public:
  /// @brief Wraps a socket.
  ///
  /// @param transport The socket to keep; may be null, in which case every
  ///     call fails as closed.
  explicit RetainedTransport(
      std::unique_ptr<net::transport::datagram::DatagramTransport> transport)
      : transport_(std::move(transport)) {}
  /// @brief Closes the socket unless it was released.
  ~RetainedTransport() override { (void)close_retained(); }

  /// @brief Sends one datagram on the socket.
  ///
  /// @param data The datagram.
  /// @return What the socket's send() returns.
  /// @retval net::transport::datagram::TransportError::closed The socket
  ///     was closed or released.
  auto send(std::span<const std::byte> data)
      -> Expected<std::size_t,
                  net::transport::datagram::TransportError> override {
    if (!transport_ || closed_)
      return unexpected(net::transport::datagram::TransportError::closed);
    return transport_->send(data);
  }
  /// @brief Reads one datagram from the socket without waiting.
  ///
  /// @return What the socket's receive() returns.
  /// @retval net::transport::datagram::TransportError::closed The socket
  ///     was closed or released.
  auto receive()
      -> Expected<std::optional<net::transport::datagram::Datagram>,
                  net::transport::datagram::TransportError> override {
    if (!transport_ || closed_)
      return unexpected(net::transport::datagram::TransportError::closed);
    return transport_->receive();
  }
  /// @brief Does nothing, leaving the socket open for a later handoff.
  ///
  /// @return Always nothing.
  auto close()
      -> Expected<void, net::transport::datagram::TransportError> override {
    return {};
  }
  /// @brief Sets the socket's don't-fragment flag.
  ///
  /// @param enabled Whether IP fragmentation is forbidden.
  /// @return What the socket's set_dont_fragment() returns.
  /// @retval net::transport::datagram::TransportError::closed The socket
  ///     was closed or released.
  auto set_dont_fragment(bool enabled)
      -> Expected<void, net::transport::datagram::TransportError> override {
    if (!transport_ || closed_)
      return unexpected(net::transport::datagram::TransportError::closed);
    return transport_->set_dont_fragment(enabled);
  }
  /// @brief Waits until the socket can take a datagram; blocks up to
  /// `timeout`.
  ///
  /// @param timeout The longest wait.
  /// @return What the socket's wait_writable() returns.
  /// @retval net::transport::datagram::TransportError::closed The socket
  ///     was closed or released.
  auto wait_writable(std::chrono::microseconds timeout)
      -> Expected<bool, net::transport::datagram::TransportError> override {
    if (!transport_ || closed_)
      return unexpected(net::transport::datagram::TransportError::closed);
    return transport_->wait_writable(timeout);
  }
  /// @brief Really closes the socket; later calls do nothing.
  ///
  /// @return Nothing, or the socket's close() failure the first time.
  auto close_retained()
      -> Expected<void, net::transport::datagram::TransportError> {
    if (!transport_ || std::exchange(closed_, true)) return {};
    return transport_->close();
  }
  /// @brief Hands the socket over, leaving this wrapper empty.
  ///
  /// @return The socket; null when it was closed or already released.
  auto release()
      -> std::unique_ptr<net::transport::datagram::DatagramTransport> {
    if (closed_) return {};
    return std::move(transport_);
  }

 private:
  /// @brief The kept socket; null once released.
  std::unique_ptr<net::transport::datagram::DatagramTransport> transport_;
  /// @brief Whether close_retained() has run.
  bool closed_{};
};

}  // namespace libchiaking::net::protocol::senkusha
