#include "tests/support/enum_printing.hpp"
#include "tests/unit/platform/net/sockets/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>

namespace platform_net_test {
namespace {

using libchiaking::platform::IpAddress;
using libchiaking::platform::NetError;
using libchiaking::platform::UdpReceiveResult;
using libchiaking::platform::UdpSocket;

class UdpReceive : public SocketRig {
 protected:
  using Received = libchiaking::Expected<UdpReceiveResult, NetError>;

  auto receive_after_delay(UdpSocket& socket, std::span<std::byte> buffer)
      -> Received {
    Received received = libchiaking::unexpected(NetError::would_block);
    for (int attempt = 0; attempt < 200; ++attempt) {
      received = socket.receive_from(buffer);
      if (received || received.error() != NetError::would_block)
        return received;
      pause(std::chrono::milliseconds{5});
    }
    return received;
  }
};

TEST_F(UdpReceive, AnOversizedDatagramIsTruncatedAndTheNextOneArrivesWhole) {
  auto receiver = sockets().create_udp_socket(IpAddress::loopback_ipv4());
  auto sender = sockets().create_udp_socket(IpAddress::loopback_ipv4());
  ASSERT_TRUE(receiver);
  ASSERT_TRUE(sender);
  ASSERT_TRUE(receiver.value()->bind({IpAddress::loopback_ipv4(), 0}));
  ASSERT_TRUE(sender.value()->bind({IpAddress::loopback_ipv4(), 0}));
  const auto endpoint = receiver.value()->local_endpoint();
  ASSERT_TRUE(endpoint);
  std::array<std::byte, 64> payload{};
  for (std::size_t index = 0; index < payload.size(); ++index)
    payload[index] = static_cast<std::byte>(index);
  const std::array<std::byte, 3> follow_up{std::byte{0xa1}, std::byte{0xa2},
                                           std::byte{0xa3}};
  ASSERT_TRUE(sender.value()->send_to(payload, endpoint.value()));
  ASSERT_TRUE(sender.value()->send_to(follow_up, endpoint.value()));
  std::array<std::byte, 16> small{};
  const auto truncated = receive_after_delay(*receiver.value(), small);
  ASSERT_TRUE(truncated);
  EXPECT_EQ(truncated.value().size, small.size());
  EXPECT_EQ(small[0], std::byte{0});
  EXPECT_EQ(small[15], std::byte{15});
  std::array<std::byte, 64> large{};
  const auto whole = receive_after_delay(*receiver.value(), large);
  ASSERT_TRUE(whole);
  EXPECT_EQ(whole.value().size, follow_up.size());
  EXPECT_EQ(large[0], std::byte{0xa1});
}

TEST_F(UdpReceive, AnUnreachablePeerLeavesTheSenderQuiet) {
  auto closed = sockets().create_udp_socket(IpAddress::loopback_ipv4());
  auto sender = sockets().create_udp_socket(IpAddress::loopback_ipv4());
  ASSERT_TRUE(closed);
  ASSERT_TRUE(sender);
  ASSERT_TRUE(closed.value()->bind({IpAddress::loopback_ipv4(), 0}));
  ASSERT_TRUE(sender.value()->bind({IpAddress::loopback_ipv4(), 0}));
  const auto dead = closed.value()->local_endpoint();
  ASSERT_TRUE(dead);
  ASSERT_TRUE(closed.value()->close());
  const std::array<std::byte, 4> payload{};
  for (int attempt = 0; attempt < 4; ++attempt) {
    const auto sent = sender.value()->send_to(payload, dead.value());
    if (!sent) {
      EXPECT_EQ(sent.error(), NetError::would_block);
    }
    pause(std::chrono::milliseconds{10});
    std::array<std::byte, 16> buffer{};
    const auto received = sender.value()->receive_from(buffer);
    ASSERT_FALSE(received) << "attempt " << attempt;
    EXPECT_EQ(received.error(), NetError::would_block);
  }
}

}  // namespace
}  // namespace platform_net_test
