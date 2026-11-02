#include "tests/unit/core/net/transport/datagram/loopback_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace datagram_test {
namespace {

namespace datagram = libchiaking::net::transport::datagram;
namespace pf = libchiaking::platform;
using datagram::TransportError;

class UdpTransportLoopback : public UdpLoopbackRig {
 protected:
  static constexpr int repetitions = 25;

  static auto large_payload() -> Bytes {
    Bytes large(8000);
    for (std::size_t index = 0; index < large.size(); ++index)
      large[index] = static_cast<std::byte>(index & 0xff);
    return large;
  }

  const Bytes small{std::byte{1}, std::byte{2}, std::byte{3}};
  const Bytes medium{std::byte{9}, std::byte{8}};
  const Bytes reply{std::byte{42}};
};

TEST_F(UdpTransportLoopback, BindsAnEphemeralPortAndStartsIdle) {
  for (int round = 0; round < repetitions; ++round) {
    SCOPED_TRACE(round);
    ASSERT_NO_FATAL_FAILURE(open_pair());
    const auto idle = first->wait_readable(std::chrono::milliseconds{20}, stop);
    ASSERT_TRUE(idle);
    EXPECT_FALSE(idle.value());
    const auto nothing = first->receive();
    ASSERT_TRUE(nothing);
    EXPECT_FALSE(nothing.value().has_value());
  }
}

TEST_F(UdpTransportLoopback, CarriesDatagramsBothWays) {
  for (int round = 0; round < repetitions; ++round) {
    SCOPED_TRACE(round);
    ASSERT_NO_FATAL_FAILURE(open_pair());
    EXPECT_TRUE(delivers(*first, *second, small));
    EXPECT_TRUE(delivers(*first, *second, medium));
    EXPECT_TRUE(delivers(*second, *first, reply));
    EXPECT_TRUE(delivers(*first, *second, large_payload()));
  }
}

TEST_F(UdpTransportLoopback, RefusesADatagramOverTheLimit) {
  for (int round = 0; round < repetitions; ++round) {
    SCOPED_TRACE(round);
    ASSERT_NO_FATAL_FAILURE(open_pair());
    const auto refused = first->send(Bytes(70000));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error(), TransportError::io_failure);
  }
}

TEST_F(UdpTransportLoopback, DropsADatagramFromAStranger) {
  for (int round = 0; round < repetitions; ++round) {
    SCOPED_TRACE(round);
    ASSERT_NO_FATAL_FAILURE(open_pair());
    auto bystander = factory.network().sockets().create_udp_socket(
        pf::IpAddress::loopback_ipv4());
    ASSERT_TRUE(bystander);
    ASSERT_TRUE(bystander.value()->bind(
        pf::SocketEndpoint{pf::IpAddress::loopback_ipv4(), 0}));
    ASSERT_TRUE(bystander.value()->send_to(medium, first_at));
    ASSERT_TRUE(ready(*first, stop));
    const auto filtered = first->receive();
    ASSERT_TRUE(filtered);
    EXPECT_FALSE(filtered.value().has_value());
    EXPECT_TRUE(delivers(*first, *second, small));
  }
}

TEST_F(UdpTransportLoopback, RefusesEveryCallOnceClosed) {
  for (int round = 0; round < repetitions; ++round) {
    SCOPED_TRACE(round);
    ASSERT_NO_FATAL_FAILURE(open_pair());
    EXPECT_TRUE(first->close());
    EXPECT_TRUE(first->close());
    const auto sent = first->send(small);
    ASSERT_FALSE(sent);
    EXPECT_EQ(sent.error(), TransportError::closed);
    const auto received = first->receive();
    ASSERT_FALSE(received);
    EXPECT_EQ(received.error(), TransportError::closed);
    const auto waited =
        first->wait_readable(std::chrono::milliseconds{0}, stop);
    ASSERT_FALSE(waited);
    EXPECT_EQ(waited.error(), TransportError::closed);
    const auto endpoint = first->local_endpoint();
    ASSERT_FALSE(endpoint);
    EXPECT_EQ(endpoint.error(), TransportError::closed);
  }
}

