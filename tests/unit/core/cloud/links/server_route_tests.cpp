#include "tests/support/clock.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/cloud/links/connect_rig.hpp"
#include "tests/support/crypto/refusing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace cloud_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using Reason = ConnectRig::Reason;
using Stage = ConnectRig::Stage;
using Route =
    libchiaking::Expected<remote_play::EstablishedRoute, play::ConnectError>;

class ServerRoute : public PinnedTakionVersions {
 protected:
  void SetUp() override {
    PinnedTakionVersions::SetUp();
    if (IsSkipped()) return;
    selected.name = "dc1";
    selected.measured = true;
    selected.rtt_ms = 40;
    selected.mtu_in = 1454;
    selected.mtu_out = 1254;
    allocation.server_ip = "127.0.0.1";
  }

  auto connect_route(CloudService service = CloudService::legacy_cloud_stream)
      -> Route {
    test_support::FakeClock clock{1'000};
    test_support::FixedRandom random;
    FakeCrypto crypto;
    test_support::RefusingTakionCrypto takion_crypto;
    auto calibration = play::Calibration::from_ping(selected, service);
    if (!calibration) return libchiaking::unexpected(calibration.error());
    return play::ServerConnect{{network, clock,
                                test_support::process_threading(), random,
                                crypto, takion_crypto}}
        .route(allocation, std::move(calibration.value()));
  }

  auto open_peer(platform::IpAddress address)
      -> std::unique_ptr<datagram::ProviderUdpTransport> {
    datagram::UdpTransportConfig config;
    config.local = {address, 0};
    auto created = datagram::ProviderUdpTransport::create(network, config);
    if (!created) return nullptr;
    return std::move(created.value());
  }

  static auto refused_within_two_seconds(datagram::DatagramTransport& transport)
      -> testing::AssertionResult {
    const auto deadline =
        ConnectRig::monotonic().now_microseconds() + 2'000'000;
    while (ConnectRig::monotonic().now_microseconds() < deadline) {
      const auto received = transport.receive();
      if (!received) {
        if (received.error() == datagram::TransportError::io_failure)
          return testing::AssertionSuccess();
        return testing::AssertionFailure() << "the receive failed otherwise";
      }
      if (received.value())
        return testing::AssertionFailure() << "a datagram arrived";
      ConnectRig::pause_briefly();
    }
    return testing::AssertionFailure() << "the closed port was not reported";
  }

  platform::SteadyClock steady;
  platform::DefaultNetworkProvider network{test_support::process_threading(),
                                           steady};
  libchiaking::psn::gk::Allocation allocation;
  libchiaking::psn::gk::Ping selected;
};

TEST_F(ServerRoute, ConnectsAMeasuredRouteToTheAllocatedServer) {
  const auto peer = open_peer(platform::IpAddress::loopback_ipv4());
  ASSERT_NE(peer, nullptr);
  const auto peer_endpoint = peer->local_endpoint();
  ASSERT_TRUE(peer_endpoint);
  allocation.server_port = peer_endpoint.value().port;
  const auto route = connect_route();
  ASSERT_TRUE(route);
  ASSERT_TRUE(route.value().transport);
  EXPECT_EQ(route.value().kind, remote_play::RouteKind::cloud_allocated);
  EXPECT_EQ(route.value().remote_endpoint, peer_endpoint.value());
  EXPECT_TRUE(route.value().local_endpoint.address.is_ipv4());
  EXPECT_NE(route.value().local_endpoint.port, 0);
  EXPECT_EQ(route.value().calibration.takion_version,
            takion::takion_version_legacy_cloud_stream);
  EXPECT_EQ(route.value().calibration.round_trip_time.adjusted_mean,
            std::chrono::microseconds{20'000});
  EXPECT_EQ(route.value().calibration.inbound_mtu, 1454);
  EXPECT_EQ(route.value().calibration.outbound_mtu, 1254);
  EXPECT_TRUE(route.value().transport->close());
}

TEST_F(ServerRoute, CarriesDatagramsBothWays) {
  const auto peer = open_peer(platform::IpAddress::loopback_ipv4());
  ASSERT_NE(peer, nullptr);
  const auto peer_endpoint = peer->local_endpoint();
  ASSERT_TRUE(peer_endpoint);
  allocation.server_port = peer_endpoint.value().port;
  const auto route = connect_route();
  ASSERT_TRUE(route);
  ASSERT_TRUE(route.value().transport);
  const std::array<std::byte, 3> hello{std::byte{1}, std::byte{2},
                                       std::byte{3}};
  const auto writable =
      route.value().transport->wait_writable(std::chrono::milliseconds{100});
  ASSERT_TRUE(writable);
  EXPECT_TRUE(writable.value());
  const auto sent = route.value().transport->send(hello);
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value(), std::size_t{3});
  const auto echoed = ConnectRig::await_datagram(*peer);
  ASSERT_TRUE(echoed);
  EXPECT_EQ(echoed->size(), std::size_t{3});
  EXPECT_TRUE(peer->set_peer(
      route.value().local_endpoint.address.is_ipv4()
          ? platform::SocketEndpoint{platform::IpAddress::loopback_ipv4(),
                                     route.value().local_endpoint.port}
          : route.value().local_endpoint));
  EXPECT_TRUE(peer->send(hello));
  const auto reply = ConnectRig::await_datagram(*route.value().transport);
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->size(), std::size_t{3});
  EXPECT_TRUE(route.value().transport->close());
}

TEST_F(ServerRoute, ReportsAClosedPortLikeAConnectedSocket) {
  const auto closed = open_peer(platform::IpAddress::loopback_ipv4());
  ASSERT_NE(closed, nullptr);
  const auto closed_endpoint = closed->local_endpoint();
  ASSERT_TRUE(closed_endpoint);
  ASSERT_TRUE(closed->close());
  allocation.server_port = closed_endpoint.value().port;
  selected.rtt_ms = 20;
  const auto route = connect_route(CloudService::cloud_stream);
  ASSERT_TRUE(route);
  const std::array<std::byte, 8> probe{};
  EXPECT_TRUE(route.value().transport->send(probe));
  EXPECT_TRUE(refused_within_two_seconds(*route.value().transport));
  EXPECT_TRUE(route.value().transport->close());
}

TEST_F(ServerRoute, ConnectsOverIpv6WhereAvailable) {
  const auto peer_v6 = open_peer(platform::IpAddress::loopback_ipv6());
  if (peer_v6 == nullptr) GTEST_SKIP() << "this host has no IPv6 loopback";
  const auto peer_v6_endpoint = peer_v6->local_endpoint();
  ASSERT_TRUE(peer_v6_endpoint);
  allocation.server_ip = "::1";
  allocation.server_port = peer_v6_endpoint.value().port;
  const auto route = connect_route();
  ASSERT_TRUE(route);
  EXPECT_TRUE(route.value().remote_endpoint.address.is_ipv6());
  EXPECT_TRUE(route.value().local_endpoint.address.is_ipv6());
  EXPECT_EQ(route.value().remote_endpoint.port, peer_v6_endpoint.value().port);
  EXPECT_TRUE(route.value().transport->close());
}

TEST_F(ServerRoute, RefusesAnUnmeasuredDatacenter) {
  selected.measured = false;
  EXPECT_TRUE(ConnectRig::fails_with(connect_route(), Stage::route,
                                     Reason::not_measured));
}

TEST_F(ServerRoute, RefusesAnAllocationWithoutAHost) {
  allocation.server_ip.clear();
  EXPECT_TRUE(ConnectRig::fails_with(connect_route(), Stage::route,
                                     Reason::invalid_host));
}

}  // namespace
}  // namespace cloud_test
