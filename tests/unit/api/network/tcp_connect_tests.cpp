#include "tests/unit/api/network/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace network_test {
namespace {

using InMemoryTcpConnect = InMemoryNetworkTest;

TEST_F(InMemoryTcpConnect, IsRefusedOnAPortNobodyListensOn) {
  auto stream = client(network);
  const auto started = stream->connect(loopback(9));
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), platform::NetError::would_block);
  EXPECT_EQ(wait_for(network, *stream, platform::PollEvent::writable) &
                platform::PollEvent::writable,
            platform::PollEvent::writable);
  const auto completed = stream->complete_connect();
  ASSERT_FALSE(completed);
  EXPECT_EQ(completed.error(), platform::NetError::connection_refused);
  EXPECT_EQ(stream->peer_endpoint().error(), platform::NetError::not_connected);
  std::array<std::byte, 4> buffer{};
  EXPECT_EQ(stream->receive(buffer).error(), platform::NetError::not_connected);
  EXPECT_EQ(stream->send(bytes("x")).error(),
            platform::NetError::not_connected);

  auto unbound = network.sockets().create_tcp_listener(
      platform::IpAddress::loopback_ipv4());
  ASSERT_TRUE(unbound);
  EXPECT_TRUE(unbound.value()->bind(loopback(4000)));
  auto idle = client(network);
  EXPECT_EQ(idle->connect(loopback(4000)).error(),
            platform::NetError::would_block);
  EXPECT_EQ(idle->complete_connect().error(),
            platform::NetError::connection_refused);

  auto foreign = client(network);
  EXPECT_EQ(foreign->connect({platform::IpAddress::ipv4({203, 0, 113, 1}), 80})
                .error(),
            platform::NetError::would_block);
  EXPECT_EQ(foreign->complete_connect().error(),
            platform::NetError::host_unreachable);
}

TEST_F(InMemoryTcpConnect, CanStallAndTimeOut) {
  auto server = listener(network);
  const auto target = server->local_endpoint().value();
  network.stall_connects();
  auto stalled = client(network);
  EXPECT_EQ(stalled->connect(target).error(), platform::NetError::would_block);
  EXPECT_EQ(wait_for(network, *stalled, platform::PollEvent::writable, 40ms),
            platform::PollEvent::none);
  EXPECT_EQ(stalled->complete_connect().error(),
            platform::NetError::would_block);
  EXPECT_EQ(stalled->connect(target).error(), platform::NetError::would_block);
  EXPECT_EQ(server->accept().error(), platform::NetError::would_block);

  network.fail_connects_with(platform::NetError::timed_out);
  EXPECT_EQ(wait_for(network, *stalled, platform::PollEvent::writable, 40ms),
            platform::PollEvent::none);
  network.stall_connects(false);
  EXPECT_EQ(wait_for(network, *stalled, platform::PollEvent::writable),
            platform::PollEvent::writable);
  const auto timed_out = stalled->complete_connect();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), platform::NetError::timed_out);
  EXPECT_TRUE(stalled->complete_connect());
  EXPECT_EQ(server->accept().error(), platform::NetError::would_block);

  network.clear_failures();
  network.stall_connects();
  auto resumed = client(network);
  EXPECT_EQ(resumed->connect(target).error(), platform::NetError::would_block);
  network.stall_connects(false);
  EXPECT_EQ(wait_for(network, *resumed, platform::PollEvent::writable),
            platform::PollEvent::writable);
  EXPECT_TRUE(resumed->complete_connect());
  EXPECT_TRUE(server->accept());
}

TEST_F(InMemoryTcpConnect, FailsAsInjectedUntilTheFailureIsCleared) {
  auto server = listener(network);
  const auto target = server->local_endpoint().value();
  network.fail_connects_with(platform::NetError::network_unreachable);
  auto blocked = client(network);
  EXPECT_EQ(blocked->connect(target).error(), platform::NetError::would_block);
  EXPECT_EQ(blocked->complete_connect().error(),
            platform::NetError::network_unreachable);
  EXPECT_EQ(server->accept().error(), platform::NetError::would_block);
  network.clear_failures();
  EXPECT_EQ(blocked->connect(target).error(), platform::NetError::would_block);
  EXPECT_TRUE(blocked->complete_connect());
  EXPECT_TRUE(server->accept());
  EXPECT_TRUE(blocked->connect(target));
}

TEST_F(InMemoryTcpConnect, ClosingTheListenerEndsWaitingConnections) {
  auto server = listener(network);
  const auto target = server->local_endpoint().value();
  auto waiting = client(network);
  EXPECT_EQ(waiting->connect(target).error(), platform::NetError::would_block);
  EXPECT_TRUE(waiting->complete_connect());
  EXPECT_TRUE(server->close());
  EXPECT_EQ(server->close().error(), platform::NetError::closed);
  EXPECT_EQ(server->accept().error(), platform::NetError::closed);
  std::array<std::byte, 4> buffer{};
  EXPECT_EQ(waiting->receive(buffer).error(), platform::NetError::closed);
  auto late = client(network);
  EXPECT_EQ(late->connect(target).error(), platform::NetError::would_block);
  EXPECT_EQ(late->complete_connect().error(),
            platform::NetError::connection_refused);
}

TEST_F(InMemoryTcpConnect, BindsOnceAndAcceptsWithinTheBacklog) {
  auto first = listener(network);
  const auto endpoint = first->local_endpoint().value();
  auto second = network.sockets().create_tcp_listener(
      platform::IpAddress::loopback_ipv4());
  EXPECT_EQ(second.value()->bind(endpoint).error(),
            platform::NetError::address_in_use);
  EXPECT_EQ(second.value()->accept().error(),
            platform::NetError::invalid_argument);
  EXPECT_EQ(second.value()->local_endpoint().error(),
            platform::NetError::not_connected);
  auto udp_socket = udp(network, endpoint);

  auto bound = client(network);
  EXPECT_TRUE(bound->bind(loopback(5000)));
  EXPECT_EQ(bound->bind(loopback(5001)).error(),
            platform::NetError::invalid_argument);
  EXPECT_EQ(bound->connect(endpoint).error(), platform::NetError::would_block);
  EXPECT_TRUE(bound->complete_connect());
  EXPECT_EQ(bound->local_endpoint().value(), loopback(5000));
  EXPECT_EQ(first->accept().value()->peer_endpoint().value(), loopback(5000));

  auto implicit = network.sockets().create_tcp_listener(
      platform::IpAddress::loopback_ipv4());
  EXPECT_TRUE(implicit.value()->listen(1));
  EXPECT_TRUE(
      implicit.value()->local_endpoint().value().address.is_unspecified());
  const auto port = implicit.value()->local_endpoint().value().port;

  auto one = client(network);
  auto two = client(network);
  EXPECT_EQ(one->connect(loopback(port)).error(),
            platform::NetError::would_block);
  EXPECT_EQ(two->connect(loopback(port)).error(),
            platform::NetError::would_block);
  EXPECT_EQ(wait_for(network, *one, platform::PollEvent::writable),
            platform::PollEvent::writable);
  EXPECT_EQ(wait_for(network, *two, platform::PollEvent::writable, 20ms),
            platform::PollEvent::none);
  EXPECT_TRUE(implicit.value()->accept());
  EXPECT_EQ(wait_for(network, *two, platform::PollEvent::writable),
            platform::PollEvent::writable);
  EXPECT_TRUE(two->complete_connect());
}

}  // namespace
}  // namespace network_test
