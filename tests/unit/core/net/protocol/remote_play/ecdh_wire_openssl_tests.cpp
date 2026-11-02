#include "libchiaking/platform/crypto/openssl/provider.hpp"
#include "src/core/net/protocol/remote_play/ecdh_wire.hpp"
#include "src/core/net/protocol/remote_play/stream/active_handshake.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

class OpenSslEcdhFixedKeyPair final : public platform::EcdhKeyPair {
 public:
  explicit OpenSslEcdhFixedKeyPair(std::size_t coordinate_size)
      : coordinate_size_(coordinate_size) {}

  auto public_coordinates(std::span<std::byte> output) const
      -> libchiaking::Expected<void, platform::CryptoError> override {
    if (output.size() != coordinate_size_)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = static_cast<std::byte>(i);
    return {};
  }

  auto derive_secret(std::span<const std::byte>, std::span<std::byte>) const
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(
        platform::CryptoError::unsupported_capability);
  }

 private:
  std::size_t coordinate_size_;
};

class OpenSslEcdhWire : public ::testing::Test {
 protected:
  void SetUp() override {
    for (std::size_t i = 0; i < key.size(); ++i)
      key[i] = static_cast<std::byte>(i);
  }

  static auto hex_nibble(char c) -> unsigned {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    return static_cast<unsigned>(c - 'a' + 10);
  }

  void expect_signature(platform::EcdhCurve curve, std::size_t coordinates_size,
                        std::string_view expected_hex) {
    OpenSslEcdhFixedKeyPair key_pair(coordinates_size);
    const auto result = remote_play::EcdhWireCodec::encode_public_key(
        crypto, key_pair, curve, key);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().size, coordinates_size + 1);
    EXPECT_EQ(result.value().bytes[0], std::byte{0x04});
    ASSERT_EQ(expected_hex.size(), 64U);
    for (std::size_t i = 0; i < result.value().signature.size(); ++i) {
      const auto expected =
          static_cast<std::byte>((hex_nibble(expected_hex[i * 2]) << 4U) |
                                 hex_nibble(expected_hex[i * 2 + 1]));
      EXPECT_EQ(result.value().signature[i], expected) << "index " << i;
    }
  }

  platform::openssl::OpenSslCryptoProvider crypto;
  std::array<std::byte, 16> key{};
};

// Independent Python hashlib HMAC-SHA256 vectors for key 00..0f and
// input 04||00..3f or 04||00..83, respectively.
TEST_F(OpenSslEcdhWire, SignsASecp256k1KeyLikeAnIndependentHmac) {
  expect_signature(
      platform::EcdhCurve::secp256k1, 64,
      "27e1fdef3516afb2ae3722aa2c4617564eba4f983e32157c1d76f90a7787c040");
}

TEST_F(OpenSslEcdhWire, SignsASecp521r1KeyLikeAnIndependentHmac) {
  expect_signature(
      platform::EcdhCurve::secp521r1, 132,
      "ec368aa02287277ad9ee27b70f49fb4984da3f8c7d80ddc1aab2bd9154d9d890");
}

class OpenSslEcdhExchange
    : public OpenSslEcdhWire,
      public ::testing::WithParamInterface<platform::EcdhCurve> {
 protected:
  void SetUp() override {
    OpenSslEcdhWire::SetUp();
    auto alice_prepared =
        remote_play::EcdhWireCodec::prepare(crypto, GetParam(), key);
    auto bob_prepared =
        remote_play::EcdhWireCodec::prepare(crypto, GetParam(), key);
    ASSERT_TRUE(alice_prepared);
    ASSERT_TRUE(bob_prepared);
    alice.emplace(std::move(alice_prepared.value()));
    bob.emplace(std::move(bob_prepared.value()));
  }

  std::optional<remote_play::PreparedEcdh> alice;
  std::optional<remote_play::PreparedEcdh> bob;
};

