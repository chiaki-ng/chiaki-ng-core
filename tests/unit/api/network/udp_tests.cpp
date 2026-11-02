#include "tests/unit/api/network/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace network_test {
namespace {

using InMemoryUdp = InMemoryNetworkTest;

TEST_F(InMemoryUdp, DeliversOverLoopbackWithTheSenderAsSource) {
  auto receiver = udp(network, loopback());
  auto sender = udp(network, loopback());
  const auto target = receiver->local_endpoint().value();
  std::array<std::byte, 16> buffer{};
  const auto empty = receiver->receive_from(buffer);
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), platform::NetError::would_block);
  EXPECT_EQ(sender->send_to(bytes("ping"), target).value(), 4U);
  const auto received = receiver->receive_from(buffer);
  ASSERT_TRUE(received);
  EXPECT_EQ(received.value().size, 4U);
  EXPECT_EQ(text(std::span{buffer}.first(4)), "ping");
  EXPECT_EQ(received.value().source, sender->local_endpoint().value());
  ASSERT_EQ(network.sent().size(), 1U);
  EXPECT_EQ(network.sent().front(), target);
  EXPECT_TRUE(sender->close());
  EXPECT_TRUE(sender->close());
  const auto after_close = sender->send_to(bytes("x"), target);
  ASSERT_FALSE(after_close);
  EXPECT_EQ(after_close.error(), platform::NetError::closed);
}

TEST_F(InMemoryUdp, WildcardReceivesWhatIsSentToAnInterfaceAddress) {
  auto wildcard = udp(network, {platform::IpAddress::any_ipv4(), 0});
  auto sender = udp(network, loopback());
  const auto port = wildcard->local_endpoint().value().port;
  std::array<std::byte, 8> buffer{};
  for (const auto address : {platform::IpAddress::loopback_ipv4(),
                             platform::IpAddress::ipv4({192, 0, 2, 10})}) {
    EXPECT_TRUE(sender->send_to(bytes("hi"), {address, port}));
    const auto received = wildcard->receive_from(buffer);
    ASSERT_TRUE(received);
    EXPECT_EQ(received.value().size, 2U);
  }
  EXPECT_TRUE(sender->send_to(bytes("hi"),
                              {platform::IpAddress::loopback_ipv6(), port}));
  EXPECT_EQ(wildcard->receive_from(buffer).error(),
            platform::NetError::would_block);
}

TEST_F(InMemoryUdp, TruncatesToTheReceiveBuffer) {
  auto receiver = udp(network, loopback());
  auto sender = udp(network, loopback());
  EXPECT_TRUE(
      sender->send_to(bytes("0123456789"), receiver->local_endpoint().value()));
  std::array<std::byte, 4> small{};
  const auto received = receiver->receive_from(small);
  ASSERT_TRUE(received);
  EXPECT_EQ(received.value().size, 4U);
  EXPECT_EQ(text(small), "0123");
  EXPECT_EQ(receiver->receive_from(small).error(),
            platform::NetError::would_block);
}

TEST_F(InMemoryUdp, BroadcastReachesWildcardSocketsOnly) {
  const auto interfaces = network.interfaces().enumerate();
  ASSERT_TRUE(interfaces);
  ASSERT_EQ(interfaces.value().size(), 1U);
  const auto& wired = interfaces.value().front();
  EXPECT_TRUE(wired.has_broadcast);
  EXPECT_TRUE(wired.operational);
  auto wildcard = udp(network, {platform::IpAddress::any_ipv4(), 0});
  auto bound = udp(network, loopback());
  auto sender = udp(network, loopback());
  const platform::SocketEndpoint broadcast{
      wired.broadcast, wildcard->local_endpoint().value().port};
  EXPECT_TRUE(sender->send_to(bytes("who"), broadcast));
  std::array<std::byte, 8> buffer{};
  EXPECT_EQ(wildcard->receive_from(buffer).value().size, 3U);
  EXPECT_EQ(bound->receive_from(buffer).error(),
            platform::NetError::would_block);
  EXPECT_EQ(network.sent().back(), broadcast);
}

