#include "tests/unit/platform/crypto/micro_ecc/rig.hpp"

#include <libchiaking/platform/crypto/openssl/provider.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace platform_crypto_test {
namespace {

using libchiaking::platform::EcdhCurve;

class MicroEccKeyAgreement : public MicroEccKeyPairTest {
 protected:
  static constexpr int rounds = 20;

  [[nodiscard]] static auto generator(EcdhCurve curve)
      -> std::vector<std::byte> {
    switch (curve) {
      case EcdhCurve::secp256k1:
        return from_hex(
            "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
            "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8");
      case EcdhCurve::secp256r1:
        return from_hex(
            "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
            "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
      case EcdhCurve::secp521r1:
        break;
    }
    return from_hex(
        "00c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b"
        "4d3dbaa14b5e77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2"
        "e5bd66"
        "011839296a789a3bc0045c8a5fb42c7d1bd998f54449579b446817afbd17273"
        "e662c97ee72995ef42640c550b9013fad0761353c7086a272c24088be94769fd"
        "16650");
  }

  libchiaking::platform::openssl::OpenSslCryptoProvider openssl;
};

TEST_P(MicroEccKeyAgreement, AgreesWithOpenSslOnANonzeroSecret) {
  for (int round = 0; round < rounds; ++round) {
    auto local = generate(curve, random);
    auto peer = openssl.generate_ecdh(curve);
    ASSERT_TRUE(local) << "round " << round;
    ASSERT_TRUE(peer) << "round " << round;
    std::vector<std::byte> local_coordinates(coordinates_size);
    std::vector<std::byte> peer_coordinates(coordinates_size);
    ASSERT_TRUE(local.value()->public_coordinates(local_coordinates));
    ASSERT_TRUE(peer.value()->public_coordinates(peer_coordinates));
    std::vector<std::byte> local_secret(secret_size);
    std::vector<std::byte> peer_secret(secret_size);
    ASSERT_TRUE(local.value()->derive_secret(peer_coordinates, local_secret))
        << "round " << round;
    ASSERT_TRUE(peer.value()->derive_secret(local_coordinates, peer_secret))
        << "round " << round;
    EXPECT_EQ(local_secret, peer_secret) << "round " << round;
    EXPECT_FALSE(std::ranges::all_of(
        local_secret, [](std::byte byte) { return byte == std::byte{0}; }))
        << "round " << round;
  }
}

TEST_P(MicroEccKeyAgreement, SecretWithTheGeneratorIsTheOwnPublicX) {
  const auto point = generator(curve);
  ASSERT_EQ(point.size(), coordinates_size);
  auto pair = generate(curve, random);
  ASSERT_TRUE(pair);
  std::vector<std::byte> coordinates(coordinates_size);
  ASSERT_TRUE(pair.value()->public_coordinates(coordinates));
  std::vector<std::byte> secret(secret_size);
  ASSERT_TRUE(pair.value()->derive_secret(point, secret));
  EXPECT_TRUE(std::equal(secret.begin(), secret.end(), coordinates.begin()));
}

INSTANTIATE_TEST_SUITE_P(Curves, MicroEccKeyAgreement,
                         ::testing::Values(EcdhCurve::secp256k1,
                                           EcdhCurve::secp521r1,
                                           EcdhCurve::secp256r1),
                         MicroEccKeyPairTest::curve_name);

}  // namespace
}  // namespace platform_crypto_test
