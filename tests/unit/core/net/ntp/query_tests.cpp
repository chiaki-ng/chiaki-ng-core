#include "libchiaking/platform/net/types.hpp"
#include "src/core/net/ntp/query.hpp"
#include "tests/unit/core/net/ntp/golden_reply.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace ntp_test {
namespace {

namespace ntp = libchiaking::net::ntp;
namespace platform = libchiaking::platform;

class NtpQuerySample : public ::testing::Test {
 protected:
  void SetUp() override {
    auto decoded = GoldenReply::reply(GoldenReply::valid());
    ASSERT_TRUE(decoded);
    reply.emplace(decoded.value());
  }

  const platform::IpAddress server = platform::IpAddress::ipv4({192, 0, 2, 1});
  std::optional<ntp::Packet> reply;
};

TEST_F(NtpQuerySample, TakesHalfTheNetworkDelay) {
  const auto sample = ntp::Query::sample_of(*reply, server, 10'000, 50'250);
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample.value().server, server);
  EXPECT_EQ(sample.value().at_us, 50'250U);
  EXPECT_EQ(sample.value().time,
            GoldenReply::transmitted + std::chrono::microseconds{20'000});
}

TEST_F(NtpQuerySample, IsRefusedWhenTheRoundTripTakesASecond) {
  EXPECT_FALSE(ntp::Query::sample_of(*reply, server, 10'000, 1'010'250));
  EXPECT_TRUE(ntp::Query::sample_of(*reply, server, 10'000, 1'010'249));
}

TEST_F(NtpQuerySample, IsRefusedWhenTheServerHeldItLongerThanTheRoundTrip) {
  EXPECT_FALSE(ntp::Query::sample_of(*reply, server, 10'000, 10'200));
}

TEST_F(NtpQuerySample, IsRefusedWhenTheReplyArrivedBeforeTheRequest) {
  EXPECT_FALSE(ntp::Query::sample_of(*reply, server, 10'000, 9'000));
}

TEST_F(NtpQuerySample, IsRefusedWhenTheServerTimesRunBackwards) {
  const auto reversed = GoldenReply::of({
      0x24, 0x01, 0x03, 0xe7, 0,    0,    0,    0x10, 0,    0,    0,    0x20,
      0x47, 0x4f, 0x4f, 0x47, 0xe8, 0xa2, 0x4a, 0x00, 0,    0,    0,    0,
      0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0xe8, 0xa2, 0x4a, 0x10,
      0x80, 0x10, 0x62, 0x4e, 0xe8, 0xa2, 0x4a, 0x10, 0x80, 0,    0,    0,
  });
  const auto backwards = GoldenReply::reply(reversed);
  ASSERT_TRUE(backwards);
  EXPECT_FALSE(ntp::Query::sample_of(backwards.value(), server, 0, 100'000));
}

}  // namespace
}  // namespace ntp_test
