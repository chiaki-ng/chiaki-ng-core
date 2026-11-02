#include "tests/unit/platform/crypto/openssl/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace platform_crypto_test {
namespace {

using libchiaking::platform::p256_point_size;

class OpenSslP256 : public OpenSslCryptoTest {
 protected:
  static constexpr std::string_view generator =
      "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
      "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";
  static constexpr std::string_view doubled =
      "047cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978"
      "07775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1";
  static constexpr std::string_view tripled =
      "045ecbe4d1a6330a44c8f7ef951d4bf165e6c6b721efada985fb41661bc6e7fd6c"
      "8734640c4998ff7e374b06ce1a64a2ecd82ab036384fb83d9a79b127a27d5032";
  static constexpr std::string_view negated =
      "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
      "b01cbd1c01e58065711814b583f061e9d431cca994cea1313449bf97c840ae0a";
  static constexpr std::string_view order =
      "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551";

  std::array<std::byte, p256_point_size> point{};
};

TEST_F(OpenSslP256, BaseMultiplyByOneGivesTheGenerator) {
  ASSERT_TRUE(crypto.p256_base_multiply(scalar(1), point));
  EXPECT_EQ(hex(point), generator);
}

TEST_F(OpenSslP256, BaseMultiplyByTwoGivesTheDoubledGenerator) {
  ASSERT_TRUE(crypto.p256_base_multiply(scalar(2), point));
  EXPECT_EQ(hex(point), doubled);
}

TEST_F(OpenSslP256, MultiplyingTheGeneratorByThreeGivesItsTriple) {
  ASSERT_TRUE(crypto.p256_multiply(scalar(3), unhex(generator), point));
  EXPECT_EQ(hex(point), tripled);
}

TEST_F(OpenSslP256, AddingTheGeneratorToItsDoubleGivesItsTriple) {
  ASSERT_TRUE(crypto.p256_add(unhex(generator), unhex(doubled), point));
  EXPECT_EQ(hex(point), tripled);
}

TEST_F(OpenSslP256, AddingTheGeneratorToItselfDoublesIt) {
  ASSERT_TRUE(crypto.p256_add(unhex(generator), unhex(generator), point));
  EXPECT_EQ(hex(point), doubled);
}

TEST_F(OpenSslP256, BaseMultiplyByOrderMinusOneGivesTheNegatedGenerator) {
  auto last = unhex(order);
  last.back() = std::byte{0x50};
  ASSERT_TRUE(crypto.p256_base_multiply(last, point));
  EXPECT_EQ(hex(point), negated);
}

TEST_F(OpenSslP256, RefusesASumAtInfinity) {
  EXPECT_FALSE(crypto.p256_add(unhex(generator), unhex(negated), point));
}

TEST_F(OpenSslP256, RefusesAZeroOrFullOrderScalar) {
  EXPECT_FALSE(crypto.p256_base_multiply(scalar(0), point));
  EXPECT_FALSE(crypto.p256_base_multiply(unhex(order), point));
  EXPECT_FALSE(crypto.p256_multiply(scalar(0), unhex(generator), point));
}

TEST_F(OpenSslP256, RefusesAPointOffTheCurve) {
  auto off_curve = unhex(generator);
  off_curve.back() ^= std::byte{1};
  EXPECT_FALSE(crypto.p256_multiply(scalar(2), off_curve, point));
  EXPECT_FALSE(crypto.p256_add(off_curve, unhex(generator), point));
  EXPECT_FALSE(crypto.p256_add(unhex(generator), off_curve, point));
}

TEST_F(OpenSslP256, RefusesCompressedHybridAndZeroEncodings) {
  auto compressed = unhex(generator);
  compressed.resize(33);
  compressed[0] = std::byte{3};
  EXPECT_FALSE(crypto.p256_multiply(scalar(2), compressed, point));
  auto hybrid = unhex(generator);
  hybrid[0] = std::byte{7};
  EXPECT_FALSE(crypto.p256_multiply(scalar(2), hybrid, point));
  std::vector<std::byte> zero(p256_point_size);
  zero[0] = std::byte{4};
  EXPECT_FALSE(crypto.p256_multiply(scalar(2), zero, point));
}

TEST_F(OpenSslP256, RefusesAShortPointOrScalar) {
  std::array<std::byte, 64> short_point{};
  EXPECT_FALSE(crypto.p256_base_multiply(scalar(1), short_point));
  const auto one = scalar(1);
  EXPECT_FALSE(crypto.p256_base_multiply(
      std::span<const std::byte>{one}.first(31), point));
}

}  // namespace
}  // namespace platform_crypto_test
