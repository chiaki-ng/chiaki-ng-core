#include "tests/unit/core/net/protocol/remote_play/stream/active_handshake_rig.hpp"
#include "tests/support/crypto/refusing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace remote_play_test {
namespace {

class ActiveCryptoInstall : public ::testing::Test {
 protected:
  static void expect_exact_install(std::uint32_t version,
                                   std::size_t secret_size) {
    const auto profile = takion::resolve_takion_wire_profile(version);
    ASSERT_TRUE(profile);
    test_support::RefusingTakionCrypto provider;
    std::array<std::byte, 16> key{};
    for (std::size_t i = 0; i < key.size(); ++i)
      key[i] = static_cast<std::byte>(0x80 + i);
    const auto command =
        remote_play::ActiveHandshakeMessages::make_crypto_install(
            provider, *profile, key,
            ActiveHandshakeRig::shared_secret(secret_size));
    ASSERT_TRUE(command);
    const auto& value = command.value();
    EXPECT_EQ(value.provider, &provider);
    EXPECT_EQ(value.local_key_position, 0U);
    EXPECT_EQ(value.remote_key_position, 0U);
    EXPECT_TRUE(value.local.has_gkcrypt_material());
    EXPECT_TRUE(value.remote.has_gkcrypt_material());
    EXPECT_EQ(value.local.gkcrypt_index(), 2U);
    EXPECT_EQ(value.remote.gkcrypt_index(), 3U);
    EXPECT_TRUE(std::equal(value.local.handshake_key().begin(),
                           value.local.handshake_key().end(), key.begin()));
    EXPECT_TRUE(std::equal(value.remote.handshake_key().begin(),
                           value.remote.handshake_key().end(), key.begin()));
    ASSERT_EQ(value.local.ecdh_secret().size(), secret_size);
    ASSERT_EQ(value.remote.ecdh_secret().size(), secret_size);
    for (std::size_t i = 0; i < secret_size; ++i) {
      EXPECT_EQ(value.local.ecdh_secret()[i], static_cast<std::byte>(i + 1))
          << "index " << i;
      EXPECT_EQ(value.remote.ecdh_secret()[i], static_cast<std::byte>(i + 1))
          << "index " << i;
    }
  }
};

TEST_F(ActiveCryptoInstall, KeysBothDirectionsForVersion12) {
  expect_exact_install(12, 32);
}

TEST_F(ActiveCryptoInstall, KeysBothDirectionsForVersion20) {
  expect_exact_install(20, 66);
}

TEST_F(ActiveCryptoInstall, RejectsAShortKeyOrASecretOfTheWrongSize) {
  const auto profile = takion::resolve_takion_wire_profile(20);
  ASSERT_TRUE(profile);
  test_support::RefusingTakionCrypto provider;
  const std::array<std::byte, 16> key{};
  EXPECT_EQ(remote_play::ActiveHandshakeMessages::make_crypto_install(
                provider, *profile, std::span(key).first(15),
                ActiveHandshakeRig::shared_secret(66))
                .error(),
            remote_play::ActiveCryptoError::invalid_handshake_key);
  EXPECT_EQ(remote_play::ActiveHandshakeMessages::make_crypto_install(
                provider, *profile, key, ActiveHandshakeRig::shared_secret(32))
                .error(),
            remote_play::ActiveCryptoError::invalid_secret);
  EXPECT_EQ(remote_play::ActiveHandshakeMessages::make_crypto_install(
                provider, *profile, key, ActiveHandshakeRig::shared_secret(67))
                .error(),
            remote_play::ActiveCryptoError::invalid_secret);
}

}  // namespace
}  // namespace remote_play_test
