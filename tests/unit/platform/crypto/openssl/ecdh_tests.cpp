#include "src/core/utils/text/hex.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/crypto/openssl/provider.hpp>
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
using libchiaking::platform::openssl::OpenSslCryptoProvider;

class OpenSslEcdh : public ::testing::TestWithParam<EcdhCurve> {
 protected:
  static constexpr std::byte marker{0x5a};

  [[nodiscard]] static auto generator(EcdhCurve curve)
      -> std::vector<std::byte> {
    std::string_view text;
    switch (curve) {
      case EcdhCurve::secp256k1:
        text =
            "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
            "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8";
        break;
      case EcdhCurve::secp256r1:
        text =
            "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
            "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";
        break;
      case EcdhCurve::secp521r1:
        text =
            "00c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b"
            "4d3dbaa14b5e77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2e5"
            "bd66"
            "011839296a789a3bc0045c8a5fb42c7d1bd998f54449579b446817afbd17273"
            "e662c97ee72995ef42640c550b9013fad0761353c7086a272c24088be94769fd1"
            "6650";
        break;
    }
    std::vector<std::byte> bytes(text.size() / 2);
    if (!libchiaking::text::decode_hex(text, bytes)) bytes.clear();
    return bytes;
  }

  [[nodiscard]] static auto all_marked(std::span<const std::byte> bytes)
      -> bool {
    return std::ranges::all_of(bytes,
                               [](std::byte byte) { return byte == marker; });
  }

  [[nodiscard]] static auto any_nonzero(std::span<const std::byte> bytes)
      -> bool {
    return std::ranges::any_of(
        bytes, [](std::byte byte) { return byte != std::byte{0}; });
  }

  void SetUp() override {
    auto generated_a = provider.generate_ecdh(curve);
    auto generated_b = provider.generate_ecdh(curve);
    ASSERT_TRUE(generated_a);
    ASSERT_TRUE(generated_b);
    a = std::move(generated_a.value());
    b = std::move(generated_b.value());
    ASSERT_TRUE(a->public_coordinates(a_coordinates));
    ASSERT_TRUE(b->public_coordinates(b_coordinates));
  }

  OpenSslCryptoProvider provider;
  const EcdhCurve curve = GetParam();
  const std::size_t coordinates_size = ecdh_public_coordinates_size(curve);
  const std::size_t secret_size = ecdh_shared_secret_size(curve);
  std::unique_ptr<libchiaking::platform::EcdhKeyPair> a;
  std::unique_ptr<libchiaking::platform::EcdhKeyPair> b;
  std::vector<std::byte> a_coordinates =
      std::vector<std::byte>(coordinates_size);
  std::vector<std::byte> b_coordinates =
      std::vector<std::byte>(coordinates_size);
};

TEST_P(OpenSslEcdh, PublishesNonzeroCoordinates) {
  EXPECT_TRUE(any_nonzero(a_coordinates));
}

TEST_P(OpenSslEcdh, BothSidesDeriveTheSameNonzeroSecret) {
  std::vector<std::byte> a_secret(secret_size);
  std::vector<std::byte> b_secret(secret_size);
  ASSERT_TRUE(a->derive_secret(b_coordinates, a_secret));
  ASSERT_TRUE(b->derive_secret(a_coordinates, b_secret));
  EXPECT_EQ(a_secret, b_secret);
  EXPECT_TRUE(any_nonzero(a_secret));
}

TEST_P(OpenSslEcdh, SecretWithTheGeneratorIsTheOwnPublicX) {
  const auto point = generator(curve);
  ASSERT_EQ(point.size(), coordinates_size);
  std::vector<std::byte> secret(secret_size);
  ASSERT_TRUE(a->derive_secret(point, secret));
  EXPECT_TRUE(std::equal(secret.begin(), secret.end(), a_coordinates.begin()));
}

TEST_P(OpenSslEcdh, RefusesShortCoordinatesWithoutWritingThem) {
  std::vector<std::byte> short_coordinates(coordinates_size - 1, marker);
  const auto result = a->public_coordinates(short_coordinates);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CryptoError::invalid_argument);
  EXPECT_TRUE(all_marked(short_coordinates));
}

TEST_P(OpenSslEcdh, RefusesBadPeersAndShortSecretsWithoutWriting) {
  std::vector<std::byte> marked_secret(secret_size, marker);
  const auto short_peer = a->derive_secret(
      std::span(b_coordinates).first(coordinates_size - 1), marked_secret);
  ASSERT_FALSE(short_peer);
  EXPECT_EQ(short_peer.error(), CryptoError::invalid_argument);
  const auto short_secret = a->derive_secret(
      b_coordinates, std::span(marked_secret).first(secret_size - 1));
  ASSERT_FALSE(short_secret);
  EXPECT_EQ(short_secret.error(), CryptoError::invalid_argument);
  const std::vector<std::byte> invalid_point(coordinates_size);
  const auto invalid = a->derive_secret(invalid_point, marked_secret);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), CryptoError::invalid_argument);
  EXPECT_TRUE(all_marked(marked_secret));
}

INSTANTIATE_TEST_SUITE_P(Curves, OpenSslEcdh,
                         ::testing::Values(EcdhCurve::secp256k1,
                                           EcdhCurve::secp521r1,
                                           EcdhCurve::secp256r1),
                         [](const ::testing::TestParamInfo<EcdhCurve>& tested) {
                           return std::string{
                               magic_enum::enum_name(tested.param)};
                         });

TEST(OpenSslEcdhCurves, RefusesAnUnknownCurve) {
  OpenSslCryptoProvider provider;
  const auto result = provider.generate_ecdh(static_cast<EcdhCurve>(99));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CryptoError::invalid_argument);
}

}  // namespace
}  // namespace platform_crypto_test
