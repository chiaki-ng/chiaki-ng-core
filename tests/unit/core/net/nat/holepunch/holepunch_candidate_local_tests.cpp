#include "libchiaking/platform/factory.hpp"
#include "src/core/net/nat/holepunch/holepunch_candidate.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nat_test {
namespace {

namespace platform = libchiaking::platform;
using namespace libchiaking::net::nat::holepunch;

class ScriptedDns final : public platform::DnsProvider {
 public:
  auto resolve(std::string_view host, std::string_view service)
      -> libchiaking::Expected<std::vector<platform::IpAddress>,
                               platform::NetError> override {
    calls.emplace_back(host, service);
    if (host == "unspec.example")
      return std::vector{platform::IpAddress::any_ipv4(),
                         platform::IpAddress::any_ipv6(),
                         platform::IpAddress::ipv4({192, 168, 0, 1})};
    if (host == "only-unspec.example")
      return std::vector{platform::IpAddress::any_ipv4()};
    if (host == "dead.example")
      return libchiaking::unexpected(platform::NetError::host_unreachable);
    if (host == "empty.example") return std::vector<platform::IpAddress>{};
    if (host == "v4.example")
      return std::vector{platform::IpAddress::ipv4({10, 0, 0, 1}),
                         platform::IpAddress::ipv4({10, 0, 0, 2})};
    if (host == "v6.example")
      return std::vector{platform::IpAddress::ipv6(
          {0x20, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1})};
    return libchiaking::unexpected(platform::NetError::host_unreachable);
  }

  std::vector<std::pair<std::string, std::string>> calls;
};

class HolepunchCandidateEndpoints : public ::testing::Test {
 protected:
  static auto candidate(std::string addr, std::uint16_t port) -> Candidate {
    Candidate out{};
    out.addr = std::move(addr);
    out.port = port;
    return out;
  }

  ScriptedDns dns;
};

TEST_F(HolepunchCandidateEndpoints, ResolveTheFirstUsableAddressOfEachHost) {
  const std::vector<Candidate> candidates{
      candidate("dead.example", 1000),   candidate("v4.example", 1001),
      candidate("empty.example", 1002),  candidate("v4.example", 0),
      candidate("unspec.example", 1003), candidate("v6.example", 1004),
  };
  auto resolved = CandidateGathering::resolve_endpoints(candidates, dns);
  ASSERT_TRUE(resolved.has_value());
  ASSERT_EQ(resolved.value().size(), 3U);
  EXPECT_EQ(resolved.value()[0].candidate_index, 1U);
  EXPECT_EQ(resolved.value()[0].endpoint,
            (platform::SocketEndpoint{platform::IpAddress::ipv4({10, 0, 0, 1}),
                                      1001}));
  EXPECT_EQ(resolved.value()[1].candidate_index, 4U);
  EXPECT_EQ(resolved.value()[1].endpoint,
            (platform::SocketEndpoint{
                platform::IpAddress::ipv4({192, 168, 0, 1}), 1003}));
  EXPECT_EQ(resolved.value()[2].candidate_index, 5U);
  EXPECT_TRUE(resolved.value()[2].endpoint.address.is_ipv6());
  EXPECT_EQ(resolved.value()[2].endpoint.port, 1004U);
  ASSERT_EQ(dns.calls.size(), 5U);
  EXPECT_EQ(dns.calls[0].second, "1000");
  EXPECT_EQ(dns.calls[3].first, "unspec.example");
}

TEST_F(HolepunchCandidateEndpoints, FailWhenNoCandidateResolves) {
  EXPECT_FALSE(CandidateGathering::resolve_endpoints({}, dns).has_value());
  EXPECT_FALSE(
      CandidateGathering::resolve_endpoints(
          {candidate("dead.example", 1), candidate("empty.example", 2)}, dns)
          .has_value());
  EXPECT_FALSE(
      CandidateGathering::resolve_endpoints(
          {candidate("v4.example", 0), candidate("only-unspec.example", 1)},
          dns)
          .has_value());
}

class FakeInterfaces final : public platform::InterfacesProvider {
 public:
  auto enumerate()
      -> libchiaking::Expected<std::vector<platform::InterfaceInfo>,
                               platform::NetError> override {
    if (fail)
      return libchiaking::unexpected(platform::NetError::system_failure);
    return entries;
  }

  std::vector<platform::InterfaceInfo> entries;
  bool fail{};
};

class HolepunchLocalCandidate : public ::testing::Test {
 protected:
  using Medium = platform::InterfaceMedium;
  using Error = LocalCandidateError;

  static auto info(platform::IpAddress address, bool operational,
                   bool loopback = false, bool lan_adapter = true)
      -> platform::InterfaceInfo {
    platform::InterfaceInfo result{};
    result.address = address;
    result.operational = operational;
    result.loopback = loopback;
    result.lan_adapter = lan_adapter;
    return result;
  }

