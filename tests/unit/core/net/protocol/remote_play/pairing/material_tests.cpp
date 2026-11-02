#include "src/core/net/protocol/remote_play/pairing/material.hpp"
#include "src/core/utils/text/hex.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
using libchiaking::net::protocol::remote_play::MaterialError;
using libchiaking::net::protocol::remote_play::RegistrationMaterial;
using libchiaking::net::protocol::remote_play::RegistrationTarget;

class RegistrationMaterialZeroCrypto final : public platform::CryptoProvider {
 public:
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    ++calls;
    if (fail)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    for (auto& b : output) b = std::byte{0};
    return {};
  }
  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>,
                            std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    ++calls;
    for (auto& b : output) b = std::byte{0};
    return {};
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
  unsigned calls{};
  bool fail{};
};

struct RegistrationMaterialCase {
  RegistrationTarget target;
  std::string_view bright;
  std::string_view aeropause;
};

class RegistrationMaterialRig {
 protected:
  using Key = std::array<std::byte, 16>;

  static auto key(std::string_view hex) -> Key {
    Key result{};
    EXPECT_TRUE(libchiaking::text::decode_hex(hex, result)) << hex;
    return result;
  }

  static constexpr Key ambassador = [] {
    Key result{};
    for (std::size_t i = 0; i < result.size(); ++i)
      result[i] = static_cast<std::byte>(i);
    return result;
  }();
  static constexpr std::uint32_t pin = 0x01020304U;
};

class RegistrationMaterialLocal
    : public ::testing::TestWithParam<RegistrationMaterialCase>,
      protected RegistrationMaterialRig {};

TEST_P(RegistrationMaterialLocal, DerivesTheReferenceKeys) {
  const auto derived =
      RegistrationMaterial::derive_local(GetParam().target, ambassador, pin);
  ASSERT_TRUE(derived);
  EXPECT_TRUE(std::ranges::equal(derived.value().ambassador, ambassador));
  EXPECT_TRUE(
      std::ranges::equal(derived.value().bright, key(GetParam().bright)));
  EXPECT_TRUE(
      std::ranges::equal(derived.value().aeropause, key(GetParam().aeropause)));
}

INSTANTIATE_TEST_SUITE_P(
    EveryTarget, RegistrationMaterialLocal,
    ::testing::Values(
        RegistrationMaterialCase{RegistrationTarget::ps4_8,
                                 "3e1ec7b2dcbb3ecc50baedef9734c7c9",
                                 "363b4bed0a6adf522bd9caaf47e517d3"},
        RegistrationMaterialCase{RegistrationTarget::ps4_9,
                                 "3e1ec7b2dcbb3ecc50baedef9734c7c9",
                                 "363b4bed0a6adf522bd9caaf47e517d3"},
        RegistrationMaterialCase{RegistrationTarget::ps4_10,
                                 "cebcb640080776047b85e85bf2ee9767",
                                 "88142727c2e37371df5ed5a59cf77e8f"},
        RegistrationMaterialCase{RegistrationTarget::ps5_1,
                                 "d860e146bdb0bd94b460070bb04de427",
                                 "7b0a388cfad1da4345260d8aec78656c"}));

class RegistrationMaterialDerivation : public ::testing::Test,
                                       protected RegistrationMaterialRig {
 protected:
  RegistrationMaterialZeroCrypto crypto;
  std::array<std::byte, 16> custom = [] {
    std::array<std::byte, 16> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
      result[i] = static_cast<std::byte>(0x10U + i);
    return result;
  }();
  std::array<std::byte, 16> data1{};
  std::array<std::byte, 16> data2{};
};

TEST_F(RegistrationMaterialDerivation, RefusesAnUnknownTarget) {
  EXPECT_FALSE(RegistrationMaterial::derive_local(
      static_cast<RegistrationTarget>(0xff), ambassador, pin));
}

TEST_F(RegistrationMaterialDerivation, RefusesAnAmbassadorOfTheWrongLength) {
  const std::array<std::byte, 17> oversized{};
  const auto empty =
      RegistrationMaterial::derive_local(RegistrationTarget::ps4_10, {}, pin);
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), MaterialError::invalid_ambassador);
  const auto short_one = RegistrationMaterial::derive_local(
      RegistrationTarget::ps5_1, std::span(ambassador).first(15), pin);
  ASSERT_FALSE(short_one);
  EXPECT_EQ(short_one.error(), MaterialError::invalid_ambassador);
  const auto long_one = RegistrationMaterial::derive_local(
      RegistrationTarget::ps4_8, oversized, pin);
  ASSERT_FALSE(long_one);
  EXPECT_EQ(long_one.error(), MaterialError::invalid_ambassador);
}

