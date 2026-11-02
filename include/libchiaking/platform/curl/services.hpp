/// @file
/// @brief Declares CurlPlatformServices, a ready-made set of platform providers
/// that uses libcurl for HTTP and WebSocket.
#pragma once

#include <libchiaking/platform/diagnostics/default.hpp>
#include <libchiaking/platform/default_factory.hpp>
#include <libchiaking/platform/http/curl/provider.hpp>
#include <libchiaking/platform/storage/default.hpp>
#include <libchiaking/platform/websocket/curl/provider.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::platform {

/// @brief Owns a working set of stock providers and a PlatformFactory wired to
/// them.
///
/// It is a convenience for hosts that have no providers of their own. It owns a
/// DefaultDiagnosticsProvider, a CurlHttpProvider, a CurlWebSocketProvider,
/// optionally a CryptoProvider, and a DefaultStorageProvider that the factory
/// does not expose; pass storage() to the library as ChiakiNg::Stores::records.
/// It also owns a DefaultThreadingProvider, a SteadyClock and a
/// DefaultNetworkProvider built on them, which the HTTP and WebSocket providers
/// share and the factory returns. The wall clock and random come from the
/// defaults built into DefaultPlatformFactory. No gateway or secure datagram
/// provider is supplied, so PlatformFactory::gateway and
/// PlatformFactory::secure_datagram return stand-ins and the factory does not
/// report either port.
///
/// The factory borrows the providers, so this object must outlive everything
/// created from factory().
class CurlPlatformServices {
 public:
  /// @brief Builds the providers and the factory that refers to them.
  ///
  /// @param storage_root Location handed to DefaultStorageProvider for its
  ///     records.
  /// @param diagnostic_subsystem Subsystem name handed to
  ///     DefaultDiagnosticsProvider.
  /// @param diagnostic_category Category name handed to
  ///     DefaultDiagnosticsProvider.
  /// @param crypto Crypto provider to own and expose. When null, the
  ///     factory does not supply PlatformCapability::crypto,
  ///     PlatformFactory::crypto returns a stand-in and features that need
  ///     cryptography are unavailable.
  explicit CurlPlatformServices(std::string storage_root,
                                std::string_view diagnostic_subsystem,
                                std::string_view diagnostic_category,
                                std::unique_ptr<CryptoProvider> crypto = {})
      : network_(threading_, clock_),
        crypto_(std::move(crypto)),
        storage_(std::move(storage_root)),
        diagnostics_(diagnostic_subsystem, diagnostic_category),
        http_(threading_),
        websocket_(threading_, clock_),
        factory_(PlatformProviders{
            .threading = &threading_,
            .clock = &clock_,
            .network = &network_,
            .crypto = crypto_.get(),
            .http = &http_,
            .websocket = &websocket_,
            .diagnostics = &diagnostics_,
        }) {}

  /// @brief Destroys the factory first, then the providers it borrowed.
  ~CurlPlatformServices() = default;
  /// @brief Not copyable; the factory points at this object's members.
  CurlPlatformServices(const CurlPlatformServices&) = delete;
  /// @brief Not copy-assignable; the factory points at this object's members.
  CurlPlatformServices& operator=(const CurlPlatformServices&) = delete;
  /// @brief Not movable; the factory points at this object's members.
  CurlPlatformServices(CurlPlatformServices&&) = delete;
  /// @brief Not move-assignable; the factory points at this object's members.
  CurlPlatformServices& operator=(CurlPlatformServices&&) = delete;

  /// @brief Returns the factory to hand to the library. It is valid for the
  /// lifetime of this object.
  ///
  /// @return The factory, wired to this object's providers.
  PlatformFactory& factory() noexcept { return factory_; }
  /// @brief Returns the factory for read-only use.
  ///
  /// @return The same factory as the mutable overload.
  const PlatformFactory& factory() const noexcept { return factory_; }
  /// @brief Returns the crypto provider given to the constructor, or null
  /// when none was given.
  ///
  /// @return The owned provider, or nullptr.
  CryptoProvider* crypto() noexcept { return crypto_.get(); }
  /// @brief Returns the crypto provider given to the constructor for
  /// read-only use, or null.
  ///
  /// @return The owned provider, or nullptr.
  const CryptoProvider* crypto() const noexcept { return crypto_.get(); }
  /// @brief Returns the owned record store, to pass as
  /// ChiakiNg::Stores::records.
  ///
  /// @return The store rooted at the constructor's storage_root. The factory
  ///     does not expose it.
  DefaultStorageProvider& storage() noexcept { return storage_; }
  /// @brief Returns the owned diagnostics provider.
  ///
  /// @return The provider that PlatformFactory::diagnostics returns.
  DefaultDiagnosticsProvider& diagnostics() noexcept { return diagnostics_; }
  /// @brief Returns the owned libcurl HTTP provider.
  ///
  /// @return The provider that PlatformFactory::http returns.
  CurlHttpProvider& http() noexcept { return http_; }
  /// @brief Returns the owned libcurl WebSocket provider.
  ///
  /// @return The provider that PlatformFactory::websocket returns.
  CurlWebSocketProvider& websocket() noexcept { return websocket_; }

 private:
  DefaultThreadingProvider threading_;
  SteadyClock clock_;
  DefaultNetworkProvider network_;
  std::unique_ptr<CryptoProvider> crypto_;
  DefaultStorageProvider storage_;
  DefaultDiagnosticsProvider diagnostics_;
  CurlHttpProvider http_;
  CurlWebSocketProvider websocket_;
  /// @brief Declared last so it is destroyed before the providers it borrows.
  DefaultPlatformFactory factory_;
};

}  // namespace libchiaking::platform
