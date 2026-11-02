#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/platform/default.hpp"

#include "libchiaking/platform/gateway/provider.hpp"
#include "libchiaking/platform/http/curl/selected_provider.hpp"
#include "libchiaking/platform/websocket/curl/provider.hpp"
#include "src/core/utils/diagnostics/traced_http_provider.hpp"

#if defined(LIBCHIAKING_DEFAULT_PLATFORM_BORINGSSL)
#include "libchiaking/platform/crypto/boringssl/provider.hpp"
#else
#include "libchiaking/platform/crypto/openssl/provider.hpp"
#endif

#if defined(LIBCHIAKING_DEFAULT_PLATFORM_DTLS_OPENSSL)
#include "libchiaking/platform/dtls/openssl/provider.hpp"
#endif

namespace libchiaking::platform {

class DefaultPlatform::Providers {
 public:
  Providers(const HttpTransportSettings& settings,
            ThreadingProvider* host_threading, ClockProvider* host_clock,
            ClockProvider* host_wall_clock)
      : threading(host_threading != nullptr ? *host_threading : own_threading),
        clock(host_clock != nullptr ? *host_clock : own_clock),
        wall_clock(host_wall_clock != nullptr ? *host_wall_clock
                                              : own_wall_clock),
        http_backend(settings, threading, clock, &log) {}

  DefaultThreadingProvider own_threading;
  SteadyClock own_clock;
  SystemClock own_wall_clock;
  ThreadingProvider& threading;
  ClockProvider& clock;
  ClockProvider& wall_clock;
  DefaultNetworkProvider network{threading, clock};
  diagnostics::LogQueue log{clock};
#if defined(LIBCHIAKING_DEFAULT_PLATFORM_BORINGSSL)
  boringssl::BoringSslCryptoProvider crypto;
#else
  openssl::OpenSslCryptoProvider crypto;
#endif
#if defined(LIBCHIAKING_DEFAULT_PLATFORM_DTLS_OPENSSL)
  openssl::OpenSslSecureDatagramProvider secure_datagram_backend;
  SecureDatagramProvider* secure_datagram{&secure_datagram_backend};
#else
  SecureDatagramProvider* secure_datagram{nullptr};
#endif
  SelectedCurlHttpProvider http_backend;
  CurlWebSocketProvider websocket{threading, clock};
  std::unique_ptr<GatewayProvider> gateway{
      make_gateway_provider(threading, clock)};
  diagnostics::ApiCapture api_capture;
  diagnostics::TracedHttpProvider http{http_backend, log, clock, &api_capture,
                                       log.attempt()};
  DefaultPlatformFactory factory{
      PlatformProviders{.threading = &threading,
                        .clock = &clock,
                        .wall_clock = &wall_clock,
                        .network = &network,
                        .crypto = &crypto,
                        .http = &http,
                        .websocket = &websocket,
                        .diagnostics = &log,
                        .gateway = gateway.get(),
                        .secure_datagram = secure_datagram}};
};

DefaultPlatform::DefaultPlatform() : DefaultPlatform(HttpTransportSettings{}) {}

DefaultPlatform::DefaultPlatform(const HttpTransportSettings& settings)
    : providers_(
          std::make_unique<Providers>(settings, nullptr, nullptr, nullptr)) {}

DefaultPlatform::DefaultPlatform(const HttpTransportSettings& settings,
                                 ThreadingProvider& threading,
                                 ClockProvider& clock,
                                 ClockProvider& wall_clock)
    : providers_(std::make_unique<Providers>(settings, &threading, &clock,
                                             &wall_clock)) {}

DefaultPlatform::~DefaultPlatform() = default;

auto DefaultPlatform::factory() noexcept -> PlatformFactory& {
  return providers_->factory;
}
auto DefaultPlatform::log() noexcept -> diagnostics::LogQueue& {
  return providers_->log;
}
auto DefaultPlatform::api_capture() noexcept -> diagnostics::ApiCapture& {
  return providers_->api_capture;
}

auto DefaultPlatform::http() noexcept -> HttpProvider& {
  return providers_->http;
}
auto DefaultPlatform::http_transport() noexcept -> SelectedCurlHttpProvider& {
  return providers_->http_backend;
}
auto DefaultPlatform::websocket() noexcept -> WebSocketProvider& {
  return providers_->websocket;
}
auto DefaultPlatform::crypto() noexcept -> CryptoProvider& {
  return providers_->crypto;
}
auto DefaultPlatform::threading() noexcept -> ThreadingProvider& {
  return providers_->factory.threading();
}
auto DefaultPlatform::clock() noexcept -> ClockProvider& {
  return providers_->factory.clock();
}
auto DefaultPlatform::wall_clock() noexcept -> ClockProvider& {
  return providers_->factory.wall_clock();
}
auto DefaultPlatform::random() noexcept -> SecureRandomProvider& {
  return providers_->factory.random();
}
auto DefaultPlatform::network() noexcept -> NetworkProvider& {
  return providers_->factory.network();
}

}  // namespace libchiaking::platform
