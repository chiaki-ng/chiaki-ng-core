#include "src/core/net/protocol/remote_play/ecdh_wire.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;

class EcdhWireKeyPair final : public platform::EcdhKeyPair {
 public:
  explicit EcdhWireKeyPair(std::size_t coordinates_size)
      : coordinates_size_(coordinates_size) {}

  auto public_coordinates(std::span<std::byte> output) const
      -> libchiaking::Expected<void, platform::CryptoError> override {
    ++calls;
    if (fail || output.size() != coordinates_size_)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = static_cast<std::byte>(i);
    return {};
  }

  auto derive_secret(std::span<const std::byte> input,
                     std::span<std::byte> output) const
      -> libchiaking::Expected<void, platform::CryptoError> override {
    ++derive_calls;
    derived_input.assign(input.begin(), input.end());
    derived_size = output.size();
    if (derive_fail)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = static_cast<std::byte>(0x40 + i);
    return {};
  }

  mutable unsigned calls{};
  mutable unsigned derive_calls{};
  mutable std::vector<std::byte> derived_input;
  mutable std::size_t derived_size{};
  bool fail{false};
  bool derive_fail{false};

 private:
  std::size_t coordinates_size_;
};

class EcdhWireCrypto final : public platform::CryptoProvider {
 public:
  auto generate_ecdh(platform::EcdhCurve curve)
      -> libchiaking::Expected<std::unique_ptr<platform::EcdhKeyPair>,
                               platform::CryptoError> override {
    ++generate_calls;
    generated_curve = curve;
    if (generate_fail)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    if (generate_null) return std::unique_ptr<platform::EcdhKeyPair>{};
    return std::unique_ptr<platform::EcdhKeyPair>{
        std::make_unique<EcdhWireKeyPair>(generated_coordinates_size)};
  }

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte> key,
                   std::span<const std::byte> input,
                   std::span<std::byte> digest)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    ++calls;
    last_key.assign(key.begin(), key.end());
    last_input.assign(input.begin(), input.end());
    if (fail)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    EXPECT_EQ(digest.size(), 32U);
    for (std::size_t i = 0; i < digest.size(); ++i)
      digest[i] = static_cast<std::byte>(0xa0 + i);
    return {};
  }

  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  std::vector<std::byte> last_key;
  std::vector<std::byte> last_input;
  unsigned calls{};
  bool fail{false};
  unsigned generate_calls{};
  platform::EcdhCurve generated_curve{platform::EcdhCurve::secp256k1};
  std::size_t generated_coordinates_size{64};
  bool generate_fail{false};
  bool generate_null{false};
};

struct EcdhWireCurve {
  platform::EcdhCurve curve;
  std::size_t coordinates_size;
  std::size_t secret_size;
};

class EcdhWireOnCurve : public ::testing::TestWithParam<EcdhWireCurve> {
 protected:
  static auto signature_pattern() -> std::array<std::byte, 32> {
    std::array<std::byte, 32> signature{};
    for (std::size_t i = 0; i < signature.size(); ++i)
      signature[i] = static_cast<std::byte>(0xa0 + i);
    return signature;
  }

  auto wire() const -> std::vector<std::byte> {
    std::vector<std::byte> result(GetParam().coordinates_size + 1);
    result[0] = std::byte{0x04};
    for (std::size_t i = 0; i < GetParam().coordinates_size; ++i)
      result[i + 1] = static_cast<std::byte>(i);
    return result;
  }

  EcdhWireCrypto crypto;
};

