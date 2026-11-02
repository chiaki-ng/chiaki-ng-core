#include "src/core/net/protocol/remote_play/stream/direct_route.hpp"

#include "src/core/net/transport/datagram/udp_transport.hpp"

#include <memory>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

auto DirectRoute::connect(platform::NetworkProvider& network,
                          const DirectRouteConfig& config)
    -> Expected<RouteCandidate, DirectRouteError> {
  if (config.local.address.family() != config.remote.address.family() ||
      config.remote.address.is_unspecified() || config.remote.port == 0)
    return unexpected(DirectRouteError::invalid_configuration);

  DontFragmentPolicy dont_fragment = config.dont_fragment;
  if (dont_fragment == DontFragmentPolicy::enabled) {
    const auto capabilities = network.capabilities();
    const bool supported = config.remote.address.is_ipv4()
                               ? capabilities.socket_dont_fragment_ipv4
                               : capabilities.socket_dont_fragment_ipv6;
    if (!supported) dont_fragment = DontFragmentPolicy::disabled;
  }

  net::transport::datagram::UdpTransportConfig transport_config;
  transport_config.local = config.local;
  transport_config.peer = config.remote;
  if (dont_fragment == DontFragmentPolicy::enabled)
    transport_config.dont_fragment = true;
  auto transport = net::transport::datagram::ProviderUdpTransport::create(
      network, transport_config);
  if (!transport) return unexpected(DirectRouteError::transport_failure);
  const auto local_endpoint = transport.value()->local_endpoint();
  if (!local_endpoint) return unexpected(DirectRouteError::transport_failure);

  return RouteCandidate{std::move(transport.value()), local_endpoint.value(),
                        config.remote, RouteKind::local_direct, dont_fragment};
}

}  // namespace libchiaking::net::protocol::remote_play