TEST_F(InMemoryUdp, BindsOnlyFreeAvailableAddressesOnce) {
  auto first = udp(network, loopback());
  const auto port = first->local_endpoint().value().port;
  auto second =
      network.sockets().create_udp_socket(platform::IpAddress::loopback_ipv4());
  ASSERT_TRUE(second);
  EXPECT_EQ(second.value()->bind(loopback(port)).error(),
            platform::NetError::address_in_use);
  EXPECT_EQ(
      second.value()->bind({platform::IpAddress::any_ipv4(), port}).error(),
      platform::NetError::address_in_use);
  EXPECT_EQ(second.value()
                ->bind({platform::IpAddress::ipv4({203, 0, 113, 9}), 0})
                .error(),
            platform::NetError::address_not_available);
  EXPECT_EQ(
      second.value()->bind({platform::IpAddress::loopback_ipv6(), 0}).error(),
      platform::NetError::invalid_argument);
  EXPECT_EQ(second.value()->local_endpoint().error(),
            platform::NetError::not_connected);
  EXPECT_TRUE(second.value()->bind(loopback()));
  EXPECT_EQ(second.value()->bind(loopback()).error(),
            platform::NetError::invalid_argument);
  EXPECT_NE(second.value()->local_endpoint().value().port, port);
  EXPECT_TRUE(first->close());
  auto third =
      network.sockets().create_udp_socket(platform::IpAddress::loopback_ipv4());
  EXPECT_TRUE(third.value()->bind(loopback(port)));
}

TEST_F(InMemoryUdp, FailsSendsAndReceivesAsInjected) {
  auto receiver = udp(network, loopback());
  auto sender = udp(network, loopback());
  const auto target = receiver->local_endpoint().value();
  network.fail_sends_with(platform::NetError::network_unreachable);
  const auto refused = sender->send_to(bytes("x"), target);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), platform::NetError::network_unreachable);
  network.clear_failures();
  EXPECT_TRUE(sender->send_to(bytes("x"), target));
  network.fail_receives_with(platform::NetError::connection_reset);
  std::array<std::byte, 4> buffer{};
  const auto failed = receiver->receive_from(buffer);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), platform::NetError::connection_reset);
  network.clear_failures();
  EXPECT_EQ(receiver->receive_from(buffer).value().size, 1U);
}

TEST_F(InMemoryUdp, ConnectedSocketsTalkToOnePeer) {
  auto console = udp(network, loopback());
  auto stranger = udp(network, loopback());
  const auto target = console->local_endpoint().value();
  auto knocker = network.sockets()
                     .create_udp_socket(platform::IpAddress::loopback_ipv4())
                     .value();
  EXPECT_EQ(knocker->send(bytes("x")).error(),
            platform::NetError::not_connected);
  EXPECT_TRUE(knocker->connect(target));
  EXPECT_EQ(knocker->send(bytes("ping")).value(), 4U);
  EXPECT_EQ(network.sent().back(), target);
  std::array<std::byte, 16> buffer{};
  const auto heard = console->receive_from(buffer);
  ASSERT_TRUE(heard);
  EXPECT_EQ(heard.value().size, 4U);
  EXPECT_EQ(heard.value().source, knocker->local_endpoint().value());
  EXPECT_TRUE(heard.value().source.address.is_loopback());
  EXPECT_NE(heard.value().source.port, 0U);
  EXPECT_TRUE(stranger->send_to(bytes("no"), heard.value().source));
  EXPECT_EQ(knocker->receive_from(buffer).error(),
            platform::NetError::would_block);
  EXPECT_TRUE(console->send_to(bytes("pong"), heard.value().source));
  EXPECT_EQ(knocker->receive_from(buffer).value().source, target);

  EXPECT_TRUE(console->close());
  EXPECT_TRUE(knocker->send(bytes("ping")));
  EXPECT_EQ(knocker->receive_from(buffer).error(),
            platform::NetError::connection_refused);
  EXPECT_EQ(knocker->receive_from(buffer).error(),
            platform::NetError::would_block);

  auto scoped = network.sockets()
                    .create_udp_socket(platform::IpAddress::loopback_ipv6())
                    .value();
  const auto link_local = platform::IpAddress::ipv6(
      {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
  EXPECT_EQ(scoped->connect({link_local.with_scope_id(77), 9}).error(),
            platform::NetError::address_not_available);
  EXPECT_TRUE(scoped->connect({link_local.with_scope_id(1), 9}));
  EXPECT_EQ(knocker->connect({platform::IpAddress::loopback_ipv6(), 9}).error(),
            platform::NetError::invalid_argument);
}

}  // namespace
}  // namespace network_test
