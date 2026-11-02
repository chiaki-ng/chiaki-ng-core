/// @file
/// @brief Declares what the ICE agent reports, the path it nominates and
/// why it fails.
#pragma once

#include "libchiaking/platform/net/types.hpp"
#include "src/core/net/nat/ice/parameters.hpp"

#include <cstdint>
#include <variant>

namespace libchiaking::net::nat::ice {

/// @brief The caller's name for one local UDP socket; the agent never touches
/// sockets itself.
using SocketId = std::uint32_t;

/// @brief Why the agent failed.
enum class Error : std::uint8_t {
  /// @brief connect() was called out of order, or a message could not be
  /// encoded.
  protocol,
  /// @brief No usable path: no base, the host's deadline passed, a mapping
  /// changed, or consent expired.
  no_path,
  /// @brief Randomness or HMAC-SHA1 is unavailable.
  crypto_unavailable,
  /// @brief An invariant failed, such as duplicate sockets.
  internal,
};

/// @brief The nominated pair: the route the session's traffic must use.
struct Path {
  /// @brief The local socket.
  SocketId socket{};
  /// @brief The local socket's address as the peer sees it.
  platform::SocketEndpoint mapped;
  /// @brief The peer's address and port.
  platform::SocketEndpoint remote;
};

/// @brief What the agent reports through Agent::take_event().
namespace event {
/// @brief Gathering finished; these parameters go into the local block.
struct Gathered {
  /// @brief The local credentials, up to 32 candidates and the port-guess
  /// count.
  Parameters local;
};
/// @brief A pair was nominated; the session may start.
struct Nominated {
  /// @brief The nominated pair.
  Path path;
};
/// @brief The agent failed and does nothing more.
struct Failed {
  /// @brief Why.
  Error error{Error::no_path};
};
}  // namespace event

/// @brief One report from the agent.
using Event = std::variant<event::Gathered, event::Nominated, event::Failed>;

}  // namespace libchiaking::net::nat::ice
