#include "src/core/net/ntp/trusted_time.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/ntp/golden_reply.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace ntp_test {
namespace {

namespace ntp = libchiaking::net::ntp;

class NtpTrustedTime : public ::testing::Test {
 protected:
  static constexpr std::uint64_t synced_us = 5'000'000;
  static constexpr auto hour_us = static_cast<std::uint64_t>(3'600'000'000);
  const ntp::TrustedTime trusted{ntp::TimeSource::google, GoldenReply::received,
                                 synced_us};
};

TEST_F(NtpTrustedTime, RemembersItsSourceAndSyncTime) {
  EXPECT_EQ(trusted.source(), ntp::TimeSource::google);
  EXPECT_EQ(trusted.synced_at(), GoldenReply::received);
}

TEST_F(NtpTrustedTime, AdvancesWithTheMonotonicClock) {
  EXPECT_EQ(trusted.at(synced_us), GoldenReply::received);
  EXPECT_EQ(trusted.at(5'250'000),
            GoldenReply::received + std::chrono::milliseconds{250});
}

TEST_F(NtpTrustedTime, ReadsNothingBeforeItWasSynced) {
  EXPECT_FALSE(trusted.at(4'999'999));
}

TEST_F(NtpTrustedTime, ExpiresAnHourAfterItWasSynced) {
  EXPECT_TRUE(trusted.at(synced_us + hour_us - 1));
  EXPECT_FALSE(trusted.at(synced_us + hour_us));
  EXPECT_FALSE(trusted.at(UINT64_MAX));
}

}  // namespace
}  // namespace ntp_test
