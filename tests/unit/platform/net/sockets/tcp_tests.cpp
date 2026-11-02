#include "tests/unit/platform/net/sockets/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>

namespace platform_net_test {
namespace {

using libchiaking::platform::IpAddress;
using libchiaking::platform::NetError;
using libchiaking::platform::TcpSocket;

class TcpSockets : public SocketRig {
 protected:
  struct Pair {
    std::unique_ptr<TcpSocket> client;
    std::unique_ptr<TcpSocket> server;
  };

  auto connect_pair(Pair& pair) -> ::testing::AssertionResult {
    auto listener = sockets().create_tcp_listener(IpAddress::loopback_ipv4());
    auto client = sockets().create_tcp_socket(IpAddress::loopback_ipv4());
    if (!listener || !client ||
        !listener.value()->bind({IpAddress::loopback_ipv4(), 0}) ||
        !listener.value()->listen(1))
      return ::testing::AssertionFailure() << "no listening socket";
    const auto endpoint = listener.value()->local_endpoint();
    if (!endpoint)
      return ::testing::AssertionFailure() << "the listener has no endpoint";
    const auto connected = client.value()->connect(endpoint.value());
    if (!connected && connected.error() != NetError::would_block)
      return ::testing::AssertionFailure() << "connect failed";
    for (int attempt = 0; attempt < 200; ++attempt) {
      auto accepted = listener.value()->accept();
      if (accepted) {
        pause(std::chrono::milliseconds{5});
        if (!client.value()->complete_connect())
          return ::testing::AssertionFailure() << "connect did not complete";
        pair.client = std::move(client.value());
        pair.server = std::move(accepted.value());
        return ::testing::AssertionSuccess();
      }
      if (accepted.error() != NetError::would_block)
        return ::testing::AssertionFailure() << "accept failed";
      pause(std::chrono::milliseconds{5});
    }
    return ::testing::AssertionFailure() << "nothing was accepted";
  }

  auto sends_until_peer_gone(TcpSocket& socket) -> ::testing::AssertionResult {
    const std::array<std::byte, 512> payload{};
    for (int attempt = 0; attempt < 200; ++attempt) {
      const auto sent = socket.send(payload);
      if (!sent) {
        if (sent.error() != NetError::would_block)
          return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure()
               << "the send would block instead of failing";
      }
      pause(std::chrono::milliseconds{5});
    }
    return ::testing::AssertionFailure() << "every send succeeded";
  }
};

TEST_F(TcpSockets, SendingToAClosedServerFailsWithoutKillingTheProcess) {
  Pair outbound;
  ASSERT_TRUE(connect_pair(outbound));
  ASSERT_TRUE(outbound.server->close());
  EXPECT_TRUE(sends_until_peer_gone(*outbound.client));
}

TEST_F(TcpSockets, SendingToAClosedClientFailsWithoutKillingTheProcess) {
  Pair inbound;
  ASSERT_TRUE(connect_pair(inbound));
  ASSERT_TRUE(inbound.client->close());
  EXPECT_TRUE(sends_until_peer_gone(*inbound.server));
}

}  // namespace
}  // namespace platform_net_test
