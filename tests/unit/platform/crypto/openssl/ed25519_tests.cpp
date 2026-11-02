#include "tests/unit/platform/crypto/openssl/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace platform_crypto_test {
namespace {

using libchiaking::platform::ed25519_public_key_size;
using libchiaking::platform::ed25519_signature_size;

class OpenSslEd25519 : public OpenSslCryptoTest {
 protected:
  void SetUp() override {
    ASSERT_TRUE(crypto.ed25519_public_key(seed, public_key));
    ASSERT_TRUE(crypto.ed25519_sign(seed, {}, signature));
  }

  const std::vector<std::byte> seed =
      unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  std::array<std::byte, ed25519_public_key_size> public_key{};
  std::array<std::byte, ed25519_signature_size> signature{};
};

TEST_F(OpenSslEd25519, DerivesTheRfc8032FirstPublicKey) {
  EXPECT_EQ(hex(public_key),
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
}

TEST_F(OpenSslEd25519, SignsTheEmptyMessageAsRfc8032Expects) {
  EXPECT_EQ(hex(signature),
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
            "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
}

TEST_F(OpenSslEd25519, VerifiesItsOwnSignature) {
  const auto verified = crypto.ed25519_verify(public_key, {}, signature);
  ASSERT_TRUE(verified);
  EXPECT_TRUE(verified.value());
}

TEST_F(OpenSslEd25519, RejectsTheSignatureOverAnotherMessage) {
  const auto verified =
      crypto.ed25519_verify(public_key, text_bytes("x"), signature);
  ASSERT_TRUE(verified);
  EXPECT_FALSE(verified.value());
}

TEST_F(OpenSslEd25519, RejectsAnAlteredSignature) {
  auto altered = signature;
  altered[0] ^= std::byte{1};
  const auto verified = crypto.ed25519_verify(public_key, {}, altered);
  ASSERT_TRUE(verified);
  EXPECT_FALSE(verified.value());
}

TEST_F(OpenSslEd25519, RejectsTheSignatureUnderAnotherKey) {
  auto other_key = public_key;
  other_key[0] ^= std::byte{1};
  const auto verified = crypto.ed25519_verify(other_key, {}, signature);
  ASSERT_TRUE(verified);
  EXPECT_FALSE(verified.value());
}

TEST_F(OpenSslEd25519, RefusesShortKeysAndSeeds) {
  EXPECT_FALSE(crypto.ed25519_verify(
      std::span<const std::byte>{public_key}.first(31), {}, signature));
  EXPECT_FALSE(crypto.ed25519_sign(std::span<const std::byte>{seed}.first(31),
                                   {}, signature));
  EXPECT_FALSE(crypto.ed25519_public_key(
      std::span<const std::byte>{seed}.first(31), public_key));
}

}  // namespace
}  // namespace platform_crypto_test
