/// @file
/// @brief Declares AbsentGatewayProvider, the stand-in a factory returns when
/// the host supplies no gateway port.
#pragma once

#include <libchiaking/platform/gateway/provider.hpp>

namespace libchiaking::platform {

/// @brief GatewayProvider that never finds a gateway.
///
/// A factory returns it from PlatformFactory::gateway when the host supplies
/// no provider, and leaves PlatformCapability::gateway_mapping unset. It has
/// no state and may be called from any thread at once.
class AbsentGatewayProvider final : public GatewayProvider {
 public:
  /// @brief Refuses the search without touching the network.
  ///
  /// @param stop Ignored.
  /// @param timeout Ignored.
  /// @return Always GatewayError::unavailable.
  [[nodiscard]] auto discover(
      const StopToken& stop,
      std::chrono::milliseconds timeout = default_gateway_timeout)
      -> Expected<std::unique_ptr<GatewaySession>, GatewayError> override {
    (void)stop;
    (void)timeout;
    return unexpected(GatewayError::unavailable);
  }
};

}  // namespace libchiaking::platform
