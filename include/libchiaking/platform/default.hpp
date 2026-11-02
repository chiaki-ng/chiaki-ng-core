/// @file
/// @brief Declares DefaultPlatform, the ready-made set of stock providers.
#pragma once

#include "libchiaking/detail/diagnostics/api_capture.hpp"
#include "libchiaking/detail/diagnostics/log_queue.hpp"
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/http/curl/selected_provider.hpp"
#include "libchiaking/platform/http/curl/transport.hpp"

#include <memory>

namespace libchiaking::platform {

/// @brief Owns one stock implementation of every port and the factory that
/// hands them to the library.
///
/// A frontend that has no providers of its own constructs one DefaultPlatform,
/// passes factory() to the library and keeps the object alive for as long as
/// anything built from it exists. It assembles the stock threading, clock,
/// random and network providers, the OpenSSL or BoringSSL CryptoProvider chosen
/// at build time, the curl HttpProvider and WebSocketProvider, a
/// GatewayProvider, and the OpenSSL SecureDatagramProvider when the build
/// includes it.
///
/// Diagnostics go to an in-memory diagnostics::LogQueue that the frontend
/// drains through log(). HTTP calls are traced into that queue and, when
/// enabled, captured in api_capture().
///
/// @note No RecordStore is supplied; the frontend passes its own as
///     ChiakiNg::Stores::records.
/// @ingroup api_platform
class DefaultPlatform {
 public:
  /// @brief Constructs every stock provider and wires them into the factory,
  /// with the default HTTP transport.
  DefaultPlatform();
  /// @brief Constructs every stock provider and wires them into the factory,
  /// with the HTTP transport the settings choose.
  ///
  /// @param settings Which curl HTTP provider runs and its tuning. The
  ///     choice and any fallback are written to log().
  explicit DefaultPlatform(const HttpTransportSettings& settings);
  /// @brief Wires the stock providers onto the host's threading provider and
  /// clocks, with the HTTP transport the settings choose.
  ///
  /// Every thread, lock and timed wait of the stock providers, and of the
  /// library built from factory(), then runs on the host's choice, so a host
  /// that uses one provider set for its own work shares it with the library.
  ///
  /// @param settings Which curl HTTP provider runs and its tuning. The
  ///     choice and any fallback are written to log().
  /// @param threading Threads, mutexes and condition variables. Must outlive
  ///     the platform.
  /// @param clock Monotonic clock. Must outlive the platform.
  /// @param wall_clock Wall clock, counting from the Unix epoch. Must outlive
  ///     the platform.
  DefaultPlatform(const HttpTransportSettings& settings,
                  ThreadingProvider& threading, ClockProvider& clock,
                  ClockProvider& wall_clock);
  /// @brief Not copyable; the factory hands out pointers into this object.
  DefaultPlatform(const DefaultPlatform&) = delete;
  /// @brief Not copy-assignable; the factory hands out pointers into this
  /// object.
  auto operator=(const DefaultPlatform&) -> DefaultPlatform& = delete;
  /// @brief Destroys the providers.
  ///
  /// @pre Nothing created from factory() is still in use.
  ~DefaultPlatform();

  /// @brief The factory to hand to the library.
  ///
  /// @return A reference valid for the lifetime of this DefaultPlatform. Its
  ///     PlatformFactory::diagnostics is log() and its PlatformFactory::http
  ///     is http().
  auto factory() noexcept -> PlatformFactory&;
  /// @brief The queue that receives every diagnostic line the library writes.
  ///
  /// @return The queue to drain with diagnostics::LogQueue::read. Records are
  ///     dropped, and counted, when it is not drained fast enough.
  auto log() noexcept -> diagnostics::LogQueue&;
  /// @brief The capture buffer that records HTTP requests and responses made
  /// through http().
  ///
  /// @return The buffer, disabled until diagnostics::ApiCapture::enable is
  ///     called.
  auto api_capture() noexcept -> diagnostics::ApiCapture&;

  /// @brief The HTTP provider the factory exposes.
  ///
  /// @return The curl provider wrapped so that each call writes a start line
  ///     and a result line to log() and is offered to api_capture().
  auto http() noexcept -> HttpProvider&;
  /// @brief The curl HTTP provider underneath http(), for reading which
  /// transport runs and for dropping connections after sleep or a network
  /// change.
  ///
  /// @return The provider, valid for the lifetime of this object. Requests
  ///     sent through it directly are not traced; send them through http().
  auto http_transport() noexcept -> SelectedCurlHttpProvider&;
  /// @brief The curl WebSocket provider the factory exposes.
  ///
  /// @return The provider, valid for the lifetime of this object.
  auto websocket() noexcept -> WebSocketProvider&;
  /// @brief The cryptography provider the factory exposes.
  ///
  /// @return The OpenSSL provider, or the BoringSSL provider in builds
  ///     configured for it. The BoringSSL provider lacks the optional
  ///     primitives.
  auto crypto() noexcept -> CryptoProvider&;
  /// @brief The stock threading provider, as returned by the factory.
  ///
  /// @return The host's threading provider, or the owned
  ///     DefaultThreadingProvider.
  auto threading() noexcept -> ThreadingProvider&;
  /// @brief The stock monotonic clock, as returned by the factory.
  ///
  /// @return The host's monotonic clock, or the owned SteadyClock.
  auto clock() noexcept -> ClockProvider&;
  /// @brief The stock wall clock, as returned by the factory.
  ///
  /// @return The host's wall clock, or the owned SystemClock.
  auto wall_clock() noexcept -> ClockProvider&;
  /// @brief The operating system random source, as returned by the factory.
  ///
  /// @return The owned SystemSecureRandom.
  auto random() noexcept -> SecureRandomProvider&;
  /// @brief The stock socket and DNS provider, as returned by the factory.
  ///
  /// @return The owned DefaultNetworkProvider.
  auto network() noexcept -> NetworkProvider&;

 private:
  class Providers;

  std::unique_ptr<Providers> providers_;
};

}  // namespace libchiaking::platform
