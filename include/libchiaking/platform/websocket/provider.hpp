/// @file
/// @brief Declares the WebSocket port: request and message types,
/// WebSocketError, WebSocketConnection and WebSocketProvider.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/secret_string.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

namespace libchiaking::platform {

/// @brief Kind of payload a WebSocket message carries.
enum class WebSocketMessageType : std::uint8_t {
  /// @brief A text message; the payload is UTF-8.
  text,
  /// @brief A binary message.
  binary,
};

/// @brief One complete received WebSocket message, with fragments already
/// joined.
struct WebSocketMessage {
  /// @brief Whether the payload is text or binary.
  WebSocketMessageType type = WebSocketMessageType::text;
  /// @brief Message bytes. Held in a wiping buffer that is erased when the
  /// message is destroyed, because push messages carry session data.
  SecretBytes payload;

  /// @brief Makes an independent copy of the message and its payload.
  ///
  /// @return A message of the same type with a separately wiped payload.
  auto clone() const -> WebSocketMessage { return {type, payload.clone()}; }
};

/// @brief How WebSocketConnection::close ends the connection.
enum class WebSocketCloseMode : std::uint8_t {
  /// @brief Sends a WebSocket CLOSE frame before the transport is torn down.
  protocol_close,
  /// @brief Drops the transport without sending a CLOSE frame.
  transport_only,
};

/// @brief Everything needed to open one WebSocket connection.
///
/// The request owns its URL, headers and TLS material. The URL and header
/// values are wiping secret types, so the request is movable but not copyable;
/// use clone().
struct WebSocketRequest {
  /// @brief Absolute URL. The stock provider accepts the ws and wss schemes
  /// only.
  SecretString url;
  /// @brief Extra headers for the opening handshake, sent in this order with
  /// duplicates kept.
  std::vector<HttpHeader> headers;
  /// @brief Subprotocol to offer in the handshake, or empty to offer none.
  std::string subprotocol;
  /// @brief Limit on connecting and completing the handshake. Zero leaves the
  /// backend's own default in place.
  std::chrono::milliseconds connect_timeout{0};
  /// @brief Interval between keepalive PING frames. Zero sends none.
  std::chrono::milliseconds ping_interval{0};
  /// @brief How long to wait for the PONG that answers a keepalive PING
  /// before the connection fails with WebSocketError::timed_out. Zero does
  /// not wait for it.
  std::chrono::milliseconds pong_timeout{0};
  /// @brief Largest message to accept, in bytes, measured after fragments are
  /// joined. Zero means no cap.
  std::size_t maximum_message_bytes = 0;
  /// @brief Certificate trust and proxy settings for the connection.
  TlsConfiguration tls_configuration;
  /// @brief How close() ends the connection.
  WebSocketCloseMode close_mode = WebSocketCloseMode::protocol_close;
  /// @brief True to send the first keepalive PING as soon as the connection
  /// is up instead of after the first ping_interval.
  bool ping_immediately = false;

  /// @brief Makes an independent copy of the request and its secrets.
  ///
  /// @return A request equal to this one that owns its own secrets.
  auto clone() const -> WebSocketRequest {
    return {url.clone(),
            platform::clone(headers),
            subprotocol,
            connect_timeout,
            ping_interval,
            pong_timeout,
            maximum_message_bytes,
            tls_configuration,
            close_mode,
            ping_immediately};
  }
};

/// @brief Reason a WebSocket connect or receive failed.
enum class WebSocketError : std::uint8_t {
  /// @brief The stop token of that call was stopped.
  cancelled,
  /// @brief Connecting timed out, or a keepalive PING was not answered within
  /// WebSocketRequest::pong_timeout.
  timed_out,
  /// @brief The URL is malformed or is not a WebSocket URL.
  invalid_url,
  /// @brief The server certificate could not be verified, the TLS handshake
  /// failed, or the custom CA material is missing or unusable.
  tls_trust_failed,
  /// @brief The proxy could not be resolved or used, or an explicit proxy was
  /// requested without a usable URL.
  proxy_failed,
  /// @brief The connection is gone: the server closed it, the transport
  /// failed, or close() has been called.
  disconnected,
  /// @brief The server broke the WebSocket protocol. The stock provider also
  /// returns it for a request whose headers or subprotocol are invalid.
  protocol_error,
  /// @brief A message exceeded WebSocketRequest::maximum_message_bytes. The
  /// stock connection also reports it when messages arrive faster than
  /// receive() takes them and its internal queue fills.
  message_too_large,
  /// @brief The backend has no WebSocket support, for example a libcurl built
  /// without it.
  unsupported,
  /// @brief Any other failure.
  backend_failure,
};

/// @brief One open WebSocket connection, used by the library to receive
/// messages only.
///
/// Implemented by the host frontend, or by the connection that
/// CurlWebSocketProvider creates. The library owns it through the
/// `std::unique_ptr` returned by WebSocketProvider::connect and dedicates a
/// thread to calling receive() in a loop.
///
/// The implementation answers server PINGs, sends the keepalive PINGs asked for
/// in the WebSocketRequest, joins fragmented messages and keeps queueing
/// incoming messages between receive() calls, until close() or a fatal
/// transport error. There is no send operation.
class WebSocketConnection {
 public:
  /// @brief Not copyable; a connection has one owner.
  WebSocketConnection(const WebSocketConnection&) = delete;
  /// @brief Not copy-assignable; a connection has one owner.
  WebSocketConnection& operator=(const WebSocketConnection&) = delete;

