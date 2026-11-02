#include "tests/support/net/scripted_time_servers.hpp"
#include "tests/support/random.hpp"

#include "src/core/catalog/ntp_clock.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

using libchiaking::catalog::NtpClock;
using TrustedTime = libchiaking::catalog::TrustedClock::Time;

class CatalogNtpClock : public ::testing::Test {
 protected:
  void cloudflare_answers() {
    network.resolves("time.cloudflare.com",
                     {test_support::net::ScriptedTimeServers::v4(1),
                      test_support::net::ScriptedTimeServers::v4(2)});
    network.serves(test_support::net::ScriptedTimeServers::v4(1), {});
    network.serves(test_support::net::ScriptedTimeServers::v4(2), {});
  }

  auto truth() const -> TrustedTime {
    return std::chrono::floor<std::chrono::milliseconds>(network.true_now());
  }

  test_support::net::ScriptedTimeServers network;
  test_support::ScriptedRandom random{0x41};
  libchiaking::platform::DefaultThreadingProvider threading;
  NtpClock clock{network, threading, network.clock, random};
};

TEST_F(CatalogNtpClock, HoldsNoTimeUntilARefreshSyncs) {
  cloudflare_answers();
  EXPECT_FALSE(clock.now());
  EXPECT_FALSE(clock.source());
  EXPECT_EQ(network.queries(), 0);
  EXPECT_EQ(clock.refresh({}, {}), truth());
  EXPECT_EQ(clock.now(), truth());
  const auto source = clock.source();
  ASSERT_TRUE(source);
  EXPECT_EQ(source->source(), libchiaking::net::ntp::TimeSource::cloudflare);
  EXPECT_EQ(network.queries(), 2);
}

TEST_F(CatalogNtpClock, KeepsASyncedTimeWithoutAskingAgain) {
  cloudflare_answers();
  EXPECT_EQ(clock.refresh({}, {}), truth());
  const auto asked = network.queries();
  EXPECT_EQ(asked, 2);
  network.clock.advance(std::chrono::minutes{59});
  EXPECT_EQ(clock.now(), truth());
  EXPECT_EQ(clock.refresh({}, {}), truth());
  EXPECT_EQ(network.queries(), asked);
}

TEST_F(CatalogNtpClock, HoldsNoTimeOnceItExpiresUntilTheNextSync) {
  cloudflare_answers();
  EXPECT_TRUE(clock.refresh({}, {}));
  network.clock.advance(std::chrono::hours{1});
  EXPECT_FALSE(clock.now());
  EXPECT_FALSE(clock.source());
  EXPECT_EQ(network.queries(), 2);
  EXPECT_EQ(clock.refresh({}, {}), truth());
  EXPECT_EQ(network.queries(), 4);
}

TEST_F(CatalogNtpClock, WaitsAfterAFailedSyncBeforeTryingAgain) {
  EXPECT_FALSE(clock.refresh({}, {}));
  EXPECT_EQ(network.lookups("time.cloudflare.com"), 1);
  EXPECT_FALSE(clock.refresh({}, {}));
  EXPECT_EQ(network.lookups("time.cloudflare.com"), 1);
  network.clock.advance(NtpClock::retry_after_failure);
  cloudflare_answers();
  EXPECT_EQ(clock.refresh({}, {}), truth());
  EXPECT_EQ(network.lookups("time.cloudflare.com"), 2);
}

TEST_F(CatalogNtpClock, TellsTheHostsItAsksOnlyWhileSyncing) {
  cloudflare_answers();
  std::vector<std::string> told;
  const auto tell = [&told](std::string_view host) { told.emplace_back(host); };
  EXPECT_EQ(clock.refresh({}, tell), truth());
  EXPECT_EQ(told, std::vector<std::string>{"time.cloudflare.com"});
  EXPECT_EQ(clock.refresh({}, tell), truth());
  EXPECT_EQ(told.size(), 1U);
}

}  // namespace
}  // namespace catalog_test
