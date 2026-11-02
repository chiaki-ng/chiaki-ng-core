#include "tests/unit/platform/crypto/micro_ecc/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace platform_crypto_test {
namespace {

using libchiaking::platform::CryptoError;
using libchiaking::platform::EcdhCurve;
using libchiaking::platform::EcdhKeyPair;

class MicroEccKeyRejection : public MicroEccKeyPairTest {
 protected:
  void SetUp() override {
    auto generated = generate(curve, random);
    auto generated_other = generate(curve, random);
    ASSERT_TRUE(generated);
    ASSERT_TRUE(generated_other);
    pair = std::move(generated.value());
    auto other = std::move(generated_other.value());
    ASSERT_TRUE(other->public_coordinates(other_coordinates));
  }

  std::unique_ptr<EcdhKeyPair> pair;
  std::vector<std::byte> other_coordinates =
      std::vector<std::byte>(coordinates_size);
  std::vector<std::byte> marked = std::vector<std::byte>(secret_size, marker);
};

TEST_P(MicroEccKeyRejection, RefusesShortCoordinatesWithoutWritingThem) {
  std::vector<std::byte> short_coordinates(coordinates_size - 1, marker);
  const auto result = pair->public_coordinates(short_coordinates);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CryptoError::invalid_argument);
  EXPECT_TRUE(all_marked(short_coordinates));
}

TEST_P(MicroEccKeyRejection, RefusesBadPeersAndShortSecretsWithoutWriting) {
  const auto short_peer = pair->derive_secret(
      std::span(other_coordinates).first(coordinates_size - 1), marked);
  ASSERT_FALSE(short_peer);
  EXPECT_EQ(short_peer.error(), CryptoError::invalid_argument);
  const auto short_secret = pair->derive_secret(
      other_coordinates, std::span(marked).first(secret_size - 1));
  ASSERT_FALSE(short_secret);
  EXPECT_EQ(short_secret.error(), CryptoError::invalid_argument);
  auto off_curve = other_coordinates;
  off_curve.back() ^= std::byte{0x01};
  const auto invalid = pair->derive_secret(off_curve, marked);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), CryptoError::invalid_argument);
  EXPECT_TRUE(all_marked(marked));
}

TEST_P(MicroEccKeyRejection, FailsToGenerateWhenTheRandomSourceFails) {
  ScriptedRandom failing{true};
  const auto result = generate(curve, failing);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CryptoError::backend_failure);
}

TEST_P(MicroEccKeyRejection, FailsToGenerateFromOnlyZeroBytes) {
  ScriptedRandom zeros{false};
  const auto result = generate(curve, zeros);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CryptoError::backend_failure);
}

INSTANTIATE_TEST_SUITE_P(Curves, MicroEccKeyRejection,
                         ::testing::Values(EcdhCurve::secp256k1,
                                           EcdhCurve::secp521r1,
                                           EcdhCurve::secp256r1),
                         MicroEccKeyPairTest::curve_name);

class MicroEccP256Peer : public MicroEccKeyPairTest {};

TEST_P(MicroEccP256Peer, RefusesAPeerWithAZeroXWithoutWriting) {
  const auto peer = from_hex(
      "0000000000000000000000000000000000000000000000000000000000000000"
      "66485c780e2f83d72433bd5d84a06bb6541c2af31dae871728bf856a174f93f4");
  auto generated = generate(curve, random);
  ASSERT_TRUE(generated);
  std::vector<std::byte> marked(
      libchiaking::platform::secp256r1_shared_secret_size, marker);
  const auto derived = generated.value()->derive_secret(peer, marked);
  ASSERT_FALSE(derived);
  EXPECT_EQ(derived.error(), CryptoError::backend_failure);
  EXPECT_TRUE(all_marked(marked));
}

INSTANTIATE_TEST_SUITE_P(Secp256r1, MicroEccP256Peer,
                         ::testing::Values(EcdhCurve::secp256r1),
                         MicroEccKeyPairTest::curve_name);

}  // namespace
}  // namespace platform_crypto_test
