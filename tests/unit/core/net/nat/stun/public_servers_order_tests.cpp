#include "src/core/net/nat/stun/public_servers.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nat_test {
namespace {

namespace stun = libchiaking::net::nat::stun;
namespace platform = libchiaking::platform;
using stun::PublicServers;

TEST(StunHostOrder, PutsPreferredHostsBeforeTheFixedOnes) {
  const std::vector<stun::StunHostListEntry> preferred{{"a.example", 1111},
                                                       {"b.example", 2222}};
  const auto ordered = PublicServers::build_host_order(preferred);
  ASSERT_EQ(ordered.size(), 13U);
  EXPECT_EQ(ordered[0].host, "a.example");
  EXPECT_EQ(ordered[0].port, 1111);
  EXPECT_EQ(ordered[1].host, "b.example");
  EXPECT_EQ(ordered[1].port, 2222);
  for (std::size_t index = 0; index < PublicServers::fixed_hosts.size();
       ++index) {
    EXPECT_EQ(ordered[index + 2U].host, PublicServers::fixed_hosts[index].host);
    EXPECT_EQ(ordered[index + 2U].port, PublicServers::fixed_hosts[index].port);
  }
  EXPECT_EQ(ordered[2].host, "stun.moonlight-stream.org");
  EXPECT_EQ(ordered[2].port, 3478);
  EXPECT_EQ(ordered.back().host, "stun4.l.google.com");
  EXPECT_EQ(ordered.back().port, 19305);
}

TEST(StunHostOrder, FallsBackToTheFixedHosts) {
  const auto fallback = PublicServers::build_host_order({});
  ASSERT_EQ(fallback.size(), 11U);
  EXPECT_EQ(fallback[0].host, "stun.moonlight-stream.org");
}

TEST(StunHostOrder, DropsInvalidHostsAndKeepsTenPreferred) {
  std::vector<stun::StunHostListEntry> many{{"", 1234},
                                            {"bad", 0},
                                            {"sp ace", 3478},
                                            {std::string(254, 'x'), 3478},
                                            {"bad:colon", 3478}};
  for (int index = 0; index < 14; ++index)
    many.push_back({"h" + std::to_string(index), 1000});
  const auto limited = PublicServers::build_host_order(many);
  ASSERT_EQ(limited.size(), 21U);
  for (int index = 0; index < 10; ++index)
    EXPECT_EQ(limited[static_cast<std::size_t>(index)].host,
              "h" + std::to_string(index));
  EXPECT_EQ(limited[10].host, "stun.moonlight-stream.org");
}

class ServerDns final : public platform::DnsProvider {
 public:
  auto resolve(std::string_view host, std::string_view service)
      -> libchiaking::Expected<std::vector<platform::IpAddress>,
                               platform::NetError> override {
    calls.emplace_back(host, service);
    if (host == "dead.example")
      return libchiaking::unexpected(platform::NetError::host_unreachable);
    if (host == "empty.example") return std::vector<platform::IpAddress>{};
    if (host == "mixed.example")
      return std::vector{platform::IpAddress::any_ipv4(),
                         platform::IpAddress::ipv6({0x20, 1, 0, 0, 0, 0, 0, 0,
                                                    0, 0, 0, 0, 0, 0, 0, 1}),
                         platform::IpAddress::ipv4({10, 0, 0, 7}),
                         platform::IpAddress::ipv4({10, 0, 0, 8})};
    return libchiaking::unexpected(platform::NetError::host_unreachable);
  }

  std::vector<std::pair<std::string, std::string>> calls;
};

class StunServerEndpoints : public ::testing::Test {
 protected:
  ServerDns dns;
  const std::vector<stun::StunServerHost> servers{{"dead.example", 1000},
                                                  {"mixed.example", 1001},
                                                  {"", 1002},
                                                  {"empty.example", 1003},
                                                  {"mixed.example", 0}};
};

TEST_F(StunServerEndpoints, ResolveTheFirstIpv4AddressOfEachUsableServer) {
  const auto v4 = PublicServers::resolve_server_endpoints(
      servers, platform::IpFamily::ipv4, dns);
  ASSERT_TRUE(v4);
  ASSERT_EQ(v4.value().size(), 1U);
  EXPECT_EQ(v4.value()[0].server_index, 1U);
  EXPECT_EQ(v4.value()[0].endpoint,
            (platform::SocketEndpoint{platform::IpAddress::ipv4({10, 0, 0, 7}),
                                      1001}));
  ASSERT_EQ(dns.calls.size(), 3U);
  EXPECT_EQ(dns.calls[0].second, "1000");
  EXPECT_EQ(dns.calls[1].second, "1001");
  EXPECT_EQ(dns.calls[2].second, "1003");
}

TEST_F(StunServerEndpoints, ResolveTheIpv6AddressWhenAskedForIpv6) {
  const auto v6 = PublicServers::resolve_server_endpoints(
      servers, platform::IpFamily::ipv6, dns);
  ASSERT_TRUE(v6);
  ASSERT_EQ(v6.value().size(), 1U);
  EXPECT_TRUE(v6.value()[0].endpoint.address.is_ipv6());
  EXPECT_EQ(v6.value()[0].endpoint.port, 1001U);
}

TEST_F(StunServerEndpoints, ReportNoServersWhenNoneResolves) {
  const auto empty = PublicServers::resolve_server_endpoints(
      {}, platform::IpFamily::ipv4, dns);
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), stun::StunServerEndpointsError::no_servers);
  std::vector<stun::StunServerHost> many(stun::max_stun_servers + 4U,
                                         {"dead.example", 3478});
  dns.calls.clear();
  const auto none = PublicServers::resolve_server_endpoints(
      many, platform::IpFamily::ipv4, dns);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error(), stun::StunServerEndpointsError::no_servers);
  EXPECT_EQ(dns.calls.size(), stun::max_stun_servers);
}

TEST_F(StunServerEndpoints, StopAtTheMaximumInOrder) {
  std::vector<stun::StunServerHost> bounded(stun::max_stun_servers,
                                            {"mixed.example", 3478});
  bounded.push_back({"dead.example", 3478});
  const auto full = PublicServers::resolve_server_endpoints(
      bounded, platform::IpFamily::ipv4, dns);
  ASSERT_TRUE(full);
  ASSERT_EQ(full.value().size(), stun::max_stun_servers);
  ASSERT_EQ(dns.calls.size(), stun::max_stun_servers);
  for (std::size_t index = 0; index < stun::max_stun_servers; ++index)
    EXPECT_EQ(full.value()[index].server_index, index);
  EXPECT_EQ(dns.calls.back().first, "mixed.example");
  EXPECT_EQ(full.value()[0].endpoint,
            (platform::SocketEndpoint{platform::IpAddress::ipv4({10, 0, 0, 7}),
                                      3478}));
}

}  // namespace
}  // namespace nat_test
