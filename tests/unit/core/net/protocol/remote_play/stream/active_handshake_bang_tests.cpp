#include "tests/unit/core/net/protocol/remote_play/stream/active_handshake_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace remote_play_test {
namespace {

class ActiveBangVerification : public ::testing::Test {
 protected:
  static void expect_derivation_after_signature(std::uint32_t version,
                                                platform::EcdhCurve curve,
                                                std::size_t coordinates_size,
                                                std::size_t secret_size) {
    const auto profile = takion::resolve_takion_wire_profile(version);
    ASSERT_TRUE(profile);
    HandshakeCrypto crypto;
    auto local = ActiveHandshakeRig::without_public_key(curve);
    const auto& pair = ActiveHandshakeRig::key_pair_of(local);
    const std::array<std::byte, 16> key{};
    auto remote = ActiveHandshakeRig::bang(coordinates_size);
    remote.server_version = 1;
    const auto result = remote_play::ActiveHandshakeMessages::verify_bang(
        crypto, local, *profile, key, ActiveHandshakeRig::sent_key, remote);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().size, secret_size);
    EXPECT_EQ(crypto.hmac_calls, 1U);
    EXPECT_EQ(crypto.last_input, *remote.ecdh_pub_key);
    EXPECT_EQ(crypto.last_key, std::vector<std::byte>(key.begin(), key.end()));
    EXPECT_EQ(pair.derive_calls, 1U);
    EXPECT_EQ(pair.last_coordinates,
              std::vector<std::byte>(remote.ecdh_pub_key->begin() + 1,
                                     remote.ecdh_pub_key->end()));
    for (std::size_t i = 0; i < secret_size; ++i)
      EXPECT_EQ(result.value().view()[i], static_cast<std::byte>(i + 1))
          << "index " << i;

    remote.ecdh_sig->at(31) ^= std::byte{1};
    const auto mismatched = remote_play::ActiveHandshakeMessages::verify_bang(
        crypto, local, *profile, key, ActiveHandshakeRig::sent_key, remote);
    ASSERT_FALSE(mismatched);
    EXPECT_EQ(mismatched.error(),
              remote_play::ActiveBangError::invalid_remote_signature);
    EXPECT_EQ(pair.derive_calls, 1U);
  }

  static auto verify(HandshakeCrypto& crypto,
                     const remote_play::PreparedEcdh& local,
                     const takion::WireProfile& profile,
                     std::span<const std::byte> key,
                     std::optional<std::string_view> sent,
                     const takion::Bang& remote) {
    return remote_play::ActiveHandshakeMessages::verify_bang(
        crypto, local, profile, key, sent, remote);
  }
};

TEST_F(ActiveBangVerification,
       Version12DerivesTheSecretOnlyAfterTheSignatureChecks) {
  expect_derivation_after_signature(12, platform::EcdhCurve::secp256k1, 64, 32);
}

TEST_F(ActiveBangVerification,
       Version20DerivesTheSecretOnlyAfterTheSignatureChecks) {
  expect_derivation_after_signature(20, platform::EcdhCurve::secp521r1, 132,
                                    66);
}

TEST_F(ActiveBangVerification, NeverDerivesFromAMalformedBang) {
  const auto profile = takion::resolve_takion_wire_profile(12);
  ASSERT_TRUE(profile);
  HandshakeCrypto crypto;
  auto local =
      ActiveHandshakeRig::without_public_key(platform::EcdhCurve::secp256k1);
  auto& pair = ActiveHandshakeRig::key_pair_of(local);
  const std::array<std::byte, 16> key{};
  const auto sent = ActiveHandshakeRig::sent_key;
  auto remote = ActiveHandshakeRig::bang(64);
  remote.version_accepted = false;
  EXPECT_EQ(verify(crypto, local, *profile, key, sent, remote).error(),
            remote_play::ActiveBangError::version_rejected);
  remote.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{"other"};
  EXPECT_EQ(verify(crypto, local, *profile, key, sent, remote).error(),
            remote_play::ActiveBangError::version_rejected);
  remote.version_accepted = true;
  remote.encrypted_key_accepted = false;
  EXPECT_EQ(verify(crypto, local, *profile, key, sent, remote).error(),
            remote_play::ActiveBangError::encrypted_key_rejected);
  remote.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{std::string{sent}};
  remote.encrypted_key_accepted = true;
  for (const std::string_view other :
       {std::string_view{"session-iD"}, std::string_view{"session-i"},
        std::string_view{"session-id!"}, std::string_view{"session-id\0x", 12},
        std::string_view{}}) {
    remote.session_key = libchiaking::net::protocol::takion::SharedSecretText{
        std::string{other}};
    EXPECT_EQ(verify(crypto, local, *profile, key, sent, remote).error(),
              remote_play::ActiveBangError::session_key_mismatch)
        << other.size();
  }
  remote.ecdh_sig.reset();
  remote.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{"console-own-id"};
  EXPECT_EQ(verify(crypto, local, *profile, key, std::nullopt, remote).error(),
            remote_play::ActiveBangError::missing_remote_key);
  remote.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{std::string{sent}};
  EXPECT_EQ(verify(crypto, local, *profile, key, sent, remote).error(),
            remote_play::ActiveBangError::missing_remote_key);
  EXPECT_EQ(crypto.hmac_calls, 0U);
  EXPECT_EQ(pair.derive_calls, 0U);
  remote = ActiveHandshakeRig::bang(64);
  local.curve = platform::EcdhCurve::secp521r1;
  EXPECT_EQ(verify(crypto, local, *profile, key, sent, remote).error(),
            remote_play::ActiveBangError::curve_mismatch);
  local.curve = platform::EcdhCurve::secp256k1;
  remote.ecdh_pub_key->pop_back();
  EXPECT_EQ(verify(crypto, local, *profile, key, sent, remote).error(),
            remote_play::ActiveBangError::invalid_remote_key);
  remote = ActiveHandshakeRig::bang(64);
  crypto.fail = true;
  EXPECT_EQ(verify(crypto, local, *profile, key, sent, remote).error(),
            remote_play::ActiveBangError::crypto_failure);
  crypto.fail = false;
  pair.fail = true;
  EXPECT_EQ(verify(crypto, local, *profile, key, sent, remote).error(),
            remote_play::ActiveBangError::crypto_failure);
}

}  // namespace
}  // namespace remote_play_test