 protected:
  /// @brief Constructs the base; only implementations create connections.
  WebSocketConnection() = default;

 public:
  /// @brief Releases the connection. The stock connection closes itself if
  /// close() was not called.
  virtual ~WebSocketConnection() = default;

  /// @brief Blocks until a complete message is available, the connection
  /// fails or the stop token is stopped.
  ///
  /// Stopping the token cancels this wait only; the connection stays open and
  /// receive() can be called again. Any other error is final for the
  /// connection.
  ///
  /// @param stop_token Cancels the wait when stopped.
  /// @return The next message in arrival order, owned by the caller.
  /// @retval WebSocketError::cancelled The stop token was stopped.
  /// @retval WebSocketError::disconnected The server closed the connection,
  ///     the transport failed, or close() was called.
  /// @retval WebSocketError::timed_out A keepalive PING was not answered in
  ///     time.
  /// @retval WebSocketError::protocol_error The server sent invalid frames.
  /// @retval WebSocketError::message_too_large A message exceeded the limit.
  [[nodiscard]] virtual auto receive(const StopToken& stop_token)
      -> Expected<WebSocketMessage, WebSocketError> = 0;
  /// @brief Ends the connection in the way WebSocketRequest::close_mode asked
  /// for.
  ///
  /// It can be called more than once and after receive() has failed; later
  /// calls do nothing. Messages still queued are discarded. The stock
  /// connection waits here for its receive thread to finish.
  ///
  /// @return Nothing when the connection was shut down cleanly or was already
  ///     gone.
  /// @retval WebSocketError::timed_out The CLOSE frame could not be sent in
  ///     time.
  /// @retval WebSocketError::backend_failure The backend failed while
  ///     closing.
  virtual auto close() -> Expected<void, WebSocketError> = 0;
  /// @brief Wakes a receive() that is blocked on another thread so that it
  /// checks its stop token again at once.
  ///
  /// The library calls it right after stopping the token, from the thread
  /// that is shutting the connection down, and then joins the receiving
  /// thread. It must be safe to call concurrently with receive() and must not
  /// block. The default does nothing, which is correct for an implementation
  /// whose receive() polls its stop token.
  virtual void interrupt_receive() noexcept {}
};

/// @brief Port that opens WebSocket client connections.
///
/// Implemented by the host frontend, or by CurlWebSocketProvider in this tree.
/// The host passes it in PlatformProviders::websocket; the library obtains it
/// from PlatformFactory::websocket and uses it for the PlayStation Network push
/// channel used to set up internet sessions. Features that need it are refused
/// when the factory does not supply PlatformCapability::websocket.
///
/// An implementation must verify the server certificate chain and host name,
/// apply the request's proxy policy, send the request headers in order with
/// duplicates kept, and enforce WebSocketRequest::maximum_message_bytes. No
/// type of the backend library may appear in the interface.
/// @ingroup api_platform
class WebSocketProvider {
 public:
  /// @brief Not copyable; providers are shared by reference.
  WebSocketProvider(const WebSocketProvider&) = delete;
  /// @brief Not copy-assignable; providers are shared by reference.
  WebSocketProvider& operator=(const WebSocketProvider&) = delete;

 protected:
  /// @brief Constructs the base; only implementations create providers.
  WebSocketProvider() = default;

 public:
  /// @brief Allows destruction through the interface.
  virtual ~WebSocketProvider() = default;

  /// @brief Connects and completes the WebSocket handshake, blocking until it
  /// succeeds or fails.
  ///
  /// @param request Where and how to connect. Borrowed for the call; the
  ///     provider copies what the connection needs later.
  /// @param stop_token Cancels the connection attempt when stopped. It is not
  ///     kept; each WebSocketConnection::receive takes its own token.
  /// @return An open connection owned by the caller.
  /// @retval WebSocketError::cancelled The stop token was stopped.
  /// @retval WebSocketError::timed_out The connect timeout elapsed.
  /// @retval WebSocketError::invalid_url The URL was rejected.
  /// @retval WebSocketError::tls_trust_failed The server could not be
  ///     trusted.
  /// @retval WebSocketError::proxy_failed The proxy could not be used.
  /// @retval WebSocketError::disconnected The host could not be resolved or
  ///     connected to.
  /// @retval WebSocketError::unsupported The backend has no WebSocket
  ///     support.
  /// @retval WebSocketError::backend_failure Any other failure.
  [[nodiscard]] virtual auto connect(const WebSocketRequest& request,
                                     const StopToken& stop_token)
      -> Expected<std::unique_ptr<WebSocketConnection>, WebSocketError> = 0;
};

}  // namespace libchiaking::platform
