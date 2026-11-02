#include "src/core/utils/text/hex.hpp"
#include "src/platform/crypto/armv8/gmac.hpp"

#include <libchiaking/platform/crypto/conformance.hpp>
#include <libchiaking/platform/crypto/openssl/provider.hpp>
#include <libchiaking/platform/crypto/provider.hpp>
#include <libchiaking/platform/default_factory.hpp>
#include <libchiaking/platform/runtime/random_provider.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace platform_crypto_test {
namespace {

using libchiaking::Expected;
using libchiaking::platform::aes128_block_size;
using libchiaking::platform::aes128_gcm_iv_size_max;
using libchiaking::platform::aes128_gcm_iv_size_min;
using libchiaking::platform::aes128_gcm_tag_size;
using libchiaking::platform::aes128_gcm_tag_size_max;
using libchiaking::platform::aes128_gcm_tag_size_min;
using libchiaking::platform::aes128_key_size;
using libchiaking::platform::CryptoError;
using libchiaking::platform::CryptoProvider;
using libchiaking::platform::DefaultPlatformFactory;
using libchiaking::platform::SystemSecureRandom;
using libchiaking::platform::armv8::PmullGmac;
using libchiaking::platform::openssl::OpenSslCryptoProvider;
using libchiaking::text::hex_bytes;

class PmullGmacOverOpenSsl final : public CryptoProvider {
 public:
  [[nodiscard]] auto sha256(std::span<const std::byte> input,
                            std::span<std::byte> digest)
      -> Expected<void, CryptoError> override {
    return openssl_.sha256(input, digest);
  }

  [[nodiscard]] auto hmac_sha256(std::span<const std::byte> key,
                                 std::span<const std::byte> input,
                                 std::span<std::byte> digest)
      -> Expected<void, CryptoError> override {
    return openssl_.hmac_sha256(key, input, digest);
  }

  [[nodiscard]] auto aes128_encrypt_block(std::span<const std::byte> key,
                                          std::span<const std::byte> block,
                                          std::span<std::byte> output)
      -> Expected<void, CryptoError> override {
    return openssl_.aes128_encrypt_block(key, block, output);
  }

  [[nodiscard]] auto aes128_gmac(std::span<const std::byte> key,
                                 std::span<const std::byte> iv,
                                 std::span<const std::byte> aad,
                                 std::span<std::byte> tag)
      -> Expected<void, CryptoError> override {
    if (key.size() != aes128_key_size || iv.size() < aes128_gcm_iv_size_min ||
        iv.size() > aes128_gcm_iv_size_max ||
        tag.size() < aes128_gcm_tag_size_min ||
        tag.size() > aes128_gcm_tag_size_max)
      return libchiaking::unexpected(CryptoError::invalid_argument);
    constexpr PmullGmac::Block zero_block{};
    PmullGmac::Block hash_key{};
    if (auto encrypted =
            openssl_.aes128_encrypt_block(key, zero_block, hash_key);
        !encrypted)
      return encrypted;
    const PmullGmac gmac{hash_key};
    const auto counter = gmac.counter_block(iv);
    PmullGmac::Block encrypted_counter{};
    if (auto encrypted =
            openssl_.aes128_encrypt_block(key, counter, encrypted_counter);
        !encrypted)
      return encrypted;
    gmac.authenticate(aad, encrypted_counter, tag);
    return {};
  }

 private:
  OpenSslCryptoProvider openssl_;
};

class PmullGmacTest : public ::testing::Test {
 protected:
  [[nodiscard]] auto random_bytes(std::size_t size) -> std::vector<std::byte> {
    std::vector<std::byte> bytes(size);
    if (!random.fill_bytes(bytes)) bytes.clear();
    return bytes;
  }

  static constexpr std::size_t rounds = 2000;

  PmullGmacOverOpenSsl pmull;
  OpenSslCryptoProvider openssl;
  SystemSecureRandom random;
};

TEST_F(PmullGmacTest, PassesTheCryptoConformanceSuite) {
  DefaultPlatformFactory factory{&pmull};
  const auto report = libchiaking::platform::crypto_conformance::run(factory);
  EXPECT_TRUE(report.ok) << "failed mask 0x" << std::hex << report.failed_mask;
}

TEST_F(PmullGmacTest, MatchesTheGcmSpecificationKnownAnswer) {
  constexpr auto key = hex_bytes<16>("77be63708971c4e240d1cb79e8d77feb");
  constexpr auto iv = hex_bytes<12>("e0e00f19fed7ba0136a797f3");
  constexpr auto aad = hex_bytes<16>("7a43ec1d9c0a5a78a0b16533a6213cab");
  constexpr auto expected = hex_bytes<16>("209fcc8d3675ed938e9c7166709dd946");
  std::array<std::byte, aes128_gcm_tag_size> tag{};
  ASSERT_TRUE(pmull.aes128_gmac(key, iv, aad, tag));
  EXPECT_EQ(tag, expected);
}

TEST_F(PmullGmacTest, MatchesTheAllZeroKnownAnswer) {
  constexpr auto zero_key = hex_bytes<16>("00000000000000000000000000000000");
  constexpr auto zero_iv = hex_bytes<12>("000000000000000000000000");
  constexpr auto zero_tag = hex_bytes<16>("58e2fccefa7e3061367f1d57a4e7455a");
  std::array<std::byte, aes128_gcm_tag_size> tag{};
  ASSERT_TRUE(pmull.aes128_gmac(zero_key, zero_iv, {}, tag));
  EXPECT_EQ(tag, zero_tag);
}

TEST_F(PmullGmacTest, AgreesWithOpenSslOnRandomInputs) {
  for (std::size_t round = 0; round < rounds; ++round) {
    const auto key = random_bytes(aes128_key_size);
    const auto iv_size =
        aes128_gcm_iv_size_min +
        round % (aes128_gcm_iv_size_max - aes128_gcm_iv_size_min + 1);
    const auto iv = random_bytes(iv_size);
    const auto aad = random_bytes((round * 7) % 257);
    ASSERT_FALSE(key.empty()) << "round " << round;
    ASSERT_FALSE(iv.empty()) << "round " << round;
    const auto tag_size =
        aes128_gcm_tag_size_min +
        round % (aes128_gcm_tag_size_max - aes128_gcm_tag_size_min + 1);
    std::vector<std::byte> expected(tag_size);
    std::vector<std::byte> actual(tag_size);
    ASSERT_TRUE(openssl.aes128_gmac(key, iv, aad, expected))
        << "round " << round;
    ASSERT_TRUE(pmull.aes128_gmac(key, iv, aad, actual)) << "round " << round;
    EXPECT_EQ(actual, expected) << "round " << round;
  }
}

}  // namespace
}  // namespace platform_crypto_test