TEST_P(EcdhWireOnCurve, EncodesTheUncompressedPointAndItsSignature) {
  const auto [curve, coordinates_size, secret_size] = GetParam();
  EcdhWireKeyPair key_pair(coordinates_size);
  std::array<std::byte, 16> handshake_key{};
  for (std::size_t i = 0; i < handshake_key.size(); ++i)
    handshake_key[i] = static_cast<std::byte>(i);
  const auto encoded = remote_play::EcdhWireCodec::encode_public_key(
      crypto, key_pair, curve, handshake_key);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded.value().size, coordinates_size + 1);
  EXPECT_EQ(encoded.value().view()[0], std::byte{0x04});
  EXPECT_EQ(key_pair.calls, 1U);
  EXPECT_EQ(crypto.calls, 1U);
  EXPECT_EQ(crypto.last_key,
            std::vector<std::byte>(handshake_key.begin(), handshake_key.end()));
  EXPECT_EQ(crypto.last_input,
            std::vector<std::byte>(encoded.value().view().begin(),
                                   encoded.value().view().end()));
  for (std::size_t i = 0; i < coordinates_size; ++i)
    EXPECT_EQ(encoded.value().view()[i + 1], static_cast<std::byte>(i))
        << "index " << i;
  for (std::size_t i = 0; i < encoded.value().signature.size(); ++i)
    EXPECT_EQ(encoded.value().signature[i], static_cast<std::byte>(0xa0 + i))
        << "index " << i;
  for (std::size_t i = encoded.value().size; i < encoded.value().bytes.size();
       ++i)
    EXPECT_EQ(encoded.value().bytes[i], std::byte{0}) << "index " << i;
}

TEST_P(EcdhWireOnCurve, DerivesTheSecretFromASignedRemoteKey) {
  const auto [curve, coordinates_size, secret_size] = GetParam();
  EcdhWireKeyPair pair(coordinates_size);
  const std::array<std::byte, 16> key{};
  const auto remote = wire();
  const auto signature = signature_pattern();
  const auto result = remote_play::EcdhWireCodec::decode_shared_secret(
      crypto, pair, curve, key, remote, signature);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().size, secret_size);
  EXPECT_EQ(crypto.calls, 1U);
  EXPECT_EQ(crypto.last_input, remote);
  EXPECT_EQ(pair.derive_calls, 1U);
  EXPECT_EQ(pair.derived_size, secret_size);
  EXPECT_EQ(pair.derived_input,
            std::vector<std::byte>(remote.begin() + 1, remote.end()));
  for (std::size_t i = 0; i < secret_size; ++i)
    EXPECT_EQ(result.value().view()[i], static_cast<std::byte>(0x40 + i))
        << "index " << i;
  for (std::size_t i = secret_size; i < result.value().secret.size(); ++i)
    EXPECT_EQ(result.value().secret[i], std::byte{0}) << "index " << i;
}

TEST_P(EcdhWireOnCurve, RefusesAMismatchedSignatureBeforeDeriving) {
  const auto [curve, coordinates_size, secret_size] = GetParam();
  EcdhWireKeyPair pair(coordinates_size);
  const std::array<std::byte, 16> key{};
  const auto remote = wire();
  auto signature = signature_pattern();
  signature[31] ^= std::byte{1};
  const auto mismatch = remote_play::EcdhWireCodec::decode_shared_secret(
      crypto, pair, curve, key, remote, signature);
  ASSERT_FALSE(mismatch);
  EXPECT_EQ(mismatch.error(), remote_play::EcdhWireError::signature_mismatch);
  EXPECT_EQ(pair.derive_calls, 0U);
}

TEST_P(EcdhWireOnCurve, PreparesASignedKeyPairThatKeepsItsPrivateKey) {
  const auto [curve, coordinates_size, secret_size] = GetParam();
  crypto.generated_coordinates_size = coordinates_size;
  const std::array<std::byte, 16> key{};
  auto prepared = remote_play::EcdhWireCodec::prepare(crypto, curve, key);
  ASSERT_TRUE(prepared);
  EXPECT_EQ(crypto.generate_calls, 1U);
  EXPECT_EQ(prepared.value().curve, curve);
  EXPECT_EQ(crypto.generated_curve, curve);
  EXPECT_EQ(crypto.calls, 1U);
  EXPECT_EQ(prepared.value().public_key.size, coordinates_size + 1);
  EXPECT_EQ(prepared.value().public_key.bytes[0], std::byte{0x04});

  auto retained = std::move(prepared.value());
  ASSERT_TRUE(retained.key_pair);
  std::vector<std::byte> remote_coordinates(coordinates_size);
  std::vector<std::byte> secret(secret_size);
  EXPECT_TRUE(retained.key_pair->derive_secret(remote_coordinates, secret));
  EXPECT_EQ(secret[0], std::byte{0x40});
}

