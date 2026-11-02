#include "src/core/play/adapters/links/internet/wire/psn_remote_registration.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/psn_remote_registration_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace remote_play = libchiaking::net::protocol::remote_play;
using psn_remote_registration::PairingCrypto;
using psn_remote_registration::Replies;
using ResponseError = internet::PsnResponseError;
using Target = remote_play::RegistrationTarget;

class RudpPairingResponse : public ::testing::Test {
 protected:
  auto finalize(const remote_play::RudpCtrlHttpExchangeOutcome& outcome,
                Target target = Target::ps5_1) {
    return internet::RudpPairing::finalize_response(outcome, crypto_, target,
                                                    material_);
  }

  static auto good() -> remote_play::RudpCtrlHttpExchangeOutcome {
    return Replies::outcome(Replies::accepted_http(Replies::ps5_body));
  }

  static auto padded() -> remote_play::RudpCtrlHttpExchangeOutcome {
    return Replies::outcome("HTTP/1.1 200 OK\r\nContent-Length:        " +
                            std::to_string(Replies::ps5_body.size()) +
                            "\r\n\r\n" + std::string(Replies::ps5_body));
  }

  PairingCrypto crypto_;

 private:
  remote_play::RegistrationMaterial material_;
};

TEST_F(RudpPairingResponse, ReadsTheRegistrationKey) {
  const auto result = finalize(good());
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().registration_key[0], std::byte{0x01});
  EXPECT_EQ(result.value().registration_key[15], std::byte{0x0f});
}

TEST_F(RudpPairingResponse, ReadsAContentLengthPaddedWithSpaces) {
  const auto result = finalize(good());
  ASSERT_TRUE(result);
  const auto padded_result = finalize(padded());
  ASSERT_TRUE(padded_result);
  EXPECT_TRUE(std::ranges::equal(padded_result.value().registration_key,
                                 result.value().registration_key));
}

TEST_F(RudpPairingResponse, ReportsATruncatedBodyAfterAPaddedLength) {
  auto truncated = padded();
  truncated.response.received.pop_back();
  const auto result = finalize(truncated);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), ResponseError::truncated);
}

TEST_F(RudpPairingResponse, ReportsARefusal) {
  const auto result =
      finalize(Replies::outcome("HTTP/1.1 403 Forbidden\r\n\r\n"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), ResponseError::refused);
}

TEST_F(RudpPairingResponse, ReportsATruncatedBody) {
  auto truncated = good();
  truncated.response.received.pop_back();
  const auto result = finalize(truncated);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), ResponseError::truncated);
}

TEST_F(RudpPairingResponse, AcceptsFieldsPastTheContentLength) {
  auto excess = good();
  Replies::append(excess, "X-Extra: allowed\r\n");
  EXPECT_TRUE(finalize(excess));
}

TEST_F(RudpPairingResponse, ReadsANicknamePastTheContentLength) {
  auto beyond = good();
  Replies::append(beyond, "PS5-Nickname: Beyond\r\n");
  const auto result = finalize(beyond);
  ASSERT_TRUE(result);
  EXPECT_EQ(std::string_view(result.value().nickname.data()), "Beyond");
}

TEST_F(RudpPairingResponse, IgnoresTrailingBytesWithoutAColon) {
  auto trailing = good();
  Replies::append(trailing, "\x7f\x13junk-without-colon");
  const auto result = finalize(trailing);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().mac[5], std::byte{0x55});
}

TEST_F(RudpPairingResponse, ReadsAPs4Reply) {
  constexpr std::string_view ps4_body =
      "AP-Ssid: PS4-7A1B2C3D4E5F\r\n"
      "AP-Bssid: 7a1b2c3d4e5f\r\n"
      "AP-Key: 5f4e3d2c\r\n"
      "AP-Name: PS4\r\n"
      "PS4-Mac: 7a1b2c3d4e5f\r\n"
      "PS4-RegistKey: 3a5f9c1e\r\n"
      "PS4-Nickname: PS4-Living\r\n"
      "RP-KeyType: 2\r\n"
      "RP-Key: 9ae3f2b1c0d4e5f60718293a4b5c6d7e\r\n";
  const auto ps4 = Replies::outcome(Replies::accepted_http(ps4_body) +
                                    std::string(16, '\0'));
  const auto result = finalize(ps4, Target::ps4_10);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().key_type, 2);
  EXPECT_EQ(std::string_view(result.value().nickname.data()), "PS4-Living");
  EXPECT_EQ(result.value().registration_key[3], std::byte{0x1e});
  EXPECT_EQ(result.value().registration_key[4], std::byte{0});
  EXPECT_EQ(result.value().key[15], std::byte{0x7e});
}

TEST_F(RudpPairingResponse, RefusesAnUnfinishedHeader) {
  const auto result = finalize(Replies::outcome("HTTP/1.1 200 OK\r\n"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), ResponseError::invalid_header);
}

TEST_F(RudpPairingResponse, RefusesABodyWithoutAContentLength) {
  const auto result = finalize(Replies::outcome(
      "HTTP/1.1 200 OK\r\n\r\n" + std::string(Replies::ps5_body)));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), ResponseError::invalid_header);
}

TEST_F(RudpPairingResponse, ReportsACryptoFailure) {
  crypto_.fail(true);
  const auto result = finalize(good());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), ResponseError::crypto_failure);
}

}  // namespace
}  // namespace play_test
