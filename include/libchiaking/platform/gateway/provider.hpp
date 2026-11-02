/// @file
/// @brief Declares the gateway port, through which the library asks the home
/// router to forward a UDP port for an internet session.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/net/poller.hpp>
#include <libchiaking/platform/net/types.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <chrono>
#include <memory>

namespace libchiaking::platform {

/// @brief Default limit on finding the gateway, and the stock implementation's
/// limit on creating one mapping.
inline constexpr std::chrono::seconds default_gateway_timeout{7};

/// @brief Reason a gateway operation failed.
enum class GatewayError {
  /// @brief No gateway that supports port mapping was found, or the session
  /// has been closed.
  unavailable,
  /// @brief The stop token was stopped.
  cancelled,
  /// @brief The gateway did not answer within the time allowed.
  timeout,
  /// @brief The gateway was found but refused the request or could not be
  /// talked to.
  network,
  /// @brief The gateway answered with an address that is not a usable IPv4
  /// address. The stock implementation also returns it from
  /// GatewaySession::map_udp when asked to map port 0.
  invalid_response
};

/// @brief A conversation with one discovered gateway, through which ports are
/// mapped and later released.
///
/// Implemented by the host frontend, or by the miniupnpc-based session that
/// make_gateway_provider() supplies. The library owns it through the
/// `std::unique_ptr` returned by GatewayProvider::discover and calls close()
/// when it no longer needs the mapping.
class GatewaySession {
 public:
  /// @brief Releases the session. The stock session asks the gateway to
  /// remove its mappings in the background if close() was not called, without
  /// waiting for the result.
  virtual ~GatewaySession() = default;
  /// @brief Returns this host's LAN address as the gateway sees it.
  ///
  /// The library advertises it to the peer as the local candidate address.
  /// The stock session always returns an IPv4 address.
  ///
  /// @return The address discovery recorded; it does not change and the call
  ///     does not block.
  virtual auto local_address() const -> IpAddress = 0;
  /// @brief Asks the gateway to forward a public UDP port to the same port on
  /// this host, blocking until it answers.
  ///
  /// @param port The local UDP port, which is also requested as the public
  ///     port. Must not be 0.
  /// @param stop Cancels the wait when stopped.
  /// @return The public address and port that now reach the socket. The
  ///     mapping lasts until close() or destruction removes it.
  /// @retval GatewayError::cancelled The stop token was stopped.
  /// @retval GatewayError::timeout The gateway did not answer in time; the
  ///     stock session allows default_gateway_timeout.
  /// @retval GatewayError::network The gateway refused the mapping or would
  ///     not report its public address.
  /// @retval GatewayError::invalid_response The gateway's public address is
  ///     not a usable IPv4 address, or port is 0.
  /// @retval GatewayError::unavailable The session has been closed.
  ///
  /// @note When this fails the library falls back to discovering its public
  ///     endpoint through STUN.
  [[nodiscard]] virtual auto map_udp(std::uint16_t port, const StopToken& stop)
      -> Expected<SocketEndpoint, GatewayError> = 0;
  /// @brief Removes every mapping made through this session and ends it,
  /// blocking until the gateway answers.
  ///
  /// After close(), map_udp() returns GatewayError::unavailable. The stock
  /// session waits at most three seconds.
  ///
  /// @return Nothing when every mapping was removed.
  /// @retval GatewayError::network The gateway refused to remove a mapping.
  /// @retval GatewayError::timeout The gateway did not answer in time.
  [[nodiscard]] virtual auto close() -> Expected<void, GatewayError> = 0;
};

/// @brief Port that finds a gateway able to map ports, such as a UPnP router.
///
/// Implemented by the host frontend, or by the provider that
/// make_gateway_provider() returns. The host passes it in
/// PlatformProviders::gateway. The port is optional: when the factory does not
/// supply PlatformCapability::gateway_mapping the library skips port mapping and
/// relies on STUN and hole punching alone.
///
/// The library calls discover() at the start of an internet connection attempt.
class GatewayProvider {
 public:
  /// @brief Allows destruction through the interface.
  virtual ~GatewayProvider() = default;
  /// @brief Searches the local network for a gateway, blocking until one is
  /// found, the search fails or the time runs out.
  ///
  /// @param stop Cancels the search when stopped.
  /// @param timeout Longest time to wait for the search.
  /// @return A session with the gateway, owned by the caller.
  /// @retval GatewayError::unavailable No gateway that supports port mapping
  ///     answered.
  /// @retval GatewayError::cancelled The stop token was stopped.
  /// @retval GatewayError::timeout The search outlasted timeout.
  /// @retval GatewayError::network The search itself failed.
  /// @retval GatewayError::invalid_response The gateway reported a LAN
  ///     address for this host that is not a usable IPv4 address.
  ///
  /// @note Any failure makes the library continue the connection attempt
  ///     without a port mapping.
  [[nodiscard]] virtual auto discover(
      const StopToken& stop,
      std::chrono::milliseconds timeout = default_gateway_timeout)
      -> Expected<std::unique_ptr<GatewaySession>, GatewayError> = 0;
};

/// @brief Creates the stock gateway provider.
///
/// @param threading Starts the provider's worker thread and makes its locks;
///     must outlive the provider and every session it creates.
/// @param clock The monotonic clock request timeouts are measured on; must
///     outlive the provider and every session it creates.
/// @return A provider that speaks UPnP through miniupnpc and runs gateway
///     requests on its own worker thread, or null when the library was built
///     without miniupnpc. A null result means there is no gateway port to give
///     to PlatformProviders::gateway.
auto make_gateway_provider(ThreadingProvider& threading,
                           const ClockProvider& clock)
    -> std::unique_ptr<GatewayProvider>;

}  // namespace libchiaking::platform
