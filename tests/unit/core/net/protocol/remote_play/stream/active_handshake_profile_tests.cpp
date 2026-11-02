#include "tests/unit/core/net/protocol/remote_play/stream/active_handshake_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <variant>

namespace remote_play_test {
namespace {

class ActiveHandshakeProfile : public ::testing::TestWithParam<std::uint32_t> {
};

TEST_P(ActiveHandshakeProfile,
       SendsTheBigOnceTheProfileCommitsAfterConnecting) {
  const auto version = GetParam();
  auto owner = ActiveHandshakeRig::owner(version);
  EXPECT_EQ(owner.state(),
            remote_play::ActiveHandshakeState::awaiting_connection);
  const auto early = owner.on_profile_committed(version);
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(),
            remote_play::ActiveHandshakeError::unexpected_profile);
  owner.on_connected();
  owner.on_connected();
  EXPECT_EQ(owner.state(), remote_play::ActiveHandshakeState::awaiting_profile);
  auto action = owner.on_profile_committed(version);
  ASSERT_TRUE(action);
  ASSERT_TRUE(action.value());
  const auto* big = std::get_if<takion::Big>(&action.value()->message);
  ASSERT_TRUE(big);
  EXPECT_EQ(big->client_version, version);
  EXPECT_EQ(big->session_key.view(), "session-id");
  EXPECT_EQ(big->launch_spec.view(), "launch-spec");
  EXPECT_TRUE(big->ecdh_pub_key);
  EXPECT_TRUE(big->ecdh_sig);
  EXPECT_EQ(owner.state(), remote_play::ActiveHandshakeState::awaiting_bang);
  const auto duplicate = owner.on_profile_committed(version);
  ASSERT_TRUE(duplicate);
  EXPECT_FALSE(duplicate.value());
  owner.on_connected();
  EXPECT_EQ(owner.state(), remote_play::ActiveHandshakeState::awaiting_bang);
  owner.on_closed();
  EXPECT_EQ(owner.state(), remote_play::ActiveHandshakeState::closed);
  const auto after_close = owner.on_profile_committed(version);
  ASSERT_TRUE(after_close);
  EXPECT_FALSE(after_close.value());
}

INSTANTIATE_TEST_SUITE_P(Versions, ActiveHandshakeProfile,
                         ::testing::Values(12U, 20U));

class ActiveHandshakeSecrets : public ::testing::Test {
 protected:
  static auto shared(const char* text)
      -> std::shared_ptr<const libchiaking::SecretString> {
    libchiaking::SecretString value;
    value.assign(text);
    return std::make_shared<const libchiaking::SecretString>(std::move(value));
  }
};

TEST_F(ActiveHandshakeSecrets, BigBorrowsTheSharedSecrets) {
  const auto profile = takion::resolve_takion_wire_profile(12);
  ASSERT_TRUE(profile);
  auto key = shared("session-id");
  auto spec = shared("launch-spec");
  remote_play::ActiveHandshake owner{
      ActiveHandshakeRig::prepared(platform::EcdhCurve::secp256k1), *profile,
      key, spec};
  owner.on_connected();
  auto action = owner.on_profile_committed(12);
  ASSERT_TRUE(action);
  ASSERT_TRUE(action.value());
  const auto* big = std::get_if<takion::Big>(&action.value()->message);
  ASSERT_TRUE(big);
  EXPECT_EQ(big->session_key.view().data(), key->view().data());
  EXPECT_EQ(big->launch_spec.view().data(), spec->view().data());
  const auto duplicate = owner.on_profile_committed(12);
  ASSERT_TRUE(duplicate);
  EXPECT_FALSE(duplicate.value());
}

TEST_F(ActiveHandshakeSecrets, AMissingSessionKeyFailsTheBig) {
  const auto profile = takion::resolve_takion_wire_profile(12);
  ASSERT_TRUE(profile);
  remote_play::ActiveHandshake missing{
      ActiveHandshakeRig::prepared(platform::EcdhCurve::secp256k1), *profile,
      std::shared_ptr<const libchiaking::SecretString>{},
      shared("launch-spec")};
  missing.on_connected();
  const auto rejected = missing.on_profile_committed(12);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), remote_play::ActiveHandshakeError::big_failure);
  EXPECT_EQ(missing.state(), remote_play::ActiveHandshakeState::failed);
}

TEST_F(ActiveHandshakeSecrets, AMissingLaunchSpecFailsTheBig) {
  const auto profile = takion::resolve_takion_wire_profile(12);
  ASSERT_TRUE(profile);
  remote_play::ActiveHandshake missing_spec{
      ActiveHandshakeRig::prepared(platform::EcdhCurve::secp256k1), *profile,
      shared("session-id"), std::shared_ptr<const libchiaking::SecretString>{}};
  missing_spec.on_connected();
  const auto rejected = missing_spec.on_profile_committed(12);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), remote_play::ActiveHandshakeError::big_failure);
  EXPECT_EQ(missing_spec.state(), remote_play::ActiveHandshakeState::failed);
}

TEST(ActiveHandshakeFailure, AnUnbuildableBigFailsTheHandshake) {
  const auto profile = takion::resolve_takion_wire_profile(20);
  ASSERT_TRUE(profile);
  remote_play::ActiveHandshake bad_big{
      ActiveHandshakeRig::prepared(platform::EcdhCurve::secp256k1), *profile,
      ActiveHandshakeRig::secret("session-id"),
      ActiveHandshakeRig::secret("launch-spec")};
  bad_big.on_connected();
  const auto bad = bad_big.on_profile_committed(20);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error(), remote_play::ActiveHandshakeError::big_failure);
  EXPECT_EQ(bad_big.state(), remote_play::ActiveHandshakeState::failed);
}

TEST(ActiveHandshakeFailure, AnotherCommittedVersionFailsAndStaysFailed) {
  auto mismatch = ActiveHandshakeRig::owner(12);
  mismatch.on_connected();
  const auto wrong = mismatch.on_profile_committed(20);
  ASSERT_FALSE(wrong);
  EXPECT_EQ(wrong.error(), remote_play::ActiveHandshakeError::version_mismatch);
  EXPECT_EQ(mismatch.state(), remote_play::ActiveHandshakeState::failed);
  const auto late_mismatch = mismatch.on_profile_committed(12);
  ASSERT_TRUE(late_mismatch);
  EXPECT_FALSE(late_mismatch.value());
  mismatch.on_closed();
  EXPECT_EQ(mismatch.state(), remote_play::ActiveHandshakeState::failed);
}

TEST(ActiveHandshakeFailure, AClosedHandshakeIgnoresLateEvents) {
  auto closed = ActiveHandshakeRig::owner(12);
  closed.on_connected();
  closed.on_closed();
  EXPECT_EQ(closed.state(), remote_play::ActiveHandshakeState::closed);
  const auto late = closed.on_profile_committed(12);
  ASSERT_TRUE(late);
  EXPECT_FALSE(late.value());
  closed.on_terminal_failure();
  EXPECT_EQ(closed.state(), remote_play::ActiveHandshakeState::closed);
}

TEST(ActiveHandshakeFailure, ATerminalFailureIgnoresLateEvents) {
  auto terminal = ActiveHandshakeRig::owner(20);
  terminal.on_terminal_failure();
  terminal.on_connected();
  EXPECT_EQ(terminal.state(), remote_play::ActiveHandshakeState::failed);
  const auto late_terminal = terminal.on_profile_committed(20);
  ASSERT_TRUE(late_terminal);
  EXPECT_FALSE(late_terminal.value());
}

}  // namespace
}  // namespace remote_play_test