TEST_F(RegistrationMaterialDerivation, LeavesTheAmbassadorUnchanged) {
  auto copy = ambassador;
  static_cast<void>(
      RegistrationMaterial::derive_local(RegistrationTarget::ps5_1, copy, pin));
  EXPECT_EQ(copy, ambassador);
}

TEST_F(RegistrationMaterialDerivation, MixesThePinIntoTheLastFourBrightBytes) {
  const auto a = RegistrationMaterial::derive_local(RegistrationTarget::ps4_10,
                                                    ambassador, 0x01020304U);
  const auto b = RegistrationMaterial::derive_local(RegistrationTarget::ps4_10,
                                                    ambassador, 0x04030201U);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  for (std::size_t i = 0; i < 12; ++i) {
    EXPECT_EQ(a.value().bright[i], b.value().bright[i]) << "byte " << i;
    EXPECT_EQ(a.value().aeropause[i], b.value().aeropause[i]) << "byte " << i;
  }
  for (std::size_t i = 12; i < 16; ++i) {
    EXPECT_NE(a.value().bright[i], b.value().bright[i]) << "byte " << i;
    EXPECT_EQ(a.value().aeropause[i], b.value().aeropause[i]) << "byte " << i;
  }
  EXPECT_EQ(a.value().bright[12], std::byte{0xf2});
  EXPECT_EQ(b.value().bright[15], std::byte{0x62});
}

TEST_F(RegistrationMaterialDerivation,
       MixesThePinIntoTheFirstBrightBytesBeforeVersionTen) {
  const auto derived = RegistrationMaterial::derive_local(
      RegistrationTarget::ps4_8, ambassador, 0xffffffffU);
  ASSERT_TRUE(derived);
  EXPECT_EQ(derived.value().bright[0], std::byte{0xc0});
  EXPECT_EQ(derived.value().bright[1], std::byte{0xe3});
}

TEST_F(RegistrationMaterialDerivation,
       XorsPsnCustomDataIntoTheLocalBrightWithAZeroKeystream) {
  for (const auto target :
       {RegistrationTarget::ps4_10, RegistrationTarget::ps5_1}) {
    SCOPED_TRACE(testing::PrintToString(target));
    const auto psn = RegistrationMaterial::derive_psn(
        crypto, target, ambassador, custom, data1, data2);
    const auto base = RegistrationMaterial::derive_local(target, ambassador, 0);
    ASSERT_TRUE(psn);
    ASSERT_TRUE(base);
    EXPECT_GT(crypto.calls, 0U);
    EXPECT_TRUE(std::ranges::equal(
        psn.value().aeropause, key(target == RegistrationTarget::ps5_1
                                       ? "831c4a9e0cd72c454968118c2abea1ae"
                                       : "883c292f4667959171f27fadbc1b9e8f")));
    for (std::size_t i = 0; i < 16; ++i)
      EXPECT_EQ(psn.value().bright[i], base.value().bright[i] ^ custom[i])
          << "byte " << i;
  }
}

TEST_F(RegistrationMaterialDerivation, RefusesPsnDataForAnOlderPs4) {
  const auto result = RegistrationMaterial::derive_psn(
      crypto, RegistrationTarget::ps4_9, ambassador, custom, data1, data2);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), MaterialError::invalid_target);
}

TEST_F(RegistrationMaterialDerivation, RefusesShortPsnData) {
  const auto short_custom = RegistrationMaterial::derive_psn(
      crypto, RegistrationTarget::ps5_1, ambassador,
      std::span(custom).first(15), data1, data2);
  ASSERT_FALSE(short_custom);
  EXPECT_EQ(short_custom.error(), MaterialError::invalid_psn_data);
  const auto short_data = RegistrationMaterial::derive_psn(
      crypto, RegistrationTarget::ps5_1, ambassador, custom,
      std::span(data1).first(15), data2);
  ASSERT_FALSE(short_data);
  EXPECT_EQ(short_data.error(), MaterialError::invalid_psn_data);
}

TEST_F(RegistrationMaterialDerivation, ReportsAProviderFailure) {
  crypto.fail = true;
  const auto result = RegistrationMaterial::derive_psn(
      crypto, RegistrationTarget::ps5_1, ambassador, custom, data1, data2);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), MaterialError::provider_failure);
}

}  // namespace
}  // namespace remote_play_test
