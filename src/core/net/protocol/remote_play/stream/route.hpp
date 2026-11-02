/// @file
/// @brief The UDP route a stream runs on: the owned datagram transport, its
/// endpoints, how it was reached, and, once calibrated, the Senkusha result
/// that sized it.
#pragma once

#include "libchiaking/platform/net/types.hpp"
#include "src/core/net/protocol/senkusha/types.hpp"
#include "src/core/net/transport/datagram/transport.hpp"

#include <concepts>
#include <cstddef>
#include <memory>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

/// @brief How a route to the console was made.
enum class RouteKind : std::uint8_t {
  /// @brief A UDP socket straight to the console on the home network.
  local_direct,
  /// @brief A hole-punched route through PSN, carrying RUDP.
  remote_rudp,
  /// @brief A route to a cloud server the service allocated.
  cloud_allocated,
};

/// @brief Whether the route's socket sets the IP don't-fragment flag.
enum class DontFragmentPolicy : std::uint8_t {
  /// @brief Packets may be fragmented.
  disabled,
  /// @brief Packets carry don't-fragment, so MTU probing measures the path.
  enabled,
};

/// @brief Owns a route's datagram transport and closes it when dropped.
///
/// Move-only; moving into a route that holds a transport closes that one
/// first. Not thread-safe.
class RouteTransport {
 public:
  /// @brief The transport interface held.
  using Transport = net::transport::datagram::DatagramTransport;

  /// @brief Holds no transport.
  RouteTransport() noexcept = default;
  /// @brief Holds no transport.
  RouteTransport(std::nullptr_t) noexcept {}
  /// @brief Takes ownership of a transport.
  ///
  /// @tparam Concrete A DatagramTransport implementation.
  /// @param transport The transport.
  template <std::derived_from<Transport> Concrete>
  RouteTransport(std::unique_ptr<Concrete> transport) noexcept
      : transport_(std::move(transport)) {}
  RouteTransport(const RouteTransport&) = delete;
  RouteTransport& operator=(const RouteTransport&) = delete;
  /// @brief Takes over another route's transport.
  ///
  /// @param other The route to take from; left empty.
  RouteTransport(RouteTransport&& other) noexcept
      : transport_(std::move(other.transport_)) {}
  /// @brief Closes the held transport, then takes over another's.
  ///
  /// @param other The route to take from; left empty.
  /// @return This route.
  RouteTransport& operator=(RouteTransport&& other) noexcept {
    if (this != &other) {
      reset();
      transport_ = std::move(other.transport_);
    }
    return *this;
  }
  /// @brief Closes the held transport.
  ~RouteTransport() { reset(); }

  /// @brief Closes and releases the transport.
  ///
  /// @return Nothing when none was held or it closed; the transport's error
  ///     otherwise. It is released either way.
  [[nodiscard]] auto close()
      -> Expected<void, net::transport::datagram::TransportError> {
    if (!transport_) return {};
    const auto transport = std::move(transport_);
    return transport->close();
  }

  /// @brief Closes the transport, ignoring any error.
  void reset() noexcept { static_cast<void>(close()); }

  /// @brief Gives up ownership without closing.
  ///
  /// @return The transport, or null.
  auto release() noexcept -> std::unique_ptr<Transport> {
    return std::move(transport_);
  }

  /// @brief Gives up ownership without closing.
  ///
  /// @return The transport, or null.
  operator std::unique_ptr<Transport>() && noexcept { return release(); }

  /// @brief The transport, still owned here.
  ///
  /// @return The transport, or null.
  auto get() const noexcept -> Transport* { return transport_.get(); }

  /// @brief Calls through to the transport.
  ///
  /// @return The transport; must not be null.
  auto operator->() const noexcept -> Transport* { return transport_.get(); }

  /// @brief The transport.
  ///
  /// @return The transport; must not be null.
  auto operator*() const noexcept -> Transport& { return *transport_; }

  /// @brief Whether a transport is held.
  ///
  /// @return True when held.
  explicit operator bool() const noexcept { return transport_ != nullptr; }

  /// @brief Whether no transport is held.
  ///
  /// @param route The route.
  /// @return True when empty.
  friend auto operator==(const RouteTransport& route, std::nullptr_t) noexcept
      -> bool {
    return route.transport_ == nullptr;
  }

 private:
  /// @brief The transport.
  std::unique_ptr<Transport> transport_;
};

/// @brief A route that has been opened but not yet calibrated.
///
/// Move-only through its transport.
struct RouteCandidate {
  /// @brief The transport.
  RouteTransport transport;
  /// @brief The local socket's endpoint.
  platform::SocketEndpoint local_endpoint;
  /// @brief The console's endpoint.
  platform::SocketEndpoint remote_endpoint;
  /// @brief How the route was made.
  RouteKind kind{RouteKind::local_direct};
  /// @brief Whether packets carry don't-fragment.
  DontFragmentPolicy dont_fragment{DontFragmentPolicy::disabled};
};

/// @brief A route that Senkusha calibrated, ready for the stream.
struct EstablishedRoute : RouteCandidate {
  /// @brief The measured MTU, round trip and Takion version to use.
  senkusha::Result calibration;
};

}  // namespace libchiaking::net::protocol::remote_play
