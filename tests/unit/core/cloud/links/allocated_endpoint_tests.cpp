#include "tests/unit/core/cloud/links/connect_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cloud_test {
namespace {

using ConnectReason = ConnectRig::Reason;
using Stage = ConnectRig::Stage;

class FakeDns final : public platform::DnsProvider {
 public:
  auto resolve(std::string_view host, std::string_view service)
      -> libchiaking::Expected<std::vector<platform::IpAddress>,
                               platform::NetError> override {
    ++calls;
    seen_host = host;
    seen_service = service;
    if (fail)
      return libchiaking::unexpected(platform::NetError::system_failure);
    return addresses;
  }

  std::vector<platform::IpAddress> addresses;
  bool fail{};
  int calls{};
  std::string seen_host;
  std::string seen_service;
};

class AllocatedEndpoint : public ::testing::Test {
 protected:
  auto resolve(std::string_view host, std::uint16_t port) {
    return play::AllocatedEndpoint::resolve(dns, host, port);
  }

  const platform::IpAddress ipv4 = platform::IpAddress::ipv4({203, 0, 113, 5});
  const platform::IpAddress ipv6 = platform::IpAddress::ipv6(
      {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
  FakeDns dns;
};

TEST_F(AllocatedEndpoint, ResolvesAnIpv4HostToItsIpv4Address) {
  dns.addresses = {ipv6, ipv4};
  const auto endpoint = resolve("203.0.113.5", 9295);
  ASSERT_TRUE(endpoint);
  EXPECT_EQ(endpoint.value().socket().address, ipv4);
  EXPECT_EQ(endpoint.value().socket().port, 9295);
  EXPECT_EQ(dns.calls, 1);
  EXPECT_EQ(dns.seen_host, "203.0.113.5");
  EXPECT_EQ(dns.seen_service, "9295");
}

TEST_F(AllocatedEndpoint, ResolvesAnIpv6HostToItsIpv6Address) {
  dns.addresses = {ipv4, ipv6};
  const auto endpoint = resolve("2001:db8::1", 2053);
  ASSERT_TRUE(endpoint);
  EXPECT_EQ(endpoint.value().socket().address, ipv6);
  EXPECT_EQ(endpoint.value().socket().port, 2053);
}

TEST_F(AllocatedEndpoint, HasNoAddressWithoutOneOfTheHostsFamily) {
  dns.addresses = {ipv6};
  EXPECT_TRUE(ConnectRig::fails_with(resolve("203.0.113.5", 9295), Stage::route,
                                     ConnectReason::no_address));
  dns.addresses.clear();
  EXPECT_TRUE(ConnectRig::fails_with(resolve("203.0.113.5", 9295), Stage::route,
                                     ConnectReason::no_address));
}

TEST_F(AllocatedEndpoint, ReportsADnsFailure) {
  dns.fail = true;
  EXPECT_TRUE(ConnectRig::fails_with(resolve("203.0.113.5", 9295), Stage::route,
                                     ConnectReason::dns_failure));
}

TEST_F(AllocatedEndpoint, RefusesAnEmptyHostWithoutResolving) {
  dns.fail = true;
  EXPECT_TRUE(ConnectRig::fails_with(resolve("", 9295), Stage::route,
                                     ConnectReason::invalid_host));
  EXPECT_EQ(dns.calls, 0);
}

TEST_F(AllocatedEndpoint, FallsBackToTheDefaultPort) {
  dns.addresses = {ipv4};
  const auto endpoint = resolve("203.0.113.5", 0);
  ASSERT_TRUE(endpoint);
  EXPECT_EQ(endpoint.value().socket().port, 9296);
  EXPECT_EQ(dns.seen_service, "9296");
}

TEST_F(AllocatedEndpoint, KeepsTheHighestPort) {
  dns.addresses = {ipv4};
  const auto endpoint = resolve("203.0.113.5", 65535);
  ASSERT_TRUE(endpoint);
  EXPECT_EQ(endpoint.value().socket().port, 65535);
  EXPECT_EQ(dns.seen_service, "65535");
}

}  // namespace
}  // namespace cloud_test
