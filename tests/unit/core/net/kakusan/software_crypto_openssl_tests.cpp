#include "libchiaking/platform/crypto/openssl/provider.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::platform;

class SoftwareCryptoAgainstOpenSsl : public ::testing::Test {
 protected:
  using Bytes = std::vector<std::byte>;

  auto random_bytes(std::size_t size) -> Bytes {
    Bytes bytes(size);
    for (auto& byte : bytes) byte = static_cast<std::byte>(generator() & 0xffU);
    return bytes;
  }

  auto draw(std::size_t bound) -> std::size_t {
    return static_cast<std::size_t>(generator() % bound);
  }

  test_support::SoftwareCrypto software;
  openssl::OpenSslCryptoProvider openssl;
  std::mt19937_64 generator{7};
};

TEST_F(SoftwareCryptoAgainstOpenSsl, AgreesOnDigestsAndMacs) {
  for (int round = 0; round < 200; ++round) {
    SCOPED_TRACE(round);
    const auto message = random_bytes(draw(400));
    const auto key = random_bytes(draw(200));
    std::array<std::byte, sha256_digest_size> narrow{};
    std::array<std::byte, sha256_digest_size> narrow_real{};
    EXPECT_TRUE(software.sha256(message, narrow));
    EXPECT_TRUE(openssl.sha256(message, narrow_real));
    EXPECT_EQ(narrow, narrow_real);
    EXPECT_TRUE(software.hmac_sha256(key, message, narrow));
    EXPECT_TRUE(openssl.hmac_sha256(key, message, narrow_real));
    EXPECT_EQ(narrow, narrow_real);
    std::array<std::byte, sha512_digest_size> wide{};
    std::array<std::byte, sha512_digest_size> wide_real{};
    EXPECT_TRUE(software.sha512(message, wide));
    EXPECT_TRUE(openssl.sha512(message, wide_real));
    EXPECT_EQ(wide, wide_real);
    EXPECT_TRUE(software.hmac_sha512(key, message, wide));
    EXPECT_TRUE(openssl.hmac_sha512(key, message, wide_real));
    EXPECT_EQ(wide, wide_real);
    std::array<std::byte, hmac_sha1_digest_size> legacy{};
    std::array<std::byte, hmac_sha1_digest_size> legacy_real{};
    EXPECT_TRUE(software.hmac_sha1(key, message, legacy));
    EXPECT_TRUE(openssl.hmac_sha1(key, message, legacy_real));
    EXPECT_EQ(legacy, legacy_real);
  }
}

TEST_F(SoftwareCryptoAgainstOpenSsl, AgreesOnTheBlockCipherAndGmac) {
  for (int round = 0; round < 200; ++round) {
    SCOPED_TRACE(round);
    const auto key = random_bytes(aes128_key_size);
    const auto block = random_bytes(aes128_block_size);
    std::array<std::byte, aes128_block_size> encrypted{};
    std::array<std::byte, aes128_block_size> encrypted_real{};
    EXPECT_TRUE(software.aes128_encrypt_block(key, block, encrypted));
    EXPECT_TRUE(openssl.aes128_encrypt_block(key, block, encrypted_real));
    EXPECT_EQ(encrypted, encrypted_real);
    const auto iv = random_bytes(aes128_gcm_iv_size_min + draw(5));
    const auto aad = random_bytes(draw(300));
    Bytes tag(aes128_gcm_tag_size_min + draw(13));
    Bytes tag_real(tag.size());
    EXPECT_TRUE(software.aes128_gmac(key, iv, aad, tag));
    EXPECT_TRUE(openssl.aes128_gmac(key, iv, aad, tag_real));
    EXPECT_EQ(tag, tag_real);
  }
}

