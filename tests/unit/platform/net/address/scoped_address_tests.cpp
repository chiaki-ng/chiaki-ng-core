#include <libchiaking/platform/net/types.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using libchiaking::platform::IpAddress;
using libchiaking::platform::parse_ip_address;
using libchiaking::platform::to_scoped_string;
using libchiaking::platform::to_string;

class ScopedAddress : public ::testing::Test {
 protected:
  static constexpr std::array<std::uint8_t, 16> link_local_one{
      0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
};

TEST_F(ScopedAddress, ANumericZoneBecomesTheScopeId) {
  const auto numeric = parse_ip_address("fe80::1%7");
  ASSERT_TRUE(numeric.has_value());
  EXPECT_EQ(numeric.value().ipv6_bytes(), link_local_one);
  EXPECT_EQ(numeric.value().scope_id(), 7U);
  EXPECT_EQ(to_string(numeric.value()), "fe80::1");
}

TEST_F(ScopedAddress, GroupsAfterAZeroRunKeepTheirOrder) {
  for (const char* text : {"2001:db8::1:2", "::ffff:c000:20a", "1:2:3::6:7:8",
                           "::3:4:5:6:7:8", "1::8", "2001:db8::", "::"}) {
    const auto parsed = parse_ip_address(text);
    ASSERT_TRUE(parsed.has_value()) << text;
    EXPECT_EQ(to_string(parsed.value()), text);
  }
}

TEST_F(ScopedAddress, AMappedPrefixIsRecognisedAfterAZeroRun) {
  const auto mapped = parse_ip_address("::ffff:c000:20a");
  ASSERT_TRUE(mapped.has_value());
  EXPECT_TRUE(mapped.value().is_ipv4_mapped());
}

TEST_F(ScopedAddress, NoZoneMeansScopeZeroAndTheScopeTakesPartInEquality) {
  const auto unscoped = parse_ip_address("fe80::1");
  ASSERT_TRUE(unscoped.has_value());
  EXPECT_EQ(unscoped.value().scope_id(), 0U);
  const auto numeric = parse_ip_address("fe80::1%7");
  ASSERT_TRUE(numeric.has_value());
  EXPECT_NE(numeric.value(), unscoped.value());
}

TEST_F(ScopedAddress, MalformedZonesAreRefused) {
  EXPECT_FALSE(parse_ip_address("fe80::1%").has_value());
  EXPECT_FALSE(parse_ip_address("10.0.0.1%1").has_value());
  EXPECT_FALSE(parse_ip_address("fe80::1%no-such-interface-xyz").has_value());
  EXPECT_FALSE(parse_ip_address("fe80::1%4294967296").has_value());
}

TEST_F(ScopedAddress, OnlyIpv6CarriesAScope) {
  EXPECT_EQ(IpAddress::loopback_ipv6().with_scope_id(3).scope_id(), 3U);
  EXPECT_EQ(IpAddress::loopback_ipv4().with_scope_id(3).scope_id(), 0U);
}

TEST_F(ScopedAddress, AnUnknownInterfaceIsWrittenAsTheNumericZone) {
  const auto unknown = parse_ip_address("fe80::1%4000000000");
  ASSERT_TRUE(unknown.has_value());
  EXPECT_EQ(to_scoped_string(unknown.value()), "fe80::1%4000000000");
}

}  // namespace
