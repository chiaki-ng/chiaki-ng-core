#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "src/core/net/nat/stun/binding_client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/net/nat/stun/binding_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>

namespace nat_test {
namespace {

namespace stun = libchiaking::net::nat::stun;
namespace pf = libchiaking::platform;

class StunBindingQuery : public StunBindingRig {
 protected:
  void SetUp() override {
    auto made_client =
        network.sockets().create_udp_socket(pf::IpAddress::loopback_ipv4());
    auto made_server =
        network.sockets().create_udp_socket(pf::IpAddress::loopback_ipv4());
    auto made_foreign =
        network.sockets().create_udp_socket(pf::IpAddress::loopback_ipv4());
    ASSERT_TRUE(made_client);
    ASSERT_TRUE(made_server);
    ASSERT_TRUE(made_foreign);
    client = std::move(made_client.value());
    server = std::move(made_server.value());
    foreign = std::move(made_foreign.value());
    for (auto* socket : {client.get(), server.get(), foreign.get()})
      ASSERT_TRUE(socket->bind({pf::IpAddress::loopback_ipv4(), 0}));
    const auto client_ep = client->local_endpoint();
    const auto server_ep = server->local_endpoint();
    ASSERT_TRUE(client_ep);
    ASSERT_TRUE(server_ep);
    client_endpoint = client_ep.value();
    server_endpoint = server_ep.value();
    auto made_poller = network.pollers().create_poller();
    ASSERT_TRUE(made_poller);
    poller = std::move(made_poller.value());
  }

  auto call(std::chrono::nanoseconds timeout, const pf::StopToken& token = {}) {
    return stun::BindingClient{*client, *poller, factory.clock()}.query(
        server_endpoint, transaction, token, timeout);
  }

  auto received_request() -> ::testing::AssertionResult {
    const pf::PollRequest watched[] = {{.pollable = server.get(),
                                        .tag = 1U,
                                        .interest = pf::PollEvent::readable}};
    const auto ready = poller->wait(watched, std::chrono::seconds{1}, stop);
    if (!ready || ready.value().stop_requested || ready.value().ready.empty())
      return ::testing::AssertionFailure() << "the server saw no request";
    const auto received = server->receive_from(request);
    if (!received || received.value().size != request.size())
      return ::testing::AssertionFailure() << "the request is not 20 bytes";
    return ::testing::AssertionSuccess();
  }

  auto send_from_server(std::span<const std::byte> datagram) -> bool {
    return static_cast<bool>(server->send_to(datagram, client_endpoint));
  }

  pf::DefaultPlatformFactory factory;
  pf::NetworkProvider& network = factory.network();
  std::unique_ptr<pf::UdpSocket> client;
  std::unique_ptr<pf::UdpSocket> server;
  std::unique_ptr<pf::UdpSocket> foreign;
  std::unique_ptr<pf::Poller> poller;
  pf::SocketEndpoint client_endpoint{};
  pf::SocketEndpoint server_endpoint{};
  std::array<std::byte, 20> request{};
  const pf::StopToken stop;
};

TEST_F(StunBindingQuery, ReturnsTheMappingAndSendsABindingRequest) {
  ASSERT_TRUE(send_from_server(valid_response()));
  const auto mapped = call(std::chrono::seconds{1}, stop);
  ASSERT_TRUE(mapped);
  EXPECT_EQ(mapped.value(), expected_mapping());
  ASSERT_TRUE(received_request());
  EXPECT_EQ(request, stun::BindingClient::request(transaction));
}

TEST_F(StunBindingQuery, DrainsStaleAndForeignDatagrams) {
  auto valid = valid_response();
  auto stale = valid;
  stale[8] = std::byte{0};
  ASSERT_TRUE(send_from_server(stale));
  ASSERT_TRUE(foreign->send_to(valid, client_endpoint));
  const auto drained = call(std::chrono::seconds{1}, stop);
  ASSERT_TRUE(drained);
  EXPECT_EQ(drained.value(), expected_mapping());
  EXPECT_TRUE(received_request());
}

TEST_F(StunBindingQuery, ReportsAMalformedResponseAsAProtocolError) {
  auto malformed = valid_response();
  malformed[3] = std::byte{9};
  ASSERT_TRUE(send_from_server(malformed));
  const auto bad = call(std::chrono::seconds{1}, stop);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error(), stun::BindingClient::Error::protocol);
  EXPECT_TRUE(received_request());
}

TEST_F(StunBindingQuery, TimesOutWithoutAnyTime) {
  const auto zero = call(std::chrono::nanoseconds{0}, stop);
  ASSERT_FALSE(zero);
  EXPECT_EQ(zero.error(), stun::BindingClient::Error::timeout);
}

TEST_F(StunBindingQuery, StopsBeforeSendingWhenCancelled) {
  const pf::StopToken cancelled{std::make_shared<test_support::Stopped>()};
  const auto before = call(std::chrono::seconds{1}, cancelled);
  ASSERT_FALSE(before);
  EXPECT_EQ(before.error(), stun::BindingClient::Error::cancelled);
}

TEST_F(StunBindingQuery, RefusesAServerWithoutAPortAndKeepsTheSocket) {
  const auto invalid =
      stun::BindingClient{*client, *poller, factory.clock()}.query(
          {server_endpoint.address, 0}, transaction, stop,
          std::chrono::seconds{1});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), stun::BindingClient::Error::invalid_input);
  EXPECT_TRUE(client->local_endpoint());
}

}  // namespace
}  // namespace nat_test