INSTANTIATE_TEST_SUITE_P(
    Curves, EcdhWireOnCurve,
    ::testing::Values(EcdhWireCurve{platform::EcdhCurve::secp256k1, 64, 32},
                      EcdhWireCurve{platform::EcdhCurve::secp521r1, 132, 66}),
    [](const ::testing::TestParamInfo<EcdhWireCurve>& tested) {
      return tested.param.curve == platform::EcdhCurve::secp256k1
                 ? std::string{"Secp256k1"}
                 : std::string{"Secp521r1"};
    });

class EcdhWireEncode : public ::testing::Test {
 protected:
  auto encode(platform::EcdhCurve curve, std::span<const std::byte> key) {
    return remote_play::EcdhWireCodec::encode_public_key(crypto, key_pair,
                                                         curve, key);
  }

  EcdhWireCrypto crypto;
  EcdhWireKeyPair key_pair{64};
  const std::array<std::byte, 16> handshake_key{};
};

TEST_F(EcdhWireEncode, RefusesAnUnknownCurveWithoutCallingTheProvider) {
  const auto invalid_curve =
      encode(static_cast<platform::EcdhCurve>(99), handshake_key);
  ASSERT_FALSE(invalid_curve);
  EXPECT_EQ(invalid_curve.error(), remote_play::EcdhWireError::invalid_curve);
  EXPECT_EQ(key_pair.calls, 0U);
  EXPECT_EQ(crypto.calls, 0U);
}

TEST_F(EcdhWireEncode, RefusesAShortHandshakeKeyWithoutCallingTheProvider) {
  const auto invalid_key = encode(platform::EcdhCurve::secp256k1,
                                  std::span(handshake_key).first(15));
  ASSERT_FALSE(invalid_key);
  EXPECT_EQ(invalid_key.error(),
            remote_play::EcdhWireError::invalid_handshake_key);
  EXPECT_EQ(key_pair.calls, 0U);
  EXPECT_EQ(crypto.calls, 0U);
}

TEST_F(EcdhWireEncode, ReportsACoordinatesFailureWithoutSigning) {
  key_pair.fail = true;
  const auto coordinates =
      encode(platform::EcdhCurve::secp256k1, handshake_key);
  ASSERT_FALSE(coordinates);
  EXPECT_EQ(coordinates.error(),
            remote_play::EcdhWireError::coordinates_failure);
  EXPECT_EQ(crypto.calls, 0U);
}

TEST_F(EcdhWireEncode, ReportsASigningFailure) {
  crypto.fail = true;
  const auto signature = encode(platform::EcdhCurve::secp256k1, handshake_key);
  ASSERT_FALSE(signature);
  EXPECT_EQ(signature.error(), remote_play::EcdhWireError::signature_failure);
  EXPECT_EQ(crypto.calls, 1U);
}

class EcdhWireDecode : public ::testing::Test {
 protected:
  void SetUp() override {
    wire[0] = std::byte{0x04};
    for (std::size_t i = 0; i < signature.size(); ++i)
      signature[i] = static_cast<std::byte>(0xa0 + i);
  }

  auto decode(platform::EcdhCurve chosen, std::span<const std::byte> k,
              std::span<const std::byte> w, std::span<const std::byte> s)
      -> libchiaking::Expected<remote_play::EcdhSharedSecret,
                               remote_play::EcdhWireError> {
    return remote_play::EcdhWireCodec::decode_shared_secret(crypto, pair,
                                                            chosen, k, w, s);
  }

  static constexpr auto curve = platform::EcdhCurve::secp256k1;
  EcdhWireCrypto crypto;
  EcdhWireKeyPair pair{64};
  std::array<std::byte, 16> key{};
  std::array<std::byte, 65> wire{};
  std::array<std::byte, 32> signature{};
};

