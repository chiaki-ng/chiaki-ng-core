#include "src/core/net/ntp/packet.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/ntp/golden_reply.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace ntp_test {
namespace {

namespace ntp = libchiaking::net::ntp;
using Rejection = GoldenReply::Rejection;

class NtpReply : public ::testing::Test {
 protected:
  static auto rejects(std::span<const std::byte> datagram, Rejection expected)
      -> testing::AssertionResult {
    const auto decoded = GoldenReply::reply(datagram);
    if (decoded) return testing::AssertionFailure() << "the reply was accepted";
    if (decoded.error() != expected)
      return testing::AssertionFailure()
             << "rejected as " << testing::PrintToString(decoded.error())
             << " rather than " << testing::PrintToString(expected);
    return testing::AssertionSuccess();
  }
};

TEST(NtpRequest, IsAClientPacketCarryingTheNonce) {
  constexpr auto expected = GoldenReply::of({
      0x23, 0, 0, 0, 0, 0, 0, 0, 0,    0,    0,    0,    0,    0,    0,    0,
      0,    0, 0, 0, 0, 0, 0, 0, 0,    0,    0,    0,    0,    0,    0,    0,
      0,    0, 0, 0, 0, 0, 0, 0, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
  });
  EXPECT_EQ(ntp::Packet::request(GoldenReply::nonce), expected);
}

TEST_F(NtpReply, GivesTheServerTimes) {
  const auto decoded = GoldenReply::reply(GoldenReply::valid());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().received().time(), GoldenReply::received);
  EXPECT_EQ(decoded.value().transmitted().time(), GoldenReply::transmitted);
}

TEST_F(NtpReply, AcceptsALongerDatagram) {
  std::array<std::byte, 68> longer{};
  std::ranges::copy(GoldenReply::valid(), longer.begin());
  EXPECT_TRUE(GoldenReply::reply(longer));
}

TEST_F(NtpReply, AcceptsOtherServerModesVersionsAndStrata) {
  EXPECT_TRUE(GoldenReply::reply(GoldenReply::changed(0, 0x1c)));
  EXPECT_TRUE(GoldenReply::reply(GoldenReply::changed(0, 0x64)));
  EXPECT_TRUE(GoldenReply::reply(GoldenReply::changed(1, 15)));
}

TEST_F(NtpReply, IsRejectedWhenNotFromAServer) {
  EXPECT_TRUE(rejects(GoldenReply::changed(0, 0x23), Rejection::not_a_server));
  EXPECT_TRUE(rejects(GoldenReply::changed(0, 0x25), Rejection::not_a_server));
}

TEST_F(NtpReply, IsRejectedForAnUnsupportedVersion) {
  EXPECT_TRUE(
      rejects(GoldenReply::changed(0, 0x14), Rejection::unsupported_version));
  EXPECT_TRUE(
      rejects(GoldenReply::changed(0, 0x2c), Rejection::unsupported_version));
}

TEST_F(NtpReply, IsRejectedAsAKissOfDeath) {
  EXPECT_TRUE(rejects(GoldenReply::changed(1, 0), Rejection::kiss_of_death));
}

TEST_F(NtpReply, IsRejectedForAStratumOutOfRange) {
  EXPECT_TRUE(
      rejects(GoldenReply::changed(1, 16), Rejection::stratum_out_of_range));
  EXPECT_TRUE(
      rejects(GoldenReply::changed(1, 255), Rejection::stratum_out_of_range));
}

TEST_F(NtpReply, IsRejectedFromAnUnsynchronisedServer) {
  EXPECT_TRUE(
      rejects(GoldenReply::changed(0, 0xe4), Rejection::unsynchronised));
}

TEST_F(NtpReply, IsRejectedWhenItAnswersAnotherRequest) {
  EXPECT_TRUE(
      rejects(GoldenReply::changed(31, 0xee), Rejection::origin_mismatch));
  const auto decoded =
      ntp::Packet::reply(GoldenReply::valid(), ntp::Timestamp{1});
  ASSERT_FALSE(decoded);
  EXPECT_EQ(decoded.error(), Rejection::origin_mismatch);
}

TEST_F(NtpReply, IsRejectedWhenShort) {
  const auto valid = GoldenReply::valid();
  for (std::size_t length = 0; length < ntp::Packet::size; ++length)
    EXPECT_TRUE(rejects(std::span<const std::byte>{valid}.first(length),
                        Rejection::short_packet))
        << "length " << length;
}

TEST_F(NtpReply, IsRejectedWithoutATransmitTime) {
  auto zero = GoldenReply::valid();
  for (std::size_t at = 40; at < 48; ++at) zero[at] = std::byte{0};
  EXPECT_TRUE(rejects(zero, Rejection::no_transmit_time));
}

TEST_F(NtpReply, IsRejectedWhenTheRootIsDispersed) {
  auto wide = GoldenReply::valid();
  wide[9] = std::byte{0x01};
  wide[11] = std::byte{0x00};
  EXPECT_TRUE(rejects(wide, Rejection::dispersed));
}

TEST_F(NtpReply, AcceptsTheWidestDispersionUnderTheLimit) {
  auto narrow = GoldenReply::valid();
  narrow[10] = std::byte{0xff};
  narrow[11] = std::byte{0xff};
  EXPECT_TRUE(GoldenReply::reply(narrow));
}

}  // namespace
}  // namespace ntp_test
