#include <libchiaking/platform/default_factory.hpp>
#include <libchiaking/platform/factory.hpp>

namespace libchiaking::platform {

PlatformInfo DefaultPlatformFactory::info() const noexcept {
  PlatformCapability capabilities =
      PlatformCapability::threading | PlatformCapability::monotonic_clock |
      PlatformCapability::secure_random | PlatformCapability::network;

  const NetworkCapabilities network = selected_network_->capabilities();
  if (network.socket_dont_fragment_ipv4)
    capabilities = capabilities | PlatformCapability::socket_dontfrag_v4;
  if (network.socket_dont_fragment_ipv6)
    capabilities = capabilities | PlatformCapability::socket_dontfrag_v6;

  if (crypto_ != nullptr)
    capabilities = capabilities | PlatformCapability::crypto;
  if (http_ != nullptr) capabilities = capabilities | PlatformCapability::http;
  if (websocket_ != nullptr)
    capabilities = capabilities | PlatformCapability::websocket;
  if (gateway_ != nullptr) capabilities |= PlatformCapability::gateway_mapping;
  if (secure_datagram_ != nullptr)
    capabilities |= PlatformCapability::secure_datagram;
  if (diagnostics_ != nullptr)
    capabilities = capabilities | PlatformCapability::diagnostics;

  return PlatformInfo{
      .name = "libchiaki-default",
      .abi_version = platform_abi_version,
      .capabilities = capabilities,
  };
}

ThreadingProvider& DefaultPlatformFactory::threading() noexcept {
  return *selected_threading_;
}

ClockProvider& DefaultPlatformFactory::clock() noexcept {
  return *selected_clock_;
}

ClockProvider& DefaultPlatformFactory::wall_clock() noexcept {
  return *selected_wall_clock_;
}

SecureRandomProvider& DefaultPlatformFactory::random() noexcept {
  return *selected_random_;
}

NetworkProvider& DefaultPlatformFactory::network() noexcept {
  return *selected_network_;
}

CryptoProvider& DefaultPlatformFactory::crypto() noexcept {
  return crypto_ != nullptr ? *crypto_ : absent_crypto_;
}

HttpProvider& DefaultPlatformFactory::http() noexcept {
  return http_ != nullptr ? *http_ : absent_http_;
}

WebSocketProvider& DefaultPlatformFactory::websocket() noexcept {
  return websocket_ != nullptr ? *websocket_ : absent_websocket_;
}

DiagnosticsProvider& DefaultPlatformFactory::diagnostics() noexcept {
  return diagnostics_ != nullptr ? *diagnostics_ : absent_diagnostics_;
}

}  // namespace libchiaking::platform
