#pragma once

#include "src/core/net/ntp/time_sync.hpp"
#include "tests/support/net/scripted_time_servers.hpp"
#include "tests/support/random.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ntp_test {

namespace ntp = libchiaking::net::ntp;
namespace platform = libchiaking::platform;

using test_support::net::ScriptedTimeServers;

class TimeSyncRig {
 public:
  static constexpr std::string_view cloudflare = "time.cloudflare.com";

  static auto v4(std::uint8_t last) -> platform::IpAddress {
    return ScriptedTimeServers::v4(last);
  }

  static auto v6(std::uint8_t last) -> platform::IpAddress {
    return ScriptedTimeServers::v6(last);
  }

  static auto google(std::uint8_t host) -> std::string {
    return "time" + std::to_string(host) + ".google.com";
  }

  auto synced() -> std::optional<ntp::TrustedTime> { return sync.sync({}); }

  auto reads(const ntp::TrustedTime& trusted,
             std::chrono::microseconds skew) const -> testing::AssertionResult {
    const auto read = trusted.at(network.clock.now_microseconds());
    if (!read)
      return testing::AssertionFailure() << "the trusted time has expired";
    const auto off = *read - (network.true_now() + skew);
    if (off < std::chrono::microseconds{-2} ||
        off > std::chrono::microseconds{2})
      return testing::AssertionFailure()
             << "reads " << off.count() << "us away from the expected time";
    return testing::AssertionSuccess();
  }

  void cloudflare_resolves(std::vector<platform::IpAddress> addresses) {
    network.resolves(std::string{cloudflare}, std::move(addresses));
  }

  void google_at(std::chrono::microseconds skew) {
    for (std::uint8_t host = 1; host <= 4; ++host) {
      network.resolves(google(host), {v4(10 + host)});
      network.serves(v4(10 + host), {.skew = skew});
    }
  }

  ScriptedTimeServers network;
  test_support::ScriptedRandom random{0x41};
  ntp::TimeSync sync{network, network.clock, random};
};

}  // namespace ntp_test
