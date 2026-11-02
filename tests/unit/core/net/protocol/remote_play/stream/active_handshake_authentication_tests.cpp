#include "tests/support/crypto/refusing.hpp"
#include "tests/unit/core/net/protocol/remote_play/stream/active_handshake_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <utility>
#include <variant>

namespace remote_play_test {
namespace {

class ActiveBangAuthentication : public ::testing::Test {
 protected:
  ActiveBangAuthentication() {
    crypto.expected_key_size = 16;
    crypto.expected_input_size = 65;
  }

  auto authenticate(remote_play::ActiveHandshake& handshake,
                    takion::Bang bang) {
    return remote_play::ActiveHandshake::authenticate_bang(
        handshake, crypto, takion_crypto, key, std::move(bang));
  }

  static auto pair_of(const remote_play::ActiveHandshake& handshake)
      -> const HandshakeKeyPair& {
    return ActiveHandshakeRig::key_pair_of(handshake.prepared());
  }

  unsigned call_sequence{};
  const std::array<std::byte, 16> key{};
  HandshakeCrypto crypto{&call_sequence};
  test_support::RefusingTakionCrypto takion_crypto;
};

TEST_F(ActiveBangAuthentication, InstallsCryptoOnceAndIgnoresADuplicate) {
  auto handshake = ActiveHandshakeRig::committed(&call_sequence);
  auto result = authenticate(handshake, ActiveHandshakeRig::bang());
  ASSERT_TRUE(result);
  ASSERT_TRUE(result.value());
  const auto& install = *result.value();
  EXPECT_EQ(install.provider, &takion_crypto);
  EXPECT_EQ(install.local.gkcrypt_index(), 2U);
  EXPECT_EQ(install.remote.gkcrypt_index(), 3U);
  EXPECT_EQ(crypto.hmac_calls, 1U);
  EXPECT_EQ(pair_of(handshake).derive_calls, 1U);
  EXPECT_LT(crypto.hmac_order, pair_of(handshake).derive_order);
  EXPECT_EQ(handshake.state(),
            remote_play::ActiveHandshakeState::awaiting_crypto_install);

  const auto duplicate = authenticate(handshake, ActiveHandshakeRig::bang());
  ASSERT_TRUE(duplicate);
  EXPECT_FALSE(duplicate.value());
  EXPECT_EQ(crypto.hmac_calls, 1U);
  EXPECT_EQ(pair_of(handshake).derive_calls, 1U);
}

TEST_F(ActiveBangAuthentication, ATamperedSignatureFailsClosedAndCanBeRetried) {
  auto handshake = ActiveHandshakeRig::committed(&call_sequence);
  auto tampered = ActiveHandshakeRig::bang();
  tampered.ecdh_sig->at(31) ^= std::byte{1};
  const auto rejected = authenticate(handshake, std::move(tampered));
  ASSERT_FALSE(rejected);
  ASSERT_TRUE(std::holds_alternative<remote_play::ActiveBangError>(
      rejected.error().reason));
  EXPECT_EQ(std::get<remote_play::ActiveBangError>(rejected.error().reason),
            remote_play::ActiveBangError::invalid_remote_signature);
  EXPECT_EQ(crypto.hmac_calls, 1U);
  EXPECT_EQ(pair_of(handshake).derive_calls, 0U);
  EXPECT_EQ(handshake.state(),
            remote_play::ActiveHandshakeState::awaiting_bang);
  const auto retried = authenticate(handshake, ActiveHandshakeRig::bang());
  ASSERT_TRUE(retried);
  EXPECT_TRUE(retried.value());
  EXPECT_EQ(handshake.state(),
            remote_play::ActiveHandshakeState::awaiting_crypto_install);
}

TEST_F(ActiveBangAuthentication, AForeignSessionKeyIsRejectedBeforeEcdh) {
  auto handshake = ActiveHandshakeRig::committed(&call_sequence);
  EXPECT_EQ(handshake.sent_session_key(), "session-id");
  auto foreign = ActiveHandshakeRig::bang();
  foreign.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{"session-ie"};
  const auto rejected = authenticate(handshake, std::move(foreign));
  ASSERT_FALSE(rejected);
  ASSERT_TRUE(std::holds_alternative<remote_play::ActiveBangError>(
      rejected.error().reason));
  EXPECT_EQ(std::get<remote_play::ActiveBangError>(rejected.error().reason),
            remote_play::ActiveBangError::session_key_mismatch);
  EXPECT_EQ(crypto.hmac_calls, 0U);
  EXPECT_EQ(pair_of(handshake).derive_calls, 0U);
  EXPECT_EQ(handshake.state(),
            remote_play::ActiveHandshakeState::awaiting_bang);
}

TEST_F(ActiveBangAuthentication, ComparesASharedSessionKeyExactly) {
  const auto profile = takion::resolve_takion_wire_profile(12);
  ASSERT_TRUE(profile);
  libchiaking::SecretString key_value;
  key_value.assign("session-id");
  libchiaking::SecretString spec_value;
  spec_value.assign("encrypted-launch-spec");
  remote_play::ActiveHandshake handshake{
      ActiveHandshakeRig::prepared(platform::EcdhCurve::secp256k1,
                                   &call_sequence),
      *profile,
      std::make_shared<const libchiaking::SecretString>(std::move(key_value)),
      std::make_shared<const libchiaking::SecretString>(std::move(spec_value))};
  handshake.on_connected();
  const auto big = handshake.on_profile_committed(12);
  ASSERT_TRUE(big);
  EXPECT_TRUE(big.value());
  EXPECT_EQ(handshake.sent_session_key(), "session-id");

  auto truncated = ActiveHandshakeRig::bang();
  truncated.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{"session-i"};
  const auto rejected = authenticate(handshake, std::move(truncated));
  ASSERT_FALSE(rejected);
  ASSERT_TRUE(std::holds_alternative<remote_play::ActiveBangError>(
      rejected.error().reason));
  EXPECT_EQ(std::get<remote_play::ActiveBangError>(rejected.error().reason),
            remote_play::ActiveBangError::session_key_mismatch);
  EXPECT_EQ(crypto.hmac_calls, 0U);
  const auto accepted = authenticate(handshake, ActiveHandshakeRig::bang());
  ASSERT_TRUE(accepted);
  EXPECT_TRUE(accepted.value());
  EXPECT_EQ(handshake.state(),
            remote_play::ActiveHandshakeState::awaiting_crypto_install);
}

class ActiveBangReportedMismatch : public ActiveBangAuthentication,
                                   public ::testing::WithParamInterface<bool> {
};

TEST_P(ActiveBangReportedMismatch, ProceedsAndRecordsWhetherTheKeyDiffered) {
  const bool mismatched = GetParam();
  const auto profile = takion::resolve_takion_wire_profile(12);
  ASSERT_TRUE(profile);
  remote_play::ActiveHandshake handshake{
      ActiveHandshakeRig::prepared(platform::EcdhCurve::secp256k1,
                                   &call_sequence),
      *profile, ActiveHandshakeRig::secret("session-id"),
      ActiveHandshakeRig::secret("encrypted-launch-spec"),
      remote_play::BangSessionKeyPolicy::report_mismatch};
  EXPECT_EQ(handshake.session_key_policy(),
            remote_play::BangSessionKeyPolicy::report_mismatch);
  handshake.on_connected();
  const auto big = handshake.on_profile_committed(12);
  ASSERT_TRUE(big);
  EXPECT_TRUE(big.value());
  auto peer = ActiveHandshakeRig::bang();
  if (mismatched)
    peer.session_key =
        libchiaking::net::protocol::takion::SharedSecretText{"console-own-id"};
  const auto result = authenticate(handshake, std::move(peer));
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value());
  EXPECT_EQ(handshake.state(),
            remote_play::ActiveHandshakeState::awaiting_crypto_install);
  EXPECT_EQ(handshake.session_key_mismatch_tolerated(), mismatched);
}

INSTANTIATE_TEST_SUITE_P(Peers, ActiveBangReportedMismatch, ::testing::Bool());

TEST_F(ActiveBangAuthentication, TheRequiredPolicyRejectsAForeignSessionKey) {
  auto required = ActiveHandshakeRig::committed(&call_sequence);
  EXPECT_EQ(required.session_key_policy(),
            remote_play::BangSessionKeyPolicy::required);
  auto foreign = ActiveHandshakeRig::bang();
  foreign.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{"console-own-id"};
  EXPECT_FALSE(authenticate(required, std::move(foreign)));
  EXPECT_FALSE(required.session_key_mismatch_tolerated());
}

}  // namespace
}  // namespace remote_play_test
