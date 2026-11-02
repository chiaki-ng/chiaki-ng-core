#include "src/core/play/adapters/links/stream_platform.hpp"

#include <utility>

namespace libchiaking::play {

StreamPlatform::StreamPlatform(
    platform::PlatformFactory& platform,
    std::shared_ptr<platform::NetworkProvider> network) noexcept
    : platform_(platform), network_(std::move(network)) {}

auto StreamPlatform::info() const noexcept -> platform::PlatformInfo {
  return platform_.info();
}

auto StreamPlatform::threading() noexcept -> platform::ThreadingProvider& {
  return platform_.threading();
}

auto StreamPlatform::clock() noexcept -> platform::ClockProvider& {
  return platform_.clock();
}

auto StreamPlatform::wall_clock() noexcept -> platform::ClockProvider& {
  return platform_.wall_clock();
}

auto StreamPlatform::random() noexcept -> platform::SecureRandomProvider& {
  return platform_.random();
}

auto StreamPlatform::network() noexcept -> platform::NetworkProvider& {
  return *network_;
}

auto StreamPlatform::crypto() noexcept -> platform::CryptoProvider& {
  return platform_.crypto();
}

auto StreamPlatform::http() noexcept -> platform::HttpProvider& {
  return platform_.http();
}

auto StreamPlatform::websocket() noexcept -> platform::WebSocketProvider& {
  return platform_.websocket();
}

auto StreamPlatform::gateway() noexcept -> platform::GatewayProvider& {
  return platform_.gateway();
}

auto StreamPlatform::secure_datagram() noexcept
    -> platform::SecureDatagramProvider& {
  return platform_.secure_datagram();
}

auto StreamPlatform::diagnostics() noexcept -> platform::DiagnosticsProvider& {
  return platform_.diagnostics();
}

}  // namespace libchiaking::play
