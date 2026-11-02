#include "src/core/utils/text/hex.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/crypto/boringssl/provider.hpp>
#include <libchiaking/platform/crypto/provider.hpp>

#include <gtest/gtest.h>
#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace platform_crypto_test {
namespace {

using libchiaking::platform::CryptoError;
using libchiaking::platform::ecdh_public_coordinates_size;
using libchiaking::platform::ecdh_shared_secret_size;
using libchiaking::platform::EcdhCurve;
using libchiaking::platform::EcdhKeyPair;
using libchiaking::platform::boringssl::BoringSslCryptoProvider;

class BoringSslEcdh : public ::testing::TestWithParam<EcdhCurve> {
 protected:
  static constexpr std::byte marker{0x5a};

  [[nodiscard]] static auto generator(EcdhCurve curve)
      -> std::vector<std::byte> {
    const std::string_view text =
        curve == EcdhCurve::secp256r1
            ? "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
              "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"
            : "00c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b"
              "4d3dbaa14b5e77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2"
              "e5bd66"
              "011839296a789a3bc0045c8a5fb42c7d1bd998f54449579b446817afbd17273"
              "e662c97ee72995ef42640c550b9013fad0761353c7086a272c24088be94769fd"
              "16650";
    std::vector<std::byte> bytes(text.size() / 2);
    if (!libchiaking::text::decode_hex(text, bytes)) bytes.clear();
    return bytes;
  }

  [[nodiscard]] static auto all_marked(std::span<const std::byte> bytes)
      -> bool {
    return std::ranges::all_of(bytes,
                               [](std::byte byte) { return byte == marker; });
  }

  void SetUp() override {
    auto generated_alice = provider.generate_ecdh(curve);
    auto generated_bob = provider.generate_ecdh(curve);
    ASSERT_TRUE(generated_alice);
    ASSERT_TRUE(generated_bob);
    alice = std::move(generated_alice.value());
    bob = std::move(generated_bob.value());
    ASSERT_TRUE(alice->public_coordinates(alice_public));
    ASSERT_TRUE(bob->public_coordinates(bob_public));
  }

  BoringSslCryptoProvider provider;
  const EcdhCurve curve = GetParam();
  const std::size_t coordinates_size = ecdh_public_coordinates_size(curve);
  const std::size_t secret_size = ecdh_shared_secret_size(curve);
  std::unique_ptr<EcdhKeyPair> alice;
  std::unique_ptr<EcdhKeyPair> bob;
  std::vector<std::byte> alice_public =
      std::vector<std::byte>(coordinates_size);
  std::vector<std::byte> bob_public = std::vector<std::byte>(coordinates_size);
};

TEST_P(BoringSslEcdh, BothSidesDeriveTheSameNonzeroSecret) {
  std::vector<std::byte> alice_secret(secret_size);
  std::vector<std::byte> bob_secret(secret_size);
  ASSERT_TRUE(alice->derive_secret(bob_public, alice_secret));
  ASSERT_TRUE(bob->derive_secret(alice_public, bob_secret));
  EXPECT_EQ(alice_secret, bob_secret);
  EXPECT_FALSE(std::ranges::all_of(
      alice_secret, [](std::byte byte) { return byte == std::byte{0}; }));
}

TEST_P(BoringSslEcdh, SecretWithTheGeneratorIsTheOwnPublicX) {
  const auto point = generator(curve);
  ASSERT_EQ(point.size(), coordinates_size);
  std::vector<std::byte> secret(secret_size);
  ASSERT_TRUE(alice->derive_secret(point, secret));
  EXPECT_TRUE(std::equal(secret.begin(), secret.end(), alice_public.begin()));
}

TEST_P(BoringSslEcdh, RefusesBadPeersAndShortSecretsWithoutWriting) {
  std::vector<std::byte> marked(secret_size, marker);
  auto off_curve = bob_public;
  off_curve.back() ^= std::byte{0x01};
  const auto invalid = alice->derive_secret(off_curve, marked);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), CryptoError::invalid_argument);
  const auto short_peer = alice->derive_secret(
      std::span(bob_public).first(coordinates_size - 1), marked);
  ASSERT_FALSE(short_peer);
  EXPECT_EQ(short_peer.error(), CryptoError::invalid_argument);
  const auto short_secret = alice->derive_secret(
      bob_public, std::span(marked).first(secret_size - 1));
  ASSERT_FALSE(short_secret);
  EXPECT_EQ(short_secret.error(), CryptoError::invalid_argument);
  EXPECT_TRUE(all_marked(marked));
}

TEST_P(BoringSslEcdh, RefusesShortCoordinatesWithoutWritingThem) {
  std::vector<std::byte> short_public(coordinates_size - 1, marker);
  const auto rejected = alice->public_coordinates(short_public);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), CryptoError::invalid_argument);
  EXPECT_TRUE(all_marked(short_public));
}

INSTANTIATE_TEST_SUITE_P(Curves, BoringSslEcdh,
                         ::testing::Values(EcdhCurve::secp256r1,
                                           EcdhCurve::secp521r1),
                         [](const ::testing::TestParamInfo<EcdhCurve>& info) {
                           return std::string{
                               magic_enum::enum_name(info.param)};
                         });

TEST(BoringSslEcdhCurves, DoesNotSupportSecp256k1) {
  BoringSslCryptoProvider provider;
  const auto result = provider.generate_ecdh(EcdhCurve::secp256k1);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CryptoError::unsupported_capability);
}

TEST(BoringSslEcdhCurves, RefusesAnUnknownCurve) {
  BoringSslCryptoProvider provider;
  const auto result = provider.generate_ecdh(static_cast<EcdhCurve>(99));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CryptoError::invalid_argument);
}

}  // namespace
}  // namespace platform_crypto_test