TEST_F(SoftwareCryptoAgainstOpenSsl, AgreesOnSignaturesAndTheirVerification) {
  for (int round = 0; round < 100; ++round) {
    SCOPED_TRACE(round);
    const auto seed = random_bytes(ed25519_seed_size);
    const auto message = random_bytes(draw(300));
    std::array<std::byte, ed25519_public_key_size> key{};
    std::array<std::byte, ed25519_public_key_size> key_real{};
    EXPECT_TRUE(software.ed25519_public_key(seed, key));
    EXPECT_TRUE(openssl.ed25519_public_key(seed, key_real));
    EXPECT_EQ(key, key_real);
    std::array<std::byte, ed25519_signature_size> signature{};
    std::array<std::byte, ed25519_signature_size> signature_real{};
    EXPECT_TRUE(software.ed25519_sign(seed, message, signature));
    EXPECT_TRUE(openssl.ed25519_sign(seed, message, signature_real));
    EXPECT_EQ(signature, signature_real);
    const auto verified = software.ed25519_verify(key, message, signature);
    ASSERT_TRUE(verified);
    EXPECT_TRUE(verified.value());
    auto forged = signature;
    forged[draw(forged.size())] ^= std::byte{1};
    const auto forged_software = software.ed25519_verify(key, message, forged);
    const auto forged_openssl = openssl.ed25519_verify(key, message, forged);
    ASSERT_TRUE(forged_software);
    ASSERT_TRUE(forged_openssl);
    EXPECT_EQ(forged_software.value(), forged_openssl.value());
    auto other = key;
    other[draw(other.size())] ^= static_cast<std::byte>(1U << draw(8));
    const auto other_software =
        software.ed25519_verify(other, message, signature);
    const auto other_openssl =
        openssl.ed25519_verify(other, message, signature);
    ASSERT_TRUE(other_software);
    ASSERT_TRUE(other_openssl);
    EXPECT_EQ(other_software.value(), other_openssl.value());
  }
}

TEST_F(SoftwareCryptoAgainstOpenSsl, RefusesToVerifyWithAShortKey) {
  std::array<std::byte, 1> short_key{};
  std::array<std::byte, ed25519_signature_size> signature{};
  EXPECT_FALSE(software.ed25519_verify(short_key, {}, signature));
}

TEST_F(SoftwareCryptoAgainstOpenSsl, AgreesOnCurvePoints) {
  using Point = std::array<std::byte, p256_point_size>;
  for (int round = 0; round < 100; ++round) {
    SCOPED_TRACE(round);
    const auto first = random_bytes(p256_scalar_size);
    const auto second = random_bytes(p256_scalar_size);
    Point base{};
    Point base_real{};
    EXPECT_TRUE(software.p256_base_multiply(first, base));
    EXPECT_TRUE(openssl.p256_base_multiply(first, base_real));
    EXPECT_EQ(base, base_real);
    Point product{};
    Point product_real{};
    EXPECT_TRUE(software.p256_multiply(second, base, product));
    EXPECT_TRUE(openssl.p256_multiply(second, base, product_real));
    EXPECT_EQ(product, product_real);
    Point sum{};
    Point sum_real{};
    EXPECT_TRUE(software.p256_add(base, product, sum));
    EXPECT_TRUE(openssl.p256_add(base, product, sum_real));
    EXPECT_EQ(sum, sum_real);
    EXPECT_TRUE(software.p256_add(base, base, sum));
    EXPECT_TRUE(openssl.p256_add(base, base, sum_real));
    EXPECT_EQ(sum, sum_real);
    auto off_curve = base;
    off_curve[40] ^= std::byte{2};
    EXPECT_FALSE(software.p256_add(off_curve, base, sum));
    EXPECT_FALSE(openssl.p256_add(off_curve, base, sum_real));
    EXPECT_FALSE(software.p256_multiply(second, off_curve, product));
  }
}

TEST_F(SoftwareCryptoAgainstOpenSsl, RefusesAZeroScalar) {
  const std::array<std::byte, p256_scalar_size> zero{};
  std::array<std::byte, p256_point_size> nowhere{};
  EXPECT_FALSE(software.p256_base_multiply(zero, nowhere));
  EXPECT_FALSE(openssl.p256_base_multiply(zero, nowhere));
}

