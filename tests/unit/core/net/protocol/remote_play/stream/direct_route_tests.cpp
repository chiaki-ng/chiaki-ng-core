#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/stream/direct_route.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace datagram = libchiaking::net::transport::datagram;

class DirectRouteNetworkWithoutDontFragment final
    : public platform::NetworkProvider {
 public:
  DirectRouteNetworkWithoutDontFragment(platform::ThreadingProvider& threading,
                                        const platform::ClockProvider& clock)
      : inner_{threading, clock} {}

  [[nodiscard]] platform::SocketProvider& sockets() noexcept override {
    return inner_.sockets();
  }
  [[nodiscard]] platform::PollerProvider& pollers() noexcept override {
    return inner_.pollers();
  }
  [[nodiscard]] platform::DnsProvider& dns() noexcept override {
    return inner_.dns();
  }
  [[nodiscard]] platform::InterfacesProvider& interfaces() noexcept override {
    return inner_.interfaces();
  }
  [[nodiscard]] platform::NetworkCapabilities capabilities()
      const noexcept override {
    return {};
  }

 private:
  platform::DefaultNetworkProvider inner_;
};

class DirectRouteConnect : public ::testing::Test {
 protected:
  void open_peer(platform::NetworkProvider& provider) {
    datagram::UdpTransportConfig peer_config;
    peer_config.local = {platform::IpAddress::loopback_ipv4(), 0};
    auto created =
        datagram::ProviderUdpTransport::create(provider, peer_config);
    ASSERT_TRUE(created);
    peer = std::move(created).value();
    const auto endpoint = peer->local_endpoint();
    ASSERT_TRUE(endpoint);
    peer_endpoint = endpoint.value();
  }

  platform::DefaultThreadingProvider& threading =
      test_support::process_threading();
  const platform::SteadyClock steady;
  platform::DefaultNetworkProvider network{threading, steady};
  std::unique_ptr<datagram::ProviderUdpTransport> peer;
  platform::SocketEndpoint peer_endpoint{};
};

TEST_F(DirectRouteConnect, OwnsAConnectedTransport) {
  ASSERT_NO_FATAL_FAILURE(open_peer(network));
  remote_play::DirectRouteConfig config;
  config.local = {platform::IpAddress::loopback_ipv4(), 0};
  config.remote = peer_endpoint;
  config.dont_fragment = remote_play::DontFragmentPolicy::disabled;
  auto route = remote_play::DirectRoute::connect(network, config);
  ASSERT_TRUE(route);
  EXPECT_EQ(route.value().kind, remote_play::RouteKind::local_direct);
  EXPECT_NE(route.value().local_endpoint.port, 0);
  EXPECT_EQ(route.value().remote_endpoint, peer_endpoint);

  const std::array<std::byte, 3> payload{std::byte{1}, std::byte{2},
                                         std::byte{3}};
  const auto sent = route.value().transport->send(payload);
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value(), payload.size());
  platform::StopToken token;
  const auto ready = peer->wait_readable(std::chrono::seconds{1}, token);
  ASSERT_TRUE(ready);
  EXPECT_TRUE(ready.value());
  const auto received = peer->receive();
  ASSERT_TRUE(received);
  ASSERT_TRUE(received.value());
  EXPECT_TRUE(std::ranges::equal(*received.value(), payload));
}

TEST_F(DirectRouteConnect, RejectsARemoteWithoutAPortBeforeOpeningASocket) {
  remote_play::DirectRouteConfig missing_port;
  missing_port.local = {platform::IpAddress::loopback_ipv4(), 0};
  missing_port.remote = {platform::IpAddress::loopback_ipv4(), 0};
  EXPECT_FALSE(remote_play::DirectRoute::connect(network, missing_port));
}

TEST_F(DirectRouteConnect, RejectsMixedAddressFamiliesBeforeOpeningASocket) {
  remote_play::DirectRouteConfig mixed_family;
  mixed_family.local = {platform::IpAddress::loopback_ipv4(), 0};
  mixed_family.remote = {platform::IpAddress::loopback_ipv6(), 9295};
  EXPECT_FALSE(remote_play::DirectRoute::connect(network, mixed_family));
}

TEST_F(DirectRouteConnect, UsesDontFragmentOnIpv4WhenTheNetworkAdvertisesIt) {
  if (!network.capabilities().socket_dont_fragment_ipv4)
    GTEST_SKIP() << "the network cannot set dont-fragment on IPv4";
  remote_play::DirectRouteConfig config;
  config.local = {platform::IpAddress::any_ipv4(), 0};
  config.remote = {platform::IpAddress::loopback_ipv4(), 9297};
  config.dont_fragment = remote_play::DontFragmentPolicy::enabled;
  auto route = remote_play::DirectRoute::connect(network, config);
  ASSERT_TRUE(route);
  EXPECT_TRUE(route.value().transport.close());
}

TEST_F(DirectRouteConnect,
       FallsBackToFragmentingWhenDontFragmentIsUnsupported) {
  DirectRouteNetworkWithoutDontFragment limited{threading, steady};
  ASSERT_NO_FATAL_FAILURE(open_peer(limited));
  remote_play::DirectRouteConfig config;
  config.local = {platform::IpAddress::loopback_ipv4(), 0};
  config.remote = peer_endpoint;
  config.dont_fragment = remote_play::DontFragmentPolicy::enabled;
  auto route = remote_play::DirectRoute::connect(limited, config);
  ASSERT_TRUE(route);
  EXPECT_EQ(route.value().dont_fragment,
            remote_play::DontFragmentPolicy::disabled);

  const std::array<std::byte, 2> payload{std::byte{7}, std::byte{9}};
  const auto sent = route.value().transport->send(payload);
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value(), payload.size());
  platform::StopToken token;
  const auto ready = peer->wait_readable(std::chrono::seconds{1}, token);
  ASSERT_TRUE(ready);
  EXPECT_TRUE(ready.value());
  EXPECT_TRUE(route.value().transport.close());
}

}  // namespace
}  // namespace remote_play_test
