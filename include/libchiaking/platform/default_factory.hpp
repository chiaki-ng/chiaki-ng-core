/// @file
/// @brief Declares DefaultPlatformFactory, the stock PlatformFactory.
#pragma once

#include <libchiaking/platform/crypto/absent.hpp>
#include <libchiaking/platform/diagnostics/absent.hpp>
#include <libchiaking/platform/dtls/absent.hpp>
#include <libchiaking/platform/factory.hpp>
#include <libchiaking/platform/gateway/absent.hpp>
#include <libchiaking/platform/http/absent.hpp>
#include <libchiaking/platform/net/default.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/random_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/platform/websocket/absent.hpp>

namespace libchiaking::platform {

/// @brief Stock PlatformFactory that owns the runtime and network providers and
/// borrows the rest.
///
/// It embeds a DefaultThreadingProvider, SteadyClock, SystemClock,
/// SystemSecureRandom and DefaultNetworkProvider and returns them unless
/// PlatformProviders names replacements. The embedded network runs on the
/// threading provider and monotonic clock the factory selected, supplied or
/// stock. The optional ports are whatever the caller passes in. It reports the
/// name "libchiaki-default".
///
/// @see DefaultPlatform for a version that also owns the optional providers.
/// @ingroup api_platform
class DefaultPlatformFactory final : public PlatformFactory {
 public:
  /// @brief Builds a factory with the stock runtime and network providers and
  /// no optional ports.
  DefaultPlatformFactory() = default;

  /// @brief Builds a factory with the stock runtime and network providers and
  /// one cryptography provider.
  ///
  /// @param crypto Borrowed provider that must outlive the factory, or
  ///     nullptr for none.
  explicit DefaultPlatformFactory(CryptoProvider* crypto) noexcept
      : DefaultPlatformFactory(PlatformProviders{.crypto = crypto}) {}

  /// @brief Builds a factory from a set of host-supplied providers.
  ///
  /// @param providers Borrowed providers. Null runtime and network entries
  ///     fall back to the stock implementations owned by this factory.
  explicit DefaultPlatformFactory(PlatformProviders providers) noexcept
      : selected_threading_(providers.threading ? providers.threading
                                                : &threading_),
        selected_clock_(providers.clock ? providers.clock : &clock_),
        selected_wall_clock_(providers.wall_clock ? providers.wall_clock
                                                  : &wall_clock_),
        selected_random_(providers.random ? providers.random : &random_),
        selected_network_(providers.network ? providers.network : &network_),
        crypto_(providers.crypto),
        http_(providers.http),
        websocket_(providers.websocket),
        diagnostics_(providers.diagnostics),
        gateway_(providers.gateway),
        secure_datagram_(providers.secure_datagram) {}

  /// @brief Reports the ports this factory currently supplies.
  ///
  /// @return The name "libchiaki-default", platform_abi_version, the four
  ///     mandatory flags, the don't-fragment flags the network provider
  ///     advertises, and one flag for each optional provider passed at
  ///     construction.
  PlatformInfo info() const noexcept override;
  /// @brief The threading provider. See PlatformFactory::threading.
  ///
  /// @return The supplied provider, or the owned DefaultThreadingProvider.
  ThreadingProvider& threading() noexcept override;
  /// @brief The monotonic clock. See PlatformFactory::clock.
  ///
  /// @return The supplied clock, or the owned SteadyClock.
  ClockProvider& clock() noexcept override;
  /// @brief The wall clock. See PlatformFactory::wall_clock.
  ///
  /// @return The supplied clock, or the owned SystemClock.
  ClockProvider& wall_clock() noexcept override;
  /// @brief The random source. See PlatformFactory::random.
  ///
  /// @return The supplied source, or the owned SystemSecureRandom.
  SecureRandomProvider& random() noexcept override;
  /// @brief The network provider. See PlatformFactory::network.
  ///
  /// @return The supplied provider, or the owned DefaultNetworkProvider.
  NetworkProvider& network() noexcept override;
  /// @brief The cryptography provider. See PlatformFactory::crypto.
  ///
  /// @return The provider passed at construction, or the owned
  ///     AbsentCryptoProvider.
  CryptoProvider& crypto() noexcept override;
  /// @brief The HTTP provider. See PlatformFactory::http.
  ///
  /// @return The provider passed at construction, or the owned
  ///     AbsentHttpProvider.
  HttpProvider& http() noexcept override;
  /// @brief The WebSocket provider. See PlatformFactory::websocket.
  ///
  /// @return The provider passed at construction, or the owned
  ///     AbsentWebSocketProvider.
  WebSocketProvider& websocket() noexcept override;
  /// @brief The log sink. See PlatformFactory::diagnostics.
  ///
  /// @return The provider passed at construction, or the owned
  ///     AbsentDiagnosticsProvider.
  DiagnosticsProvider& diagnostics() noexcept override;
  /// @brief The port-mapping provider. See PlatformFactory::gateway.
  ///
  /// @return The provider passed at construction, or the owned
  ///     AbsentGatewayProvider.
  GatewayProvider& gateway() noexcept override {
    return gateway_ != nullptr ? *gateway_ : absent_gateway_;
  }
  /// @brief The DTLS provider. See PlatformFactory::secure_datagram.
  ///
  /// @return The provider passed at construction, or the owned
  ///     AbsentSecureDatagramProvider.
  SecureDatagramProvider& secure_datagram() noexcept override {
    return secure_datagram_ != nullptr ? *secure_datagram_
                                       : absent_secure_datagram_;
  }

 private:
  DefaultThreadingProvider threading_;
  SteadyClock clock_;
  SystemClock wall_clock_;
  SystemSecureRandom random_;
  ThreadingProvider* selected_threading_{&threading_};
  ClockProvider* selected_clock_{&clock_};
  ClockProvider* selected_wall_clock_{&wall_clock_};
  SecureRandomProvider* selected_random_{&random_};
  DefaultNetworkProvider network_{*selected_threading_, *selected_clock_};
  NetworkProvider* selected_network_{&network_};
  CryptoProvider* crypto_{nullptr};
  AbsentCryptoProvider absent_crypto_;
  HttpProvider* http_{nullptr};
  AbsentHttpProvider absent_http_;
  WebSocketProvider* websocket_{nullptr};
  AbsentWebSocketProvider absent_websocket_;
  DiagnosticsProvider* diagnostics_{nullptr};
  AbsentDiagnosticsProvider absent_diagnostics_;
  GatewayProvider* gateway_{nullptr};
  AbsentGatewayProvider absent_gateway_;
  SecureDatagramProvider* secure_datagram_{nullptr};
  AbsentSecureDatagramProvider absent_secure_datagram_;
};

}  // namespace libchiaking::platform