TEST_F(EcdhWireDecode, RefusesMalformedInputsBeforeAnyCrypto) {
  const auto bad_curve =
      decode(static_cast<platform::EcdhCurve>(99), key, wire, signature);
  ASSERT_FALSE(bad_curve);
  EXPECT_EQ(bad_curve.error(), remote_play::EcdhWireError::invalid_curve);
  const auto bad_key = decode(curve, std::span(key).first(15), wire, signature);
  ASSERT_FALSE(bad_key);
  EXPECT_EQ(bad_key.error(), remote_play::EcdhWireError::invalid_handshake_key);
  const auto short_wire =
      decode(curve, key, std::span(wire).first(64), signature);
  ASSERT_FALSE(short_wire);
  EXPECT_EQ(short_wire.error(),
            remote_play::EcdhWireError::invalid_wire_public_key);
  wire[0] = std::byte{0x02};
  const auto compressed = decode(curve, key, wire, signature);
  ASSERT_FALSE(compressed);
  EXPECT_EQ(compressed.error(),
            remote_play::EcdhWireError::invalid_wire_public_key);
  wire[0] = std::byte{0x04};
  const auto short_signature =
      decode(curve, key, wire, std::span(signature).first(31));
  ASSERT_FALSE(short_signature);
  EXPECT_EQ(short_signature.error(),
            remote_play::EcdhWireError::invalid_remote_signature);
  EXPECT_EQ(crypto.calls, 0U);
  EXPECT_EQ(pair.derive_calls, 0U);
}

TEST_F(EcdhWireDecode, ReportsASigningFailureWithoutDeriving) {
  crypto.fail = true;
  const auto failed = decode(curve, key, wire, signature);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), remote_play::EcdhWireError::signature_failure);
  EXPECT_EQ(pair.derive_calls, 0U);
}

TEST_F(EcdhWireDecode, ReportsADerivationFailure) {
  pair.derive_fail = true;
  const auto failed = decode(curve, key, wire, signature);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), remote_play::EcdhWireError::derive_failure);
  EXPECT_EQ(pair.derive_calls, 1U);
}

class EcdhWirePrepare : public ::testing::Test {
 protected:
  auto prepare(std::span<const std::byte> handshake_key) {
    return remote_play::EcdhWireCodec::prepare(
        crypto, platform::EcdhCurve::secp256k1, handshake_key);
  }

  EcdhWireCrypto crypto;
  const std::array<std::byte, 16> key{};
};

TEST_F(EcdhWirePrepare, RefusesBadInputsWithoutGeneratingAKey) {
  const auto invalid_curve = remote_play::EcdhWireCodec::prepare(
      crypto, static_cast<platform::EcdhCurve>(99), key);
  ASSERT_FALSE(invalid_curve);
  EXPECT_EQ(invalid_curve.error(), remote_play::EcdhWireError::invalid_curve);
  const auto invalid_key = prepare(std::span(key).first(15));
  ASSERT_FALSE(invalid_key);
  EXPECT_EQ(invalid_key.error(),
            remote_play::EcdhWireError::invalid_handshake_key);
  const std::array<std::byte, 17> long_key{};
  const auto invalid_long_key = prepare(long_key);
  ASSERT_FALSE(invalid_long_key);
  EXPECT_EQ(invalid_long_key.error(),
            remote_play::EcdhWireError::invalid_handshake_key);
  EXPECT_EQ(crypto.generate_calls, 0U);
  EXPECT_EQ(crypto.calls, 0U);
}

TEST_F(EcdhWirePrepare, ReportsAFailedOrMissingKeyPairAsAGenerationFailure) {
  crypto.generate_fail = true;
  const auto failed = prepare(key);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), remote_play::EcdhWireError::generation_failure);
  crypto.generate_fail = false;
  crypto.generate_null = true;
  const auto null_pair = prepare(key);
  ASSERT_FALSE(null_pair);
  EXPECT_EQ(null_pair.error(), remote_play::EcdhWireError::generation_failure);
  EXPECT_EQ(crypto.calls, 0U);
}

TEST_F(EcdhWirePrepare, ReportsCoordinatesOfTheWrongSize) {
  crypto.generated_coordinates_size = 63;
  const auto invalid_coordinates = prepare(key);
  ASSERT_FALSE(invalid_coordinates);
  EXPECT_EQ(invalid_coordinates.error(),
            remote_play::EcdhWireError::coordinates_failure);
}

TEST_F(EcdhWirePrepare, ReportsASigningFailure) {
  crypto.fail = true;
  const auto failed_signature = prepare(key);
  ASSERT_FALSE(failed_signature);
  EXPECT_EQ(failed_signature.error(),
            remote_play::EcdhWireError::signature_failure);
}

}  // namespace
}  // namespace remote_play_test
