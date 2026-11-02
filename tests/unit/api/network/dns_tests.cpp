#include "tests/unit/api/network/rig.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace network_test {
namespace {

using InMemoryDns = InMemoryNetworkTest;

TEST_F(InMemoryDns, ResolvesLiteralsAndTheHostMap) {
  const auto literal = network.dns().resolve("192.0.2.77", "");
  ASSERT_TRUE(literal);
  EXPECT_EQ(literal.value(),
            std::vector{platform::IpAddress::ipv4({192, 0, 2, 77})});
  const auto local = network.dns().resolve("localhost", "");
  ASSERT_TRUE(local);
  EXPECT_EQ(local.value(), std::vector{platform::IpAddress::loopback_ipv4()});
  const auto unknown = network.dns().resolve("console.example", "");
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error(), platform::NetError::host_unreachable);
  network.add_host("console.example",
                   {platform::IpAddress::ipv4({192, 0, 2, 20}),
                    platform::IpAddress::ipv4({192, 0, 2, 21})});
  const auto mapped = network.dns().resolve("console.example", "");
  ASSERT_TRUE(mapped);
  ASSERT_EQ(mapped.value().size(), 2U);
  EXPECT_EQ(mapped.value().front(), platform::IpAddress::ipv4({192, 0, 2, 20}));
}

}  // namespace
}  // namespace network_test
