#include <libchiaking/platform/net/types.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string_view>

namespace {

using libchiaking::platform::IpAddress;
using libchiaking::platform::parse_ip_address;
using libchiaking::platform::SocketEndpoint;
using libchiaking::platform::to_string;

class IpAddressText : public ::testing::Test {
 protected:
  static auto round_trips(std::string_view text, std::string_view expected)
      -> ::testing::AssertionResult {
    const auto parsed = parse_ip_address(text);
    if (!parsed)
      return ::testing::AssertionFailure()
             << '"' << text << "\" does not parse";
    const auto formatted = to_string(*parsed);
    if (formatted != expected)
      return ::testing::AssertionFailure()
             << '"' << text << "\" formats as \"" << formatted << "\", not \""
             << expected << '"';
    return ::testing::AssertionSuccess();
  }
};

TEST_F(IpAddressText, AcceptedTextFormatsCanonically) {
  EXPECT_TRUE(round_trips("192.168.1.20", "192.168.1.20"));
  EXPECT_TRUE(round_trips("0.0.0.0", "0.0.0.0"));
  EXPECT_TRUE(round_trips("255.255.255.255", "255.255.255.255"));
  EXPECT_TRUE(round_trips("::", "::"));
  EXPECT_TRUE(round_trips("::1", "::1"));
  EXPECT_TRUE(round_trips("fe80::", "fe80::"));
  EXPECT_TRUE(round_trips("FE80:0:0:0:0:0:0:1", "fe80::1"));
  EXPECT_TRUE(round_trips("1:0:0:2:0:0:0:3", "1:0:0:2::3"));
  EXPECT_TRUE(round_trips("1:0:2:0:3:0:4:0", "1:0:2:0:3:0:4:0"));
  EXPECT_TRUE(round_trips("2001:db8::ff00:42:8329", "2001:db8::ff00:42:8329"));
  EXPECT_TRUE(round_trips("1:2:3:4:5:6::8", "1:2:3:4:5:6:0:8"));
}

TEST_F(IpAddressText, MalformedTextIsRefused) {
  constexpr std::array<std::string_view, 21> refused{"",
                                                     "1.2.3",
                                                     "1.2.3.4.5",
                                                     "1.2.3.",
                                                     ".1.2.3",
                                                     "256.1.1.1",
                                                     "01.2.3.4",
                                                     "1.2.3.+4",
                                                     "1..2.3",
                                                     "gg::1",
                                                     "1:2:3:4:5:6:7",
                                                     "1:2:3:4:5:6:7:8:9",
                                                     "1::2::3",
                                                     ":::",
                                                     "1:",
                                                     ":1::",
                                                     "12345::1",
                                                     "1:2:3:4:5:6:7::8",
                                                     "1:2:3:4:5:6:7:8::",
                                                     "::1:2:3:4:5:6:7:8",
                                                     "::ffff:1.2.3.4"};
  for (const auto text : refused)
    EXPECT_FALSE(parse_ip_address(text).has_value()) << '"' << text << '"';
}

TEST(IpAddressBytes, FollowTheFamily) {
  const auto v4 = IpAddress::ipv4({10, 1, 2, 3});
  EXPECT_EQ(v4.bytes().size(), 4U);
  EXPECT_EQ(v4.bytes()[3], std::byte{3});
  const auto v6 = IpAddress::loopback_ipv6();
  EXPECT_EQ(v6.bytes().size(), 16U);
  EXPECT_EQ(IpAddress::from_bytes(v4.bytes()), v4);
  EXPECT_EQ(IpAddress::from_bytes(v6.bytes()), v6);
  constexpr std::array<std::byte, 5> odd{};
  EXPECT_FALSE(IpAddress::from_bytes(odd).has_value());
}

TEST(IpAddressOrder, GoesByFamilyThenBytes) {
  const auto low = IpAddress::ipv4({9, 255, 255, 255});
  const auto high = IpAddress::ipv4({10, 0, 0, 0});
  EXPECT_LT(low, high);
  EXPECT_LT(high, IpAddress::any_ipv6());
  const auto link = parse_ip_address("fe80::1");
  ASSERT_TRUE(link);
  EXPECT_LT(*link, link->with_scope_id(2));
  EXPECT_NE(*link, link->with_scope_id(2));
  EXPECT_EQ(IpAddress::ipv4({1, 2, 3, 4}).with_scope_id(5),
            IpAddress::ipv4({1, 2, 3, 4}));
  EXPECT_NE(IpAddress::loopback_ipv4(),
            IpAddress::ipv6(
                {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1}));
  const SocketEndpoint first{low, 9000};
  const SocketEndpoint second{low, 9001};
  EXPECT_LT(first, second);
  EXPECT_NE(first, second);
}

}  // namespace
