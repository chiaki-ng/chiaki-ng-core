/// @file
/// @brief Declares StreamPlatform, the view of the host's platform a home
/// stream gets when it runs over a network provider of its own.
#pragma once

#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/net/provider.hpp"

#include <memory>

namespace libchiaking::play {

/// @brief A PlatformFactory that hands out the host's providers unchanged,
/// except that the network is the one a single stream was started with.
///
/// Play makes one for a home start whose HomeOptions name a network provider.
/// It is held by the session for its whole life and reaches only that
/// session's link; the start wakes the console over the same provider
/// through consoles::Console::wake, and the consoles context otherwise keeps
/// the host's provider. It refers to the host's
/// platform, which must outlive it, and shares ownership of the stream's
/// provider. The wrapper itself has no mutable state.
class StreamPlatform final : public platform::PlatformFactory {
 public:
  /// @brief Wraps the host's platform for one stream.
  ///
  /// @param platform The host's platform; must outlive the wrapper.
  /// @param network The provider the stream's sockets, pollers and name
  ///     lookups run over; must not be null.
  StreamPlatform(platform::PlatformFactory& platform,
                 std::shared_ptr<platform::NetworkProvider> network) noexcept;
  /// @brief Not copyable; the session refers to it by address.
  StreamPlatform(const StreamPlatform&) = delete;
  /// @brief Not copy-assignable; the session refers to it by address.
  auto operator=(const StreamPlatform&) -> StreamPlatform& = delete;

  /// @brief Describes the host platform.
  ///
  /// @return The host's PlatformInfo.
  auto info() const noexcept -> platform::PlatformInfo override;
  /// @brief Returns the host's threading provider.
  ///
  /// @return The host's provider.
  auto threading() noexcept -> platform::ThreadingProvider& override;
  /// @brief Returns the host's clock.
  ///
  /// @return The host's provider.
  auto clock() noexcept -> platform::ClockProvider& override;
  /// @brief Returns the host's wall clock.
  ///
  /// @return The host's provider.
  auto wall_clock() noexcept -> platform::ClockProvider& override;
  /// @brief Returns the host's random source.
  ///
  /// @return The host's provider.
  auto random() noexcept -> platform::SecureRandomProvider& override;
  /// @brief Returns the stream's own network provider.
  ///
  /// @return The provider given at construction.
  auto network() noexcept -> platform::NetworkProvider& override;
  /// @brief Returns the host's crypto provider.
  ///
  /// @return The host's provider, which is a stand-in when it has none.
  auto crypto() noexcept -> platform::CryptoProvider& override;
  /// @brief Returns the host's HTTP provider.
  ///
  /// @return The host's provider, which is a stand-in when it has none.
  auto http() noexcept -> platform::HttpProvider& override;
  /// @brief Returns the host's WebSocket provider.
  ///
  /// @return The host's provider, which is a stand-in when it has none.
  auto websocket() noexcept -> platform::WebSocketProvider& override;
  /// @brief Returns the host's gateway provider.
  ///
  /// @return The host's provider, which is a stand-in when it has none.
  auto gateway() noexcept -> platform::GatewayProvider& override;
  /// @brief Returns the host's secure datagram provider.
  ///
  /// @return The host's provider, which is a stand-in when it has none.
  auto secure_datagram() noexcept -> platform::SecureDatagramProvider& override;
  /// @brief Returns the host's diagnostics sink.
  ///
  /// @return The host's provider, which is a stand-in when it has none.
  auto diagnostics() noexcept -> platform::DiagnosticsProvider& override;

 private:
  /// @brief The host's platform.
  platform::PlatformFactory& platform_;
  /// @brief The stream's network provider.
  std::shared_ptr<platform::NetworkProvider> network_;
};

}  // namespace libchiaking::play
