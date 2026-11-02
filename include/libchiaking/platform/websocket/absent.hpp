/// @file
/// @brief Declares AbsentWebSocketProvider, the stand-in a factory returns
/// when the host supplies no WebSocket port.
#pragma once

#include <libchiaking/platform/websocket/provider.hpp>

namespace libchiaking::platform {

/// @brief WebSocketProvider that cannot connect anywhere.
///
/// A factory returns it from PlatformFactory::websocket when the host supplies
/// no provider, and leaves PlatformCapability::websocket unset. It has no state
/// and may be called from any thread at once.
class AbsentWebSocketProvider final : public WebSocketProvider {
 public:
  /// @brief Refuses the connection without touching the network.
  ///
  /// @param request Ignored.
  /// @param stop_token Ignored.
  /// @return Always WebSocketError::unsupported.
  [[nodiscard]] auto connect(const WebSocketRequest& request,
                             const StopToken& stop_token)
      -> Expected<std::unique_ptr<WebSocketConnection>, WebSocketError>
      override {
    (void)request;
    (void)stop_token;
    return unexpected(WebSocketError::unsupported);
  }
};

}  // namespace libchiaking::platform
