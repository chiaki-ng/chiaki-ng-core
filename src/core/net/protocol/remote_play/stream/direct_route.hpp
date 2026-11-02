/// @file
/// @brief Declares DirectRoute, which opens the UDP socket of a home stream
/// straight to the console.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/wire_sizes.hpp"
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/ports.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"
#include "src/core/net/protocol/remote_play/stream/local_settings.hpp"
#include "src/core/net/protocol/remote_play/stream/route.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace libchiaking::net::protocol::remote_play {

/// @brief Why a direct route could not be opened.
enum class DirectRouteError : std::uint8_t {
  /// @brief The addresses are of different families, or the console's address
  /// or port is unset.
  invalid_configuration,
  /// @brief Reserved; an unsupported don't-fragment flag is dropped instead.
  unsupported_capability,
  /// @brief The socket could not be created or reports no local endpoint.
  transport_failure,
};

/// @brief Where a direct route goes and how its socket is set up.
///
/// Plain data.
struct DirectRouteConfig {
  /// @brief The local endpoint to bind; an unspecified address and port 0 let
  /// the platform choose.
  platform::SocketEndpoint local{};
  /// @brief The console's endpoint.
  platform::SocketEndpoint remote{};
  /// @brief Whether to set don't-fragment; dropped when the platform cannot
  /// for the address family.
  DontFragmentPolicy dont_fragment{DontFragmentPolicy::enabled};
};

/// @brief Opens direct routes.
///
/// Stateless; every member is static.
class DirectRoute {
 public:
  /// @brief Opens a UDP socket to the console.
  ///
  /// Does not block; nothing is sent.
  ///
  /// @param network The network provider.
  /// @param config Where to go.
  /// @return The route, kind RouteKind::local_direct.
  /// @retval DirectRouteError::invalid_configuration The endpoints are
  ///     unusable.
  /// @retval DirectRouteError::transport_failure The socket failed.
  [[nodiscard]] static auto connect(platform::NetworkProvider& network,
                                    const DirectRouteConfig& config)
      -> Expected<RouteCandidate, DirectRouteError>;
};

}  // namespace libchiaking::net::protocol::remote_play
