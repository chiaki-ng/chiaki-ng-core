/// @file
/// @brief Declares CurlWebSocketProvider, the stock WebSocketProvider built on
/// libcurl.
#pragma once

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/platform/websocket/provider.hpp>

#include <string>

namespace libchiaking::platform {

/// @brief Stock WebSocketProvider that connects with libcurl.
///
/// It depends on a libcurl built with WebSocket support and on that libcurl's
/// TLS backend. Peer and host name verification are always on. It accepts ws
/// and wss URLs.
///
/// Each connection it returns runs its own thread, started through the
/// provider's ThreadingProvider, which reads from the socket,
/// answers and sends PINGs and queues complete messages for
/// WebSocketConnection::receive. The queue holds at most 64 messages and 256
/// KiB; if it overflows the connection fails with
/// WebSocketError::message_too_large.
///
/// DefaultPlatform and CurlPlatformServices each create one.
class CurlWebSocketProvider final : public WebSocketProvider {
 public:
  /// @brief Creates the provider.
  ///
  /// @param threading Starts each connection's receive thread and makes the
  ///     locks; borrowed, and must outlive the provider and every connection
  ///     it returns.
  /// @param clock The monotonic clock PING and PONG deadlines are measured
  ///     on; borrowed, and must outlive the provider and every connection it
  ///     returns.
  CurlWebSocketProvider(ThreadingProvider& threading,
                        const ClockProvider& clock);
  /// @brief Destroys the provider; open connections are unaffected.
  ~CurlWebSocketProvider() override = default;
  /// @brief Not copyable; connections borrow its intermediate certificate
  /// cache during connect().
  CurlWebSocketProvider(const CurlWebSocketProvider&) = delete;
  /// @brief Not copy-assignable; connections borrow its intermediate
  /// certificate cache during connect().
  CurlWebSocketProvider& operator=(const CurlWebSocketProvider&) = delete;

  /// @brief Connects with libcurl and starts the connection's receive thread.
  ///
  /// @param request Where and how to connect. It is read during the call
  ///     only.
  /// @param stop_token Cancels the connection attempt when stopped.
  /// @return An open connection owned by the caller.
  /// @retval WebSocketError::cancelled The stop token was stopped before or
  ///     during the attempt.
  /// @retval WebSocketError::timed_out WebSocketRequest::connect_timeout
  ///     elapsed before the TCP connect, the TLS handshake and the HTTP
  ///     upgrade had all completed; the open connection carries no such
  ///     limit afterwards.
  /// @retval WebSocketError::invalid_url The URL is empty or malformed, or is
  ///     neither ws nor wss.
  /// @retval WebSocketError::protocol_error A header name, header value or
  ///     the subprotocol contains characters that are not allowed.
  /// @retval WebSocketError::tls_trust_failed Certificate verification or the
  ///     TLS handshake failed, or a custom trust mode was given no CA
  ///     certificates.
  /// @retval WebSocketError::proxy_failed The proxy failed, or an explicit
  ///     proxy was requested with an empty or invalid URL.
  /// @retval WebSocketError::disconnected The host could not be resolved or
  ///     connected to.
  /// @retval WebSocketError::unsupported libcurl was built without WebSocket
  ///     support.
  /// @retval WebSocketError::backend_failure libcurl could not be set up, a
  ///     timeout or interval was negative, the receive thread could not
  ///     start, or libcurl reported an error not listed above.
  [[nodiscard]] auto connect(const WebSocketRequest& request,
                             const StopToken& stop_token)
      -> Expected<std::unique_ptr<WebSocketConnection>,
                  WebSocketError> override;

 private:
  /// @brief Starts receive threads and makes locks.
  ThreadingProvider& threading_;
  /// @brief Times PINGs and PONGs.
  const ClockProvider& clock_;
  /// @brief Guards intermediates_.
  Mutex intermediates_guard_;
  /// @brief Intermediate certificates recovered through AIA, in PEM, reused
  /// by later connections of this provider.
  std::string intermediates_;
};

}  // namespace libchiaking::platform
