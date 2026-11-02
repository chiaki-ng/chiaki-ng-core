#include "tests/support/enum_printing.hpp"
#include "tests/unit/platform/net/sockets/rig.hpp"

#include <gtest/gtest.h>

#include <utility>

namespace platform_net_test {
namespace {

using libchiaking::platform::IpAddress;
using libchiaking::platform::NetError;

using UdpSocketOptions = SocketRig;

TEST_F(UdpSocketOptions, TtlAcceptsValidHopsAndRefusesZeroAndAClosedSocket) {
  auto created = sockets().create_udp_socket(IpAddress::any_ipv4());
  ASSERT_TRUE(created);
  auto socket = std::move(created.value());
  ASSERT_TRUE(socket->bind({IpAddress::any_ipv4(), 0}));
  const auto ttl2 = socket->set_ipv4_ttl(2);
  if (!ttl2) {
    EXPECT_EQ(ttl2.error(), NetError::not_supported);
    return;
  }
  EXPECT_TRUE(socket->set_ipv4_ttl(64));
  const auto zero = socket->set_ipv4_ttl(0);
  ASSERT_FALSE(zero);
  EXPECT_EQ(zero.error(), NetError::invalid_argument);
  ASSERT_TRUE(socket->close());
  const auto closed = socket->set_ipv4_ttl(2);
  ASSERT_FALSE(closed);
  EXPECT_EQ(closed.error(), NetError::closed);
}

TEST_F(UdpSocketOptions, SharedPortBindingLetsTwoSocketsTakeOnePort) {
  auto first_created = sockets().create_udp_socket(IpAddress::any_ipv4());
  ASSERT_TRUE(first_created);
  auto first = std::move(first_created.value());
  ASSERT_TRUE(first->set_shared_port_binding(true));
  ASSERT_TRUE(first->bind({IpAddress::any_ipv4(), 0}));
  const auto local = first->local_endpoint();
  ASSERT_TRUE(local);
  ASSERT_NE(local.value().port, 0);

  auto second_created = sockets().create_udp_socket(IpAddress::any_ipv4());
  ASSERT_TRUE(second_created);
  auto second = std::move(second_created.value());
  ASSERT_TRUE(second->set_shared_port_binding(true));
  ASSERT_TRUE(second->bind({IpAddress::any_ipv4(), local.value().port}));
  const auto second_local = second->local_endpoint();
  ASSERT_TRUE(second_local);
  EXPECT_EQ(second_local.value().port, local.value().port);
  EXPECT_TRUE(first->close());
  EXPECT_TRUE(second->close());
  const auto closed = first->set_shared_port_binding(false);
  ASSERT_FALSE(closed);
  EXPECT_EQ(closed.error(), NetError::closed);
}

}  // namespace
}  // namespace platform_net_test
