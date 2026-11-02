#include "tests/unit/platform/net/sockets/rig.hpp"

#include <libchiaking/platform/net/conformance.hpp>
#include <libchiaking/platform/net/default.hpp>

#include <gtest/gtest.h>

#include <chrono>

namespace platform_net_test {
namespace {

using libchiaking::platform::DefaultDnsProvider;
using libchiaking::platform::DefaultPlatformFactory;

using NetworkConformance = SocketRig;

TEST_F(NetworkConformance, InterfacesIncludeAnIpv4Loopback) {
  auto interfaces = factory.network().interfaces().enumerate();
  ASSERT_TRUE(interfaces);
  bool loopback = false;
  for (const auto& info : interfaces.value())
    if (info.loopback && info.address.is_ipv4()) loopback = true;
  EXPECT_TRUE(loopback);
}

TEST(DefaultDns, GivesACancellableResolveAtLeastFifteenSeconds) {
  EXPECT_GE(DefaultDnsProvider::cancellable_resolve_timeout,
            std::chrono::seconds{15});
}

TEST_F(NetworkConformance, PassesTheNetworkConformanceSuite) {
  DefaultPlatformFactory foreign;
  const auto report =
      libchiaking::platform::net_conformance::run(factory, foreign);
  EXPECT_TRUE(report.ok) << "failed mask 0x" << std::hex
                         << static_cast<unsigned>(report.failed_mask);
}

}  // namespace
}  // namespace platform_net_test