TEST_F(SoftwareCryptoAgainstOpenSsl, AgreesOnPasswordDerivation) {
  for (const auto parameters :
       {ScryptParameters{2, 1, 1}, ScryptParameters{1024, 8, 2},
        ScryptParameters{32768, 8, 1}}) {
    const auto password = random_bytes(9);
    const auto salt = random_bytes(16);
    std::array<std::byte, 48> derived{};
    std::array<std::byte, 48> derived_real{};
    EXPECT_TRUE(software.scrypt(password, salt, parameters, derived));
    EXPECT_TRUE(openssl.scrypt(password, salt, parameters, derived_real));
    EXPECT_EQ(derived, derived_real);
  }
}

TEST_F(SoftwareCryptoAgainstOpenSsl, RefusesInvalidScryptCosts) {
  std::array<std::byte, 32> derived{};
  EXPECT_FALSE(software.scrypt({}, {}, ScryptParameters{3, 8, 1}, derived));
  EXPECT_FALSE(
      software.scrypt({}, {}, ScryptParameters{1ULL << 20U, 8, 1}, derived));
}

TEST_F(SoftwareCryptoAgainstOpenSsl, AgreesOnKeyDerivation) {
  for (int round = 0; round < 100; ++round) {
    SCOPED_TRACE(round);
    const auto digest =
        round % 2 == 0 ? HkdfDigest::sha256 : HkdfDigest::sha512;
    const auto input = random_bytes(draw(80));
    const auto salt = random_bytes(draw(80));
    const auto info = random_bytes(draw(80));
    Bytes derived(1 + draw(300));
    Bytes derived_real(derived.size());
    EXPECT_TRUE(software.hkdf(digest, salt, input, info, derived));
    EXPECT_TRUE(openssl.hkdf(digest, salt, input, info, derived_real));
    EXPECT_EQ(derived, derived_real);
  }
}

class SoftwareCryptoKeyDerivationLimits : public SoftwareCryptoAgainstOpenSsl {
 protected:
  static constexpr std::array<std::byte, 22> input{
      std::byte{0x0b}, std::byte{0x0b}, std::byte{0x0b}, std::byte{0x0b},
      std::byte{0x0b}, std::byte{0x0b}, std::byte{0x0b}, std::byte{0x0b},
      std::byte{0x0b}, std::byte{0x0b}, std::byte{0x0b}, std::byte{0x0b},
      std::byte{0x0b}, std::byte{0x0b}, std::byte{0x0b}, std::byte{0x0b},
      std::byte{0x0b}, std::byte{0x0b}, std::byte{0x0b}, std::byte{0x0b},
      std::byte{0x0b}, std::byte{0x0b}};
};

TEST_F(SoftwareCryptoKeyDerivationLimits, ReferenceDerivesTheKnownHead) {
  std::array<std::byte, 8> head{};
  EXPECT_TRUE(openssl.hkdf(HkdfDigest::sha512, {}, input, {}, head));
  const std::array<std::byte, 8> expected{
      std::byte{0xf5}, std::byte{0xfa}, std::byte{0x02}, std::byte{0xb1},
      std::byte{0x82}, std::byte{0x98}, std::byte{0xa7}, std::byte{0x2a}};
  EXPECT_EQ(head, expected);
}

TEST_F(SoftwareCryptoKeyDerivationLimits, BothRefuseAnOverlongOrEmptyOutput) {
  Bytes overlong(hkdf_max_blocks * hmac_sha512_digest_size + 1);
  EXPECT_FALSE(software.hkdf(HkdfDigest::sha512, {}, input, {}, overlong));
  EXPECT_FALSE(openssl.hkdf(HkdfDigest::sha512, {}, input, {}, overlong));
  EXPECT_FALSE(software.hkdf(HkdfDigest::sha256, {}, input, {}, {}));
  EXPECT_FALSE(openssl.hkdf(HkdfDigest::sha256, {}, input, {}, {}));
}

}  // namespace
}  // namespace net_kakusan_test