TEST_P(OpenSslEcdhExchange, PublishesTheKeyPairsCoordinates) {
  std::array<std::byte, 132> alice_coordinates{};
  const auto coordinates_size =
      platform::ecdh_public_coordinates_size(GetParam());
  ASSERT_TRUE(alice->key_pair->public_coordinates(
      {alice_coordinates.data(), coordinates_size}));
  EXPECT_TRUE(std::equal(
      alice_coordinates.begin(),
      alice_coordinates.begin() + static_cast<std::ptrdiff_t>(coordinates_size),
      alice->public_key.view().begin() + 1));
}

TEST_P(OpenSslEcdhExchange, BothSidesDeriveTheSameSecret) {
  const auto curve = GetParam();
  const auto alice_secret = remote_play::EcdhWireCodec::decode_shared_secret(
      crypto, *alice->key_pair, curve, key, bob->public_key.view(),
      bob->public_key.signature);
  const auto bob_secret = remote_play::EcdhWireCodec::decode_shared_secret(
      crypto, *bob->key_pair, curve, key, alice->public_key.view(),
      alice->public_key.signature);
  ASSERT_TRUE(alice_secret);
  ASSERT_TRUE(bob_secret);
  EXPECT_EQ(alice_secret.value().size,
            platform::ecdh_shared_secret_size(curve));
  EXPECT_TRUE(std::equal(alice_secret.value().view().begin(),
                         alice_secret.value().view().end(),
                         bob_secret.value().view().begin()));
}

TEST_P(OpenSslEcdhExchange, ABangCarryingThePeerKeyYieldsTheSameSecret) {
  const auto curve = GetParam();
  const auto bob_secret = remote_play::EcdhWireCodec::decode_shared_secret(
      crypto, *bob->key_pair, curve, key, alice->public_key.view(),
      alice->public_key.signature);
  ASSERT_TRUE(bob_secret);
  const auto profile = takion::resolve_takion_wire_profile(
      curve == platform::EcdhCurve::secp256k1 ? 12 : 20);
  ASSERT_TRUE(profile);
  takion::Bang bang;
  bang.version_accepted = true;
  bang.encrypted_key_accepted = true;
  bang.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{"session-id"};
  bang.ecdh_pub_key = std::vector<std::byte>(bob->public_key.view().begin(),
                                             bob->public_key.view().end());
  bang.ecdh_sig = std::vector<std::byte>(bob->public_key.signature.begin(),
                                         bob->public_key.signature.end());
  const auto active_secret = remote_play::ActiveHandshakeMessages::verify_bang(
      crypto, *alice, *profile, key, "session-id", bang);
  ASSERT_TRUE(active_secret);
  EXPECT_TRUE(std::equal(active_secret.value().view().begin(),
                         active_secret.value().view().end(),
                         bob_secret.value().view().begin()));
}

TEST_P(OpenSslEcdhExchange, RefusesATamperedSignature) {
  auto bad_signature = bob->public_key.signature;
  bad_signature[0] ^= std::byte{1};
  const auto rejected = remote_play::EcdhWireCodec::decode_shared_secret(
      crypto, *alice->key_pair, GetParam(), key, bob->public_key.view(),
      bad_signature);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), remote_play::EcdhWireError::signature_mismatch);
}

TEST_P(OpenSslEcdhExchange, RefusesASignedPointOffTheCurve) {
  auto invalid_point = bob->public_key;
  for (std::size_t i = 1; i < invalid_point.size; ++i)
    invalid_point.bytes[i] = std::byte{0};
  ASSERT_TRUE(
      crypto.hmac_sha256(key, invalid_point.view(), invalid_point.signature));
  const auto invalid = remote_play::EcdhWireCodec::decode_shared_secret(
      crypto, *alice->key_pair, GetParam(), key, invalid_point.view(),
      invalid_point.signature);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), remote_play::EcdhWireError::derive_failure);
}

INSTANTIATE_TEST_SUITE_P(Curves, OpenSslEcdhExchange,
                         ::testing::Values(platform::EcdhCurve::secp256k1,
                                           platform::EcdhCurve::secp521r1),
                         [](const auto& tested) {
                           return tested.param == platform::EcdhCurve::secp256k1
                                      ? std::string{"Secp256k1"}
                                      : std::string{"Secp521r1"};
                         });

}  // namespace
}  // namespace remote_play_test
