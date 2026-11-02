#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/ntp/time_sync.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/ntp/time_sync_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ntp_test {

using Reply = libchiaking::net::ntp::Packet::Datagram;

namespace ntp = libchiaking::net::ntp;
namespace platform = libchiaking::platform;
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

class NtpTimeSync : public ::testing::Test, protected TimeSyncRig {};

TEST_F(NtpTimeSync, TrustsCloudflareWhenItsServersAgree) {
  cloudflare_resolves({v4(1), v6(1), v4(2), v6(2)});
  for (const auto& address : {v4(1), v4(2), v6(1), v6(2)})
    network.serves(address, {});
  google_at(seconds{9});
  const auto trusted = synced();
  ASSERT_TRUE(trusted);
  EXPECT_EQ(trusted->source(), ntp::TimeSource::cloudflare);
  EXPECT_TRUE(reads(*trusted, {}));
  for (const auto& address : {v4(1), v4(2), v6(1), v6(2)})
    EXPECT_EQ(network.asked(address), 1) << platform::to_string(address);
  EXPECT_EQ(network.lookups(google(1)), 0);
  EXPECT_EQ(network.asked(v4(11)), 0);

  network.clock.advance(std::chrono::minutes{10});
  EXPECT_TRUE(reads(*trusted, {}));
}

TEST_F(NtpTimeSync, FallsBackToGoogleWhenCloudflareDisagrees) {
  cloudflare_resolves({v4(1), v4(2)});
  network.serves(v4(1), {});
  network.serves(v4(2), {.skew = seconds{5}});
  google_at(milliseconds{200});
  const auto trusted = synced();
  ASSERT_TRUE(trusted);
  EXPECT_EQ(trusted->source(), ntp::TimeSource::google);
  EXPECT_TRUE(reads(*trusted, milliseconds{200}));
  for (std::uint8_t host = 11; host <= 14; ++host)
    EXPECT_EQ(network.asked(v4(host)), 1) << "host " << +host;
}

TEST_F(NtpTimeSync, DoesNotTrustASingleSample) {
  cloudflare_resolves({v4(1), v4(2)});
  network.serves(v4(1), {});
  EXPECT_FALSE(synced());
  EXPECT_EQ(network.asked(v4(2)), 1);
}

TEST_F(NtpTimeSync, FallsBackToGoogleWhenCloudflareDoesNotResolve) {
  google_at(milliseconds{-40});
  const auto trusted = synced();
  ASSERT_TRUE(trusted);
  EXPECT_EQ(trusted->source(), ntp::TimeSource::google);
  EXPECT_TRUE(reads(*trusted, -milliseconds{40}));
  EXPECT_EQ(network.lookups(cloudflare), 1);
}

TEST_F(NtpTimeSync, SendsNothingWhenNoHostResolves) {
  EXPECT_FALSE(synced());
  EXPECT_EQ(network.queries(), 0);
  EXPECT_EQ(network.lookups(cloudflare), 1);
  EXPECT_EQ(network.lookups(google(4)), 1);
}

TEST_F(NtpTimeSync, FailsWhenEveryServerIsSilent) {
  cloudflare_resolves({v4(1), v4(2)});
  network.serves(v4(1), {.silent = true});
  network.serves(v4(2), {.silent = true});
  EXPECT_FALSE(synced());
  EXPECT_EQ(network.asked(v4(1)), 1);
  EXPECT_EQ(network.asked(v4(2)), 1);
}

TEST_F(NtpTimeSync, NeverMixesSamplesAcrossProviders) {
  cloudflare_resolves({v4(1), v4(2)});
  network.serves(v4(1), {});
  network.serves(v4(2), {.silent = true});
  network.resolves(google(1), {v4(11)});
  network.serves(v4(11), {});
  for (std::uint8_t host = 2; host <= 4; ++host) {
    network.resolves(google(host), {v4(10 + host)});
    network.serves(v4(10 + host), {.silent = true});
  }
  EXPECT_FALSE(synced());
  EXPECT_EQ(network.asked(v4(1)), 1);
  EXPECT_EQ(network.asked(v4(11)), 1);
}

