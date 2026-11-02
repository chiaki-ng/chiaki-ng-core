#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/remote_data_route_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <utility>

namespace play_test {
namespace {

namespace datagram = libchiaking::net::transport::datagram;
namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
using Adoption = libchiaking::play::RouteFailure::Adoption;

class RemoteDataRouteAdoption : public data_route_rig::DataRouteRig {};

TEST_F(RemoteDataRouteAdoption, TransfersTheBoundSocketToTheRoute) {
  auto peer = datagram::ProviderUdpTransport::create(
      network,
      datagram::UdpTransportConfig{{platform::IpAddress::loopback_ipv4(), 0}});
  ASSERT_TRUE(peer.has_value());
  const auto peer_endpoint = peer.value()->local_endpoint();
  ASSERT_TRUE(peer_endpoint.has_value());

  auto socket = bound_socket(network);
  ASSERT_NE(socket, nullptr);
  const auto local = socket->local_endpoint();
  ASSERT_TRUE(local.has_value());
  auto route = InternetStart::adopt_data_route(network, std::move(socket),
                                               peer_endpoint.value());
  ASSERT_TRUE(route.has_value());
  ASSERT_TRUE(route.value().transport);
  EXPECT_EQ(route.value().kind, remote_play::RouteKind::remote_rudp);
  EXPECT_EQ(route.value().dont_fragment,
            network.capabilities().socket_dont_fragment_ipv4
                ? remote_play::DontFragmentPolicy::enabled
                : remote_play::DontFragmentPolicy::disabled);
  EXPECT_EQ(route.value().local_endpoint, local.value());
  EXPECT_EQ(route.value().remote_endpoint, peer_endpoint.value());

  const std::array<std::byte, 3> payload{std::byte{7}, std::byte{8},
                                         std::byte{9}};
  const auto sent = route.value().transport->send(payload);
  ASSERT_TRUE(sent.has_value());
  EXPECT_EQ(sent.value(), payload.size());
  const platform::StopToken stop{};
  const auto ready = peer.value()->wait_readable(std::chrono::seconds{1}, stop);
  ASSERT_TRUE(ready.has_value());
  EXPECT_TRUE(ready.value());
  const auto received = peer.value()->receive();
  ASSERT_TRUE(received.has_value());
  ASSERT_TRUE(received.value().has_value());
  EXPECT_TRUE(std::ranges::equal(*received.value(), payload));
}

TEST_F(RemoteDataRouteAdoption, IgnoresQueuedIcmpUnreachableReports) {
  platform::SocketEndpoint closed_endpoint;
  {
    auto closed = bound_socket(network);
    ASSERT_NE(closed, nullptr);
    const auto endpoint = closed->local_endpoint();
    ASSERT_TRUE(endpoint.has_value());
    closed_endpoint = endpoint.value();
    EXPECT_TRUE(closed->close().has_value());
  }
  auto route = InternetStart::adopt_data_route(network, bound_socket(network),
                                               closed_endpoint);
  ASSERT_TRUE(route.has_value());
  const std::array<std::byte, 3> payload{std::byte{1}, std::byte{2},
                                         std::byte{3}};
  for (int index = 0; index < 4; ++index) {
    EXPECT_TRUE(route.value().transport->send(payload).has_value()) << index;
    test_support::LiveProviders::nap(std::chrono::milliseconds{5});
  }
  for (int index = 0; index < 8; ++index) {
    SCOPED_TRACE(index);
    const auto received = route.value().transport->receive();
    ASSERT_TRUE(received.has_value());
    EXPECT_FALSE(received.value().has_value());
  }
}

TEST_F(RemoteDataRouteAdoption, RefusesANullSocket) {
  auto null_result = InternetStart::adopt_data_route(
      network, nullptr, {platform::IpAddress::loopback_ipv4(), 9295});
  ASSERT_FALSE(null_result);
  EXPECT_EQ(null_result.error(), Adoption::null_socket);
}

TEST_F(RemoteDataRouteAdoption, RefusesAnUnspecifiedPeer) {
  auto unspecified = InternetStart::adopt_data_route(
      network, bound_socket(network), {platform::IpAddress::any_ipv4(), 9295});
  ASSERT_FALSE(unspecified);
  EXPECT_EQ(unspecified.error(), Adoption::invalid_peer);
}

TEST_F(RemoteDataRouteAdoption, RefusesAPeerOnPortZero) {
  auto zero_port = InternetStart::adopt_data_route(
      network, bound_socket(network),
      {platform::IpAddress::loopback_ipv4(), 0});
  ASSERT_FALSE(zero_port);
  EXPECT_EQ(zero_port.error(), Adoption::invalid_peer);
}

TEST_F(RemoteDataRouteAdoption, RefusesAPeerOfAnotherAddressFamily) {
  auto mixed = InternetStart::adopt_data_route(
      network, bound_socket(network),
      {platform::IpAddress::loopback_ipv6(), 9295});
  ASSERT_FALSE(mixed);
  EXPECT_EQ(mixed.error(), Adoption::invalid_peer);
}

TEST_F(RemoteDataRouteAdoption, RefusesAnUnboundSocket) {
  auto unbound =
      network.sockets().create_udp_socket(platform::IpAddress::loopback_ipv4());
  ASSERT_TRUE(unbound.has_value());
  auto unbound_result = InternetStart::adopt_data_route(
      network, std::move(unbound.value()),
      {platform::IpAddress::loopback_ipv4(), 9295});
  ASSERT_FALSE(unbound_result);
  EXPECT_EQ(unbound_result.error(), Adoption::unbound_socket);
}

TEST_F(RemoteDataRouteAdoption, RefusesASocketFromAnotherProvider) {
  platform::DefaultNetworkProvider other{test_support::LiveProviders::threading(),
                                         test_support::LiveProviders::steady()};
  auto foreign = InternetStart::adopt_data_route(
      network, bound_socket(other),
      {platform::IpAddress::loopback_ipv4(), 9295});
  ASSERT_FALSE(foreign);
  EXPECT_EQ(foreign.error(), Adoption::foreign_provider);
}

}  // namespace
}  // namespace play_test
