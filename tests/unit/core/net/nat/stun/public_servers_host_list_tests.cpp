#include "src/core/net/nat/stun/public_servers.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace nat_test {
namespace {

namespace stun = libchiaking::net::nat::stun;
using stun::PublicServers;
using stun::StunHostListError;

class StunHostList : public ::testing::Test {
 protected:
  static auto fourteen_hosts() -> std::string {
    std::string many;
    for (int index = 0; index < 14; ++index)
      many += "h" + std::to_string(index) + ":1000\n";
    return many;
  }
};

TEST_F(StunHostList, ReadsHostAndPortLinesEndedEitherWay) {
  const auto parsed =
      PublicServers::parse_host_list("a.example:3478\r\nb.example:443\n");
  ASSERT_TRUE(parsed);
  ASSERT_EQ(parsed.value().size(), 2U);
  EXPECT_EQ(parsed.value()[0].host, "a.example");
  EXPECT_EQ(parsed.value()[0].port, 3478);
  EXPECT_EQ(parsed.value()[1].host, "b.example");
  EXPECT_EQ(parsed.value()[1].port, 443);
}

TEST_F(StunHostList, KeepsTheFirstTenHosts) {
  const auto limited = PublicServers::parse_host_list(fourteen_hosts());
  ASSERT_TRUE(limited);
  ASSERT_EQ(limited.value().size(), 10U);
  EXPECT_EQ(limited.value()[9].host, "h9");
}

TEST_F(StunHostList, RefusesALongLineEvenAfterTheLimit) {
  const auto long_after_limit = PublicServers::parse_host_list(
      fourteen_hosts() + std::string(260, 'a') + ":1\n");
  ASSERT_FALSE(long_after_limit);
  EXPECT_EQ(long_after_limit.error(), StunHostListError::line_too_long);
}

TEST_F(StunHostList, SkipsMalformedLines) {
  std::string malformed =
      "\nno-colon\nok.example:3478\n:3478\nhost:\nhost:x\n"
      "host:0\nhost:65536\nhost:123456\nsp ace:1\n[::1]:3478\n";
  malformed += std::string{"nul\0x:1\n", 8};
  const auto valid_only = PublicServers::parse_host_list(malformed);
  ASSERT_TRUE(valid_only);
  ASSERT_EQ(valid_only.value().size(), 1U);
  EXPECT_EQ(valid_only.value()[0].host, "ok.example");
}

TEST_F(StunHostList, AcceptsHostsUpToTwoHundredFiftyThreeCharacters) {
  const auto max_host = PublicServers::parse_host_list(
      std::string(253, 'a') + ":65535\n" + std::string(254, 'b') + ":1\n");
  ASSERT_TRUE(max_host);
  ASSERT_EQ(max_host.value().size(), 1U);
  EXPECT_EQ(max_host.value()[0].host.size(), 253U);
}

TEST_F(StunHostList, RefusesOversizedInputAndLongLines) {
  const auto oversized = PublicServers::parse_host_list(
      std::string(PublicServers::max_host_list_bytes + 1U, 'a'));
  ASSERT_FALSE(oversized);
  EXPECT_EQ(oversized.error(), StunHostListError::input_too_large);
  const auto long_line =
      PublicServers::parse_host_list(std::string(260, 'a') + ":1\n");
  ASSERT_FALSE(long_line);
  EXPECT_EQ(long_line.error(), StunHostListError::line_too_long);
}

TEST_F(StunHostList, ReadsEmptyInputAsNoHosts) {
  const auto empty = PublicServers::parse_host_list(std::string_view{});
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty.value().empty());
}

TEST_F(StunHostList, IsCompleteWhenEveryLineHasAPort) {
  EXPECT_TRUE(PublicServers::reference_host_list_complete(""));
  EXPECT_TRUE(
      PublicServers::reference_host_list_complete("\n\na.example:3478\n"));
  EXPECT_TRUE(PublicServers::reference_host_list_complete("host::3478\n"));
  EXPECT_FALSE(PublicServers::reference_host_list_complete(
      "a.example:3478\nno-colon\n"));
  EXPECT_FALSE(PublicServers::reference_host_list_complete("host:\n"));
  EXPECT_FALSE(PublicServers::reference_host_list_complete(":::\n"));
}

TEST_F(StunHostList, IsCompleteWhenTheBadLineFollowsTheTenthHost) {
  EXPECT_TRUE(PublicServers::reference_host_list_complete(fourteen_hosts() +
                                                          "no-colon\n"));
}

TEST_F(StunHostList, ReadsBracketedIpv6Lines) {
  const auto ipv6 = PublicServers::parse_ipv6_host_list(
      "[2001:db8::1]:3478\r\n\nbad\n[]:1\n[::1]:x\n[::2]:0\n[fe80::1]:19302\n");
  ASSERT_TRUE(ipv6);
  ASSERT_EQ(ipv6.value().size(), 2U);
  EXPECT_EQ(ipv6.value()[0].host, "2001:db8::1");
  EXPECT_EQ(ipv6.value()[0].port, 3478);
  EXPECT_EQ(ipv6.value()[1].host, "fe80::1");
  EXPECT_EQ(ipv6.value()[1].port, 19302);
}

TEST_F(StunHostList, KeepsTheFirstTenIpv6Hosts) {
  std::string many_ipv6;
  for (int index = 0; index < 12; ++index)
    many_ipv6 += "[::" + std::to_string(index + 1) + "]:1000\n";
  const auto limited_ipv6 = PublicServers::parse_ipv6_host_list(many_ipv6);
  ASSERT_TRUE(limited_ipv6);
  EXPECT_EQ(limited_ipv6.value().size(), 10U);
}

TEST_F(StunHostList, RefusesALongIpv6LineAndReadsEmptyIpv6Input) {
  const auto long_ipv6 =
      PublicServers::parse_ipv6_host_list("[" + std::string(45, 'a') + "]:1\n");
  ASSERT_FALSE(long_ipv6);
  EXPECT_EQ(long_ipv6.error(), StunHostListError::line_too_long);
  const auto empty_ipv6 =
      PublicServers::parse_ipv6_host_list(std::string_view{});
  ASSERT_TRUE(empty_ipv6);
  EXPECT_TRUE(empty_ipv6.value().empty());
}

}  // namespace
}  // namespace nat_test
