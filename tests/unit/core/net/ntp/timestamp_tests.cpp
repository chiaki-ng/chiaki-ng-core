#include "src/core/net/ntp/timestamp.hpp"
#include "tests/unit/core/net/ntp/golden_reply.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>

namespace ntp_test {
namespace {

namespace ntp = libchiaking::net::ntp;
using Time = GoldenReply::Time;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::seconds;

TEST(NtpTimestamp, ZeroIsTheStartOfThe2036Era) {
  EXPECT_EQ(ntp::Timestamp{0}.time(), Time{seconds{2'085'978'496}});
}

TEST(NtpTimestamp, TheLastSecondOfEraZeroEndsJustBefore2036) {
  EXPECT_EQ(ntp::Timestamp{0xffff'ffffULL << 32U}.time(),
            Time{seconds{2'085'978'495}});
}

TEST(NtpTimestamp, RoundTripsWithinAMicrosecondAcrossTheEra) {
  const std::array times{GoldenReply::transmitted,
                         Time{seconds{2'085'978'496} + microseconds{7}},
                         Time{seconds{2'200'000'000} + milliseconds{250}}};
  for (const auto time : times) {
    const auto there_and_back = ntp::Timestamp::at(time).time();
    EXPECT_GE(time - there_and_back, microseconds{0});
    EXPECT_LE(time - there_and_back, microseconds{1});
  }
}

TEST(NtpTimestamp, EncodesTheWireValue) {
  EXPECT_EQ(ntp::Timestamp::at(GoldenReply::received).wire(),
            0xe8a2'4a10'8000'0000ULL);
}

}  // namespace
}  // namespace ntp_test
