#include "tests/unit/core/cloud/links/ping_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloud_test {
namespace {

namespace gk = libchiaking::psn::gk;
namespace platform = libchiaking::platform;
using libchiaking::play::PingTarget;
using Error = PingTarget::Error;

class PingDns final : public platform::DnsProvider {
 public:
  auto resolve(std::string_view host, std::string_view service)
      -> libchiaking::Expected<std::vector<platform::IpAddress>,
                               platform::NetError> override {
    ++calls;
    last_host = host;
    last_service = service;
    if (fail)
      return libchiaking::unexpected(platform::NetError::host_unreachable);
    return answers;
  }

  int calls{};
  bool fail{};
  std::string last_host;
  std::string last_service;
  std::vector<platform::IpAddress> answers;
};

class PingTargetResolve : public ::testing::Test {
 protected:
  static auto datacenter(std::string host, int port) -> gk::Datacenter {
    return {"dc", std::move(host), port, 20'000};
  }

  auto resolve(std::string host, int port) {
    return PingTarget::resolve(dns, datacenter(std::move(host), port));
  }

  PingDns dns;
};

TEST_F(PingTargetResolve, RefusesAnEmptyHostOrOversizedPortWithoutResolving) {
  const auto empty_host = resolve("", 2053);
  ASSERT_FALSE(empty_host);
  EXPECT_EQ(empty_host.error(), Error::invalid_host);
  const auto too_large = resolve("dc.example", 65536);
  ASSERT_FALSE(too_large);
  EXPECT_EQ(too_large.error(), Error::invalid_port);
  EXPECT_EQ(dns.calls, 0);
}

TEST_F(PingTargetResolve, FallsBackToTheSenkushaPort) {
  dns.answers = {platform::IpAddress::ipv4({10, 4, 5, 6})};
  for (const int port : {0, -1}) {
    SCOPED_TRACE(port);
    const auto fallback = resolve("dc.example", port);
    ASSERT_TRUE(fallback);
    EXPECT_EQ(fallback.value().endpoint().port, 9297);
    EXPECT_EQ(fallback.value().endpoint().address,
              platform::IpAddress::ipv4({10, 4, 5, 6}));
    EXPECT_EQ(dns.last_service, "9297");
  }
}

TEST_F(PingTargetResolve, ReportsADnsFailure) {
  dns.fail = true;
  const auto failed = resolve("dc.example", 4480);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), Error::dns_failure);
}

TEST_F(PingTargetResolve, NeedsAnIpv4Address) {
  dns.answers = {platform::IpAddress::loopback_ipv6()};
  const auto ipv6_only = resolve("dc.example", 4480);
  ASSERT_FALSE(ipv6_only);
  EXPECT_EQ(ipv6_only.error(), Error::no_ipv4_address);
}

TEST_F(PingTargetResolve, TakesTheFirstIpv4Address) {
  dns.answers = {platform::IpAddress::loopback_ipv6(),
                 platform::IpAddress::ipv4({10, 1, 2, 3}),
                 platform::IpAddress::ipv4({10, 9, 9, 9})};
  const auto selected = resolve("dc.example", 4480);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected.value().endpoint().address,
            platform::IpAddress::ipv4({10, 1, 2, 3}));
  EXPECT_EQ(selected.value().endpoint().port, 4480);
  EXPECT_EQ(dns.last_host, "dc.example");
  EXPECT_EQ(dns.last_service, "4480");
}

}  // namespace
}  // namespace cloud_test
