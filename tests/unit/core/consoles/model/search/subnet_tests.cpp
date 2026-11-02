#include "tests/unit/core/consoles/model/search/rig.hpp"

#include <gtest/gtest.h>

#include <string_view>
#include <type_traits>

namespace consoles_test {
namespace {

using libchiaking::consoles::Subnet;
using libchiaking::platform::IpAddress;

static_assert(!std::is_default_constructible_v<Subnet>);

TEST(Subnet, NormalisesATwentyFourBitRange) {
  const auto wide = PlanRig::subnet("192.0.2.99/24");
  EXPECT_EQ(wide.text(), "192.0.2.0/24");
  EXPECT_EQ(wide.network(), PlanRig::ip(192, 0, 2, 0));
  EXPECT_EQ(wide.prefix_length(), 24);
  EXPECT_EQ(wide.host_count(), 254U);
  EXPECT_EQ(wide.host(0), PlanRig::ip(192, 0, 2, 1));
  EXPECT_EQ(wide.host(253), PlanRig::ip(192, 0, 2, 254));
}

TEST(Subnet, ContainsOnlyItsOwnAddresses) {
  const auto wide = PlanRig::subnet("192.0.2.99/24");
  EXPECT_TRUE(wide.contains(PlanRig::ip(192, 0, 2, 255)));
  EXPECT_FALSE(wide.contains(PlanRig::ip(192, 0, 3, 1)));
  EXPECT_FALSE(wide.contains(IpAddress::loopback_ipv6()));
}

TEST(Subnet, EqualsTheSameNetworkAndPrefix) {
  const auto wide = PlanRig::subnet("192.0.2.99/24");
  EXPECT_TRUE(wide == PlanRig::subnet("192.0.2.1/24"));
  EXPECT_FALSE(wide == PlanRig::subnet("192.0.2.0/25"));
}

TEST(Subnet, ThirtyOneBitsHoldBothAddresses) {
  const auto pair = PlanRig::subnet("198.51.100.3/31");
  EXPECT_EQ(pair.text(), "198.51.100.2/31");
  EXPECT_EQ(pair.host_count(), 2U);
  EXPECT_EQ(pair.host(0), PlanRig::ip(198, 51, 100, 2));
  EXPECT_EQ(pair.host(1), PlanRig::ip(198, 51, 100, 3));
}

TEST(Subnet, ThirtyTwoBitsHoldOneAddress) {
  const auto single = PlanRig::subnet("203.0.113.4/32");
  EXPECT_EQ(single.host_count(), 1U);
  EXPECT_EQ(single.host(0), PlanRig::ip(203, 0, 113, 4));
}

TEST(Subnet, ThirtyBitsLeaveOutTheNetworkAndBroadcast) {
  const auto quad = PlanRig::subnet("203.0.113.9/30");
  EXPECT_EQ(quad.text(), "203.0.113.8/30");
  EXPECT_EQ(quad.host_count(), 2U);
  EXPECT_EQ(quad.host(0), PlanRig::ip(203, 0, 113, 9));
  EXPECT_EQ(quad.host(1), PlanRig::ip(203, 0, 113, 10));
}

TEST(Subnet, RefusesEveryOtherForm) {
  for (const std::string_view invalid :
       {"192.0.2.0/23", "192.0.2.0/33", "192.0.2.0/", "192.0.2.0",
        "192.0.2.0/24/24", "127.0.0.1/32", "0.1.2.3/32", "224.0.0.1/32",
        "192.0.2.01/32", "2001:db8::1/32", "", "/24", "192.0.2.0/-24",
        "192.0.2.0/+24", "192.0.2.0/2x", "192.0.2.0/ 24",
        "192.0.2.0/4294967320", "255.255.255.255/32", "192.0.2.0000/24"})
    EXPECT_FALSE(Subnet::parse(invalid)) << invalid;
}

TEST(Subnet, NamesItsHostsAsText) {
  const auto subnet = Subnet::parse("192.168.7.77/24");
  ASSERT_TRUE(subnet);
  EXPECT_EQ(subnet->text(), "192.168.7.0/24");
  EXPECT_EQ(subnet->prefix_length(), 24);
  EXPECT_EQ(subnet->host_count(), 254U);
  EXPECT_EQ(libchiaking::platform::to_string(subnet->host(0)), "192.168.7.1");
  EXPECT_EQ(libchiaking::platform::to_string(subnet->host(253)),
            "192.168.7.254");
  EXPECT_TRUE(subnet->contains(IpAddress::ipv4({192, 168, 7, 200})));
  EXPECT_FALSE(subnet->contains(IpAddress::ipv4({192, 168, 8, 1})));
}

TEST(Subnet, CountsTheHostsOfTheNarrowestRanges) {
  const auto single = Subnet::parse("10.0.0.5/32");
  ASSERT_TRUE(single);
  EXPECT_EQ(single->host_count(), 1U);
  EXPECT_EQ(libchiaking::platform::to_string(single->host(0)), "10.0.0.5");
  const auto pair = Subnet::parse("10.0.0.4/31");
  ASSERT_TRUE(pair);
  EXPECT_EQ(pair->host_count(), 2U);
  const auto quad = Subnet::parse("10.0.0.4/30");
  ASSERT_TRUE(quad);
  EXPECT_EQ(quad->host_count(), 2U);
}

TEST(Subnet, RefusesWhatIsNotASmallRoutableIpv4Range) {
  for (const std::string_view refused :
       {"", "192.168.7.0", "192.168.7.0/", "/24", "192.168.7.0/23",
        "192.168.7.0/33", "192.168.7.0/2x", "127.0.0.0/24", "0.1.2.0/24",
        "224.0.0.0/24", "fe80::/24", "host.example/24", "192.168.007.000/24x"})
    EXPECT_FALSE(Subnet::parse(refused)) << refused;
}

}  // namespace
}  // namespace consoles_test