TEST_F(UdpTransportLoopback, ReportsTheTrafficClassOnlyOnceEnabled) {
  ASSERT_NO_FATAL_FAILURE(open_pair());
  const Bytes payload{std::byte{0x02}, std::byte{0x03}};
  ASSERT_TRUE(first->send(payload));
  ASSERT_TRUE(ready(*second, stop));
  const auto plain = second->receive();
  ASSERT_TRUE(plain);
  EXPECT_TRUE(plain.value().has_value());
  EXPECT_FALSE(second->last_traffic_class().has_value());
  ASSERT_TRUE(second->enable_traffic_class());
  for (int round = 0; round < 3; ++round) {
    SCOPED_TRACE(round);
    EXPECT_TRUE(delivers(*first, *second, payload));
    EXPECT_EQ(second->last_traffic_class(), std::optional<std::uint8_t>{0});
  }
}

class CountingPollers final : public pf::PollerProvider {
 public:
  explicit CountingPollers(pf::PollerProvider& inner) noexcept
      : inner_(&inner) {}

  [[nodiscard]] auto create_poller()
      -> libchiaking::Expected<std::unique_ptr<pf::Poller>,
                               pf::NetError> override {
    ++created;
    return inner_->create_poller();
  }

  int created{0};

 private:
  pf::PollerProvider* inner_;
};

class CountingNetwork final : public pf::NetworkProvider {
 public:
  explicit CountingNetwork(pf::NetworkProvider& inner) noexcept
      : inner_(&inner), pollers_(inner.pollers()) {}

  pf::SocketProvider& sockets() noexcept override { return inner_->sockets(); }
  pf::PollerProvider& pollers() noexcept override { return pollers_; }
  pf::DnsProvider& dns() noexcept override { return inner_->dns(); }
  pf::InterfacesProvider& interfaces() noexcept override {
    return inner_->interfaces();
  }
  pf::NetworkCapabilities capabilities() const noexcept override {
    return inner_->capabilities();
  }

  int pollers_created() const noexcept { return pollers_.created; }

 private:
  pf::NetworkProvider* inner_;
  CountingPollers pollers_;
};

TEST_F(UdpTransportLoopback, KeepsOnePollerAcrossWaits) {
  CountingNetwork network{factory.network()};
  auto transport = Transport::create(network, loopback_config());
  ASSERT_TRUE(transport);
  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto readable =
        transport.value()->wait_readable(std::chrono::milliseconds{1}, stop);
    ASSERT_TRUE(readable);
    EXPECT_FALSE(readable.value());
  }
  EXPECT_EQ(network.pollers_created(), 1);
  EXPECT_NE(transport.value()->pollable(), nullptr);
  EXPECT_TRUE(transport.value()->close());
  EXPECT_EQ(transport.value()->pollable(), nullptr);
}

TEST_F(UdpTransportLoopback, RaisesASmallReceiveBuffer) {
  auto& network = factory.network();
  auto small_socket =
      network.sockets().create_udp_socket(pf::IpAddress::loopback_ipv4());
  ASSERT_TRUE(small_socket);
  ASSERT_TRUE(small_socket.value()->set_receive_buffer_size(4096));
  auto* socket = small_socket.value().get();
  auto transport =
      Transport::adopt(network, std::move(small_socket.value()), std::nullopt);
  ASSERT_TRUE(transport);
  const auto raised = socket->receive_buffer_size();
  ASSERT_TRUE(raised);
  EXPECT_GE(raised.value(), datagram::socket_receive_buffer);
}

TEST_F(UdpTransportLoopback, KeepsALargerReceiveBuffer) {
  constexpr std::size_t large_window =
      4 * std::size_t{datagram::socket_receive_buffer};
  auto& network = factory.network();
  auto large_socket =
      network.sockets().create_udp_socket(pf::IpAddress::loopback_ipv4());
  ASSERT_TRUE(large_socket);
  ASSERT_TRUE(large_socket.value()->set_receive_buffer_size(large_window));
  auto* socket = large_socket.value().get();
  const auto before = socket->receive_buffer_size();
  ASSERT_TRUE(before);
  ASSERT_GE(before.value(), large_window);
  auto transport =
      Transport::adopt(network, std::move(large_socket.value()), std::nullopt);
  ASSERT_TRUE(transport);
  const auto after = socket->receive_buffer_size();
  ASSERT_TRUE(after);
  EXPECT_EQ(after.value(), before.value());
}

}  // namespace
}  // namespace datagram_test
