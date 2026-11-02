#include "tests/unit/core/net/protocol/remote_play/stream/active_handshake_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace remote_play_test {
namespace {

class ActiveBig : public ::testing::Test {
 protected:
  static void expect_exact_big(std::uint32_t version,
                               platform::EcdhCurve curve) {
    const auto profile = takion::resolve_takion_wire_profile(version);
    ASSERT_TRUE(profile);
    auto pair = ActiveHandshakeRig::prepared(curve);
    const auto result = remote_play::ActiveHandshakeMessages::make_big(
        pair, *profile, ActiveHandshakeRig::secret("session-id"),
        ActiveHandshakeRig::secret("encrypted-launch-spec"));
    ASSERT_TRUE(result);
    const auto& big = result.value();
    EXPECT_EQ(big.client_version, version);
    EXPECT_EQ(big.session_key.view(), "session-id");
    EXPECT_EQ(big.launch_spec.view(), "encrypted-launch-spec");
    EXPECT_EQ(big.encrypted_key, std::vector<std::byte>(4, std::byte{0}));
    ASSERT_TRUE(big.ecdh_pub_key);
    EXPECT_EQ(*big.ecdh_pub_key,
              std::vector<std::byte>(pair.public_key.view().begin(),
                                     pair.public_key.view().end()));
    ASSERT_TRUE(big.ecdh_sig);
    EXPECT_EQ(*big.ecdh_sig,
              std::vector<std::byte>(pair.public_key.signature.begin(),
                                     pair.public_key.signature.end()));

    const takion::ControlCodec codec;
    const auto encoded = codec.encode(takion::ControlMessage{big.clone()});
    ASSERT_TRUE(encoded);
    const auto decoded = codec.decode(encoded.value());
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded.value() == takion::ControlMessage{big.clone()});
  }

  static auto make(remote_play::PreparedEcdh& pair,
                   const takion::WireProfile& profile,
                   std::shared_ptr<const libchiaking::SecretString> key,
                   std::shared_ptr<const libchiaking::SecretString> spec) {
    return remote_play::ActiveHandshakeMessages::make_big(
        pair, profile, std::move(key), std::move(spec));
  }
};

TEST_F(ActiveBig, CarriesTheVersion12KeysAndRoundTripsThroughTheCodec) {
  expect_exact_big(12, platform::EcdhCurve::secp256k1);
}

TEST_F(ActiveBig, CarriesTheVersion20KeysAndRoundTripsThroughTheCodec) {
  expect_exact_big(20, platform::EcdhCurve::secp521r1);
}

TEST_F(ActiveBig, RejectsTheWrongCurveAndMissingFields) {
  const auto profile = takion::resolve_takion_wire_profile(20);
  ASSERT_TRUE(profile);
  auto pair = ActiveHandshakeRig::prepared(platform::EcdhCurve::secp256k1);
  const auto mismatch = make(pair, *profile, ActiveHandshakeRig::secret("id"),
                             ActiveHandshakeRig::secret("spec"));
  ASSERT_FALSE(mismatch);
  EXPECT_EQ(mismatch.error(), remote_play::ActiveBigError::curve_mismatch);
  pair = ActiveHandshakeRig::prepared(platform::EcdhCurve::secp521r1);
  EXPECT_EQ(make(pair, *profile, ActiveHandshakeRig::secret(""),
                 ActiveHandshakeRig::secret("spec"))
                .error(),
            remote_play::ActiveBigError::invalid_session_key);
  EXPECT_EQ(make(pair, *profile, ActiveHandshakeRig::secret("id"),
                 ActiveHandshakeRig::secret(""))
                .error(),
            remote_play::ActiveBigError::invalid_launch_spec);
  pair.public_key.size = 134;
  EXPECT_EQ(make(pair, *profile, ActiveHandshakeRig::secret("id"),
                 ActiveHandshakeRig::secret("spec"))
                .error(),
            remote_play::ActiveBigError::invalid_public_key);
  pair.public_key.size = 133;
  pair.public_key.bytes[0] = std::byte{0x02};
  EXPECT_EQ(make(pair, *profile, ActiveHandshakeRig::secret("id"),
                 ActiveHandshakeRig::secret("spec"))
                .error(),
            remote_play::ActiveBigError::invalid_public_key);
  pair.public_key.bytes[0] = std::byte{0x04};
  pair.key_pair.reset();
  EXPECT_EQ(make(pair, *profile, ActiveHandshakeRig::secret("id"),
                 ActiveHandshakeRig::secret("spec"))
                .error(),
            remote_play::ActiveBigError::invalid_public_key);
}

TEST_F(ActiveBig, BorrowsTheSecretsAndMatchesTheLegacyWire) {
  const auto profile = takion::resolve_takion_wire_profile(20);
  ASSERT_TRUE(profile);
  auto pair = ActiveHandshakeRig::prepared(platform::EcdhCurve::secp521r1);
  auto key = ActiveHandshakeRig::secret("session-id");
  auto launch = ActiveHandshakeRig::secret("encrypted-launch-spec");
  auto secure = make(pair, *profile, key, launch);
  ASSERT_TRUE(secure);
  EXPECT_EQ(secure.value().session_key.view().data(), key->view().data());
  EXPECT_EQ(secure.value().launch_spec.view().data(), launch->view().data());
  auto legacy = secure.value().clone();
  legacy.session_key = takion::SharedSecretText{"session-id"};
  legacy.launch_spec = takion::SharedSecretText{"encrypted-launch-spec"};
  const takion::ControlCodec codec;
  const auto old_wire = codec.encode(takion::ControlMessage{legacy.clone()});
  const auto new_wire =
      codec.encode(takion::ControlMessage{secure.value().clone()});
  ASSERT_TRUE(old_wire);
  ASSERT_TRUE(new_wire);
  EXPECT_TRUE(
      std::ranges::equal(old_wire.value().bytes(), new_wire.value().bytes()));
  EXPECT_TRUE(legacy == secure.value());
}

TEST_F(ActiveBig, RejectsAbsentOrEmptySharedSecrets) {
  const auto profile = takion::resolve_takion_wire_profile(20);
  ASSERT_TRUE(profile);
  auto pair = ActiveHandshakeRig::prepared(platform::EcdhCurve::secp521r1);
  auto key = ActiveHandshakeRig::secret("session-id");
  auto launch = ActiveHandshakeRig::secret("encrypted-launch-spec");
  EXPECT_EQ(make(pair, *profile, nullptr, launch).error(),
            remote_play::ActiveBigError::invalid_session_key);
  EXPECT_EQ(make(pair, *profile, key, nullptr).error(),
            remote_play::ActiveBigError::invalid_launch_spec);
  auto empty = std::make_shared<const libchiaking::SecretString>();
  EXPECT_EQ(make(pair, *profile, empty, launch).error(),
            remote_play::ActiveBigError::invalid_session_key);
  EXPECT_EQ(make(pair, *profile, key, empty).error(),
            remote_play::ActiveBigError::invalid_launch_spec);
  auto wrong_curve =
      ActiveHandshakeRig::prepared(platform::EcdhCurve::secp256k1);
  EXPECT_EQ(make(wrong_curve, *profile, key, launch).error(),
            remote_play::ActiveBigError::curve_mismatch);
}

}  // namespace
}  // namespace remote_play_test
