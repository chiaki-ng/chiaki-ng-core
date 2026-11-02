#include "src/core/play/adapters/links/internet/wire/psn_remote_registration.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/psn_remote_registration_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace remote_play = libchiaking::net::protocol::remote_play;
using psn_remote_registration::PairingCrypto;
using RequestError = internet::PsnRequestError;
using Target = remote_play::RegistrationTarget;

class RudpPairingRequest : public ::testing::Test {
 protected:
  auto build(Target target, std::span<const std::byte> account_id,
             std::string_view host) {
    return internet::RudpPairing::build_request(
        random_, crypto_, target, account_id, data, data, data, host);
  }

  auto build(Target target = Target::ps5_1) {
    return build(target, account, "10.0.2.15");
  }

  static constexpr std::array<std::byte, 8> account{};
  static constexpr std::array<std::byte, 16> data{};

  test_support::CountingRandom random_;
  PairingCrypto crypto_;
};

TEST_F(RudpPairingRequest, RefusesAPs4NineTargetBeforeDrawingRandomBytes) {
  const auto request = build(Target::ps4_9);
  ASSERT_FALSE(request);
  EXPECT_EQ(request.error(), RequestError::invalid_target);
  EXPECT_EQ(random_.calls, 0U);
}

TEST_F(RudpPairingRequest, RefusesAShortAccountIdBeforeDrawingRandomBytes) {
  const auto request = build(Target::ps5_1, {account.data(), 7}, "10.0.2.15");
  ASSERT_FALSE(request);
  EXPECT_EQ(request.error(), RequestError::invalid_account_id);
  EXPECT_EQ(random_.calls, 0U);
}

TEST_F(RudpPairingRequest, RefusesAHostThatBreaksTheHeader) {
  const auto request = build(Target::ps5_1, account, "host\r\ninject");
  ASSERT_FALSE(request);
  EXPECT_EQ(request.error(), RequestError::invalid_host);
  EXPECT_EQ(random_.calls, 0U);
}

TEST_F(RudpPairingRequest, JoinsTheHeaderAndPayloadForEachTarget) {
  for (const auto target : {Target::ps4_10, Target::ps5_1}) {
    SCOPED_TRACE(static_cast<int>(target));
    const auto request = build(target);
    ASSERT_TRUE(request);
    const auto payload = remote_play::RegistrationRequestCodec::encode_payload(
        crypto_, target, request.value().material, account);
    ASSERT_TRUE(payload);
    const auto header = remote_play::RegistrationRequestCodec::encode_header(
        target, payload.value().size(), "10.0.2.15");
    ASSERT_TRUE(header);
    const auto& bytes = request.value().bytes;
    ASSERT_EQ(bytes.size(), header.value().size() + payload.value().size());
    EXPECT_TRUE(
        std::ranges::equal(std::span{bytes}.first(header.value().size()),
                           std::as_bytes(std::span{header.value()})));
    EXPECT_TRUE(std::ranges::equal(
        payload.value(), std::span{bytes}.subspan(header.value().size())));
  }
  EXPECT_EQ(random_.calls, 2U);
}

TEST_F(RudpPairingRequest, ReportsARandomFailure) {
  random_.fail = true;
  const auto request = build();
  ASSERT_FALSE(request);
  EXPECT_EQ(request.error(), RequestError::random_failure);
}

TEST_F(RudpPairingRequest, ReportsAMaterialFailure) {
  crypto_.fail_on_hmac(crypto_.hmac_calls() + 1);
  const auto request = build();
  ASSERT_FALSE(request);
  EXPECT_EQ(request.error(), RequestError::material_failure);
}

TEST_F(RudpPairingRequest, ReportsAPayloadFailure) {
  crypto_.fail_on_hmac(crypto_.hmac_calls() + 2);
  const auto request = build();
  ASSERT_FALSE(request);
  EXPECT_EQ(request.error(), RequestError::payload_failure);
}

}  // namespace
}  // namespace play_test
