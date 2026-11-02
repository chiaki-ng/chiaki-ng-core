#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/crypto/provider.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>

namespace platform_crypto_test {
namespace {

using libchiaking::Expected;
using libchiaking::platform::CryptoError;
using libchiaking::platform::CryptoProvider;
using libchiaking::platform::ScryptParameters;

class Unsupported final : public CryptoProvider {
 public:
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> Expected<void, CryptoError> override {
    return {};
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte>)
      -> Expected<void, CryptoError> override {
    return {};
  }
  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>, std::span<std::byte>)
      -> Expected<void, CryptoError> override {
    return {};
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> Expected<void, CryptoError> override {
    return {};
  }
};

class CryptoProviderDefaults : public ::testing::Test {
 protected:
  template <class Result>
  [[nodiscard]] static auto unsupported(const Result& result)
      -> ::testing::AssertionResult {
    if (result) return ::testing::AssertionFailure() << "the call succeeded";
    if (result.error() != CryptoError::unsupported_capability)
      return ::testing::AssertionFailure()
             << "failed with " << static_cast<int>(result.error())
             << " rather than unsupported_capability";
    return ::testing::AssertionSuccess();
  }

  Unsupported provider;
  std::array<std::byte, 64> wide{};
  std::array<std::byte, 65> point{};
  std::array<std::byte, 32> narrow{};
  std::array<std::byte, 20> sha1{};
};

TEST_F(CryptoProviderDefaults, ReportTheMissingCapabilityForSha512Digests) {
  EXPECT_TRUE(unsupported(provider.sha512({}, wide)));
  EXPECT_TRUE(unsupported(provider.hmac_sha512({}, {}, wide)));
}

TEST_F(CryptoProviderDefaults, ReportTheMissingCapabilityForHmacSha1) {
  EXPECT_TRUE(unsupported(provider.hmac_sha1({}, {}, sha1)));
}

TEST_F(CryptoProviderDefaults, ReportTheMissingCapabilityForScrypt) {
  EXPECT_TRUE(
      unsupported(provider.scrypt({}, {}, ScryptParameters{2, 1, 1}, wide)));
}

TEST_F(CryptoProviderDefaults, ReportTheMissingCapabilityForEd25519) {
  EXPECT_TRUE(unsupported(provider.ed25519_public_key(narrow, narrow)));
  EXPECT_TRUE(unsupported(provider.ed25519_sign(narrow, {}, wide)));
  EXPECT_TRUE(unsupported(provider.ed25519_verify(narrow, {}, wide)));
}

TEST_F(CryptoProviderDefaults, ReportTheMissingCapabilityForP256Arithmetic) {
  EXPECT_TRUE(unsupported(provider.p256_base_multiply(narrow, point)));
  EXPECT_TRUE(unsupported(provider.p256_multiply(narrow, point, point)));
  EXPECT_TRUE(unsupported(provider.p256_add(point, point, point)));
}

}  // namespace
}  // namespace platform_crypto_test