TEST_F(NtpTimeSync, DoesNotCountRejectedRepliesAsSamples) {
  cloudflare_resolves({v4(1), v4(2), v4(3), v4(4), v4(5)});
  network.serves(v4(1), {});
  network.serves(v4(2),
                 {.tamper = [](Reply& reply) { reply[1] = std::byte{0}; }});
  network.serves(v4(3), {.round_trip = milliseconds{1500}});
  network.serves(v4(4), {.forges_origin = true});
  network.serves(v4(5),
                 {.tamper = [](Reply& reply) { reply[0] = std::byte{0xe4}; }});
  google_at(milliseconds{70});
  const auto trusted = synced();
  ASSERT_TRUE(trusted);
  EXPECT_EQ(trusted->source(), ntp::TimeSource::google);
  EXPECT_TRUE(reads(*trusted, milliseconds{70}));
}

TEST_F(NtpTimeSync, TakesTheMedianOfTheAgreeingMajority) {
  cloudflare_resolves({v4(1), v4(2), v4(3)});
  network.serves(v4(1), {});
  network.serves(v4(2), {.skew = milliseconds{300}});
  network.serves(v4(3), {.skew = seconds{10}});
  const auto trusted = synced();
  ASSERT_TRUE(trusted);
  EXPECT_EQ(trusted->source(), ntp::TimeSource::cloudflare);
  EXPECT_TRUE(reads(*trusted, milliseconds{150}));
}

TEST_F(NtpTimeSync, TrustsNothingWhenTheServersSplitEvenly) {
  cloudflare_resolves({v4(1), v4(2), v4(3), v4(4)});
  network.serves(v4(1), {});
  network.serves(v4(2), {});
  network.serves(v4(3), {.skew = seconds{5}});
  network.serves(v4(4), {.skew = seconds{5}});
  EXPECT_FALSE(synced());
}

TEST_F(NtpTimeSync, AsksARepeatedAddressOnce) {
  cloudflare_resolves({v4(1), v4(1)});
  network.serves(v4(1), {});
  EXPECT_FALSE(synced());
  EXPECT_EQ(network.asked(v4(1)), 1);
}

TEST_F(NtpTimeSync, TellsEachHostAsItTurnsToIt) {
  cloudflare_resolves({v4(1), v4(2)});
  network.serves(v4(1), {});
  network.serves(v4(2), {.skew = seconds{5}});
  google_at(milliseconds{200});
  std::vector<std::string> told;
  ASSERT_TRUE(sync.sync(
      {}, [&told](std::string_view host) { told.emplace_back(host); }));
  EXPECT_EQ(told, (std::vector<std::string>{std::string{cloudflare}, google(1),
                                            google(2), google(3), google(4)}));
}

TEST_F(NtpTimeSync, TellsTheFirstHostEvenWhenItDoesNotResolve) {
  std::vector<std::string> told;
  EXPECT_FALSE(sync.sync(
      {}, [&told](std::string_view host) { told.emplace_back(host); }));
  EXPECT_EQ(told,
            (std::vector<std::string>{std::string{cloudflare}, google(1)}));
}

TEST(NtpTimeSyncAgreement, CountsARepeatedSampleOnce) {
  const ntp::TimeSync::Sample once{.server = ScriptedTimeServers::v4(1),
                                   .time = ScriptedTimeServers::true_start,
                                   .at_us = 10};
  const std::array twice{once, once};
  EXPECT_FALSE(ntp::TimeSync::agreed(ntp::TimeSource::cloudflare, twice));
}

TEST_F(NtpTimeSync, SendsNothingOnceStopped) {
  cloudflare_resolves({v4(1), v4(2)});
  network.serves(v4(1), {});
  network.serves(v4(2), {});
  platform::StopSource stop;
  stop.request_stop();
  EXPECT_FALSE(sync.sync(stop.token()));
  EXPECT_EQ(network.queries(), 0);
}

}  // namespace
}  // namespace ntp_test