  static auto adapter(std::uint32_t index, Medium medium,
                      platform::IpAddress address) -> platform::InterfaceInfo {
    auto result = info(address, true);
    result.index = index;
    result.medium = medium;
    return result;
  }

  static auto select(std::vector<platform::InterfaceInfo> entries)
      -> std::string {
    FakeInterfaces chosen;
    chosen.entries = std::move(entries);
    const auto selected = CandidateGathering::select_local(chosen, 9);
    EXPECT_TRUE(selected.has_value());
    if (!selected) return {};
    return selected.value().addr;
  }

  void SetUp() override {
    provider.entries = {
        info(platform::IpAddress::ipv4({192, 168, 1, 10}), false),
        info(platform::IpAddress::loopback_ipv4(), true, true),
        info(platform::IpAddress::ipv6(
                 {0x20, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}),
             true),
        info(platform::IpAddress::any_ipv4(), true),
        info(platform::IpAddress::ipv4({172, 16, 0, 9}), true, false, false),
        info(platform::IpAddress::ipv4({10, 0, 0, 5}), true),
        info(platform::IpAddress::ipv4({10, 0, 0, 6}), true),
    };
  }

  FakeInterfaces provider;
  const platform::IpAddress eth1 = platform::IpAddress::ipv4({10, 0, 0, 1});
  const platform::IpAddress eth1b = platform::IpAddress::ipv4({10, 0, 0, 2});
  const platform::IpAddress eth2 = platform::IpAddress::ipv4({10, 0, 1, 1});
  const platform::IpAddress eth2b = platform::IpAddress::ipv4({10, 0, 1, 2});
  const platform::IpAddress wifi = platform::IpAddress::ipv4({192, 168, 1, 5});
  const platform::IpAddress wifi2 = platform::IpAddress::ipv4({192, 168, 2, 5});
};

TEST_F(HolepunchLocalCandidate, PrefersTheLastEthernetAdaptersFirstAddress) {
  EXPECT_EQ(select({adapter(1, Medium::ethernet, eth1),
                    adapter(1, Medium::ethernet, eth1b),
                    adapter(2, Medium::ethernet, eth2),
                    adapter(2, Medium::ethernet, eth2b)}),
            "10.0.1.1");
}

TEST_F(HolepunchLocalCandidate, PrefersTheFirstWirelessAdapter) {
  EXPECT_EQ(select({adapter(1, Medium::ethernet, eth1),
                    adapter(3, Medium::wireless, wifi),
                    adapter(2, Medium::ethernet, eth2)}),
            "192.168.1.5");
  EXPECT_EQ(select({adapter(3, Medium::wireless, wifi),
                    adapter(4, Medium::wireless, wifi2),
                    adapter(1, Medium::ethernet, eth1)}),
            "192.168.1.5");
}

TEST_F(HolepunchLocalCandidate, SkipsAdaptersWithoutAnAddress) {
  EXPECT_EQ(
      select({adapter(1, Medium::ethernet, eth1),
              adapter(3, Medium::wireless, platform::IpAddress::any_ipv4()),
              adapter(2, Medium::ethernet, eth2)}),
      "10.0.1.1");
  EXPECT_EQ(
      select({adapter(1, Medium::ethernet, eth1),
              adapter(2, Medium::ethernet, platform::IpAddress::any_ipv4())}),
      "10.0.0.1");
}

TEST_F(HolepunchLocalCandidate, PicksTheFirstOperationalLanIpv4Address) {
  const auto selected = CandidateGathering::select_local(provider, 12345);
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected.value().type, CandidateType::local);
  EXPECT_EQ(selected.value().addr, "10.0.0.5");
  EXPECT_EQ(selected.value().mapped_addr, "0.0.0.0");
  EXPECT_EQ(selected.value().port, 12345);
  EXPECT_EQ(selected.value().mapped_port, 0);
}

TEST_F(HolepunchLocalCandidate, RefusesAZeroPort) {
  const auto zero = CandidateGathering::select_local(provider, 0);
  ASSERT_FALSE(zero);
  EXPECT_EQ(zero.error(), Error::zero_port);
}

TEST_F(HolepunchLocalCandidate, ReportsAProviderFailure) {
  provider.fail = true;
  const auto failed = CandidateGathering::select_local(provider, 7);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), Error::provider_failure);
}

TEST_F(HolepunchLocalCandidate, ReportsNoUsableInterface) {
  provider.entries.clear();
  const auto empty = CandidateGathering::select_local(provider, 7);
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), Error::no_usable_interface);
}

}  // namespace
}  // namespace nat_test
