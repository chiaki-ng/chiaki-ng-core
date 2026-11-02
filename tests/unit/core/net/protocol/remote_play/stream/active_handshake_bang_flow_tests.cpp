#include "tests/unit/core/net/protocol/remote_play/stream/active_handshake_rig.hpp"
#include "tests/support/crypto/refusing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <variant>

namespace remote_play_test {
namespace {

struct BangFlowCase {
  std::uint32_t version;
  std::size_t secret_size;
};

class ActiveHandshakeBangFlow : public ::testing::TestWithParam<BangFlowCase> {
};

TEST_P(ActiveHandshakeBangFlow,
       InstallsCryptoThenReplaysTheEarlyStreamInfoUntilReady) {
  const auto [version, secret_size] = GetParam();
  auto owner = ActiveHandshakeRig::owner(version);
  EXPECT_FALSE(owner.on_bang(takion::Bang{}));
  EXPECT_FALSE(
      owner.on_stream_info(ActiveHandshakeRig::stream_info_with_width(640)));
  owner.on_connected();
  EXPECT_FALSE(
      owner.on_stream_info(ActiveHandshakeRig::stream_info_with_width(640)));
  EXPECT_TRUE(owner.on_profile_committed(version));
  EXPECT_FALSE(
      owner.on_stream_info(ActiveHandshakeRig::stream_info_with_width(1280)));
  takion::Bang bang;
  bang.server_version = version;
  auto verify = owner.on_bang(std::move(bang));
  ASSERT_TRUE(verify);
  EXPECT_EQ(verify->server_version, version);
  EXPECT_EQ(owner.state(), remote_play::ActiveHandshakeState::verifying_bang);
  EXPECT_FALSE(owner.on_bang(takion::Bang{}));
  EXPECT_FALSE(
      owner.on_stream_info(ActiveHandshakeRig::stream_info_with_width(720)));
  test_support::RefusingTakionCrypto provider;
  const std::array<std::byte, 16> key{};
  auto install = owner.on_bang_verified(
      provider, key, ActiveHandshakeRig::shared_secret(secret_size));
  ASSERT_TRUE(install);
  EXPECT_EQ(install.value().provider, &provider);
  EXPECT_EQ(install.value().local.gkcrypt_index(), 2U);
  EXPECT_EQ(install.value().remote.gkcrypt_index(), 3U);
  EXPECT_EQ(owner.state(),
            remote_play::ActiveHandshakeState::awaiting_crypto_install);
  EXPECT_FALSE(owner.on_bang(takion::Bang{}));
  EXPECT_FALSE(owner.on_bang_verified(
      provider, key, ActiveHandshakeRig::shared_secret(secret_size)));
  EXPECT_FALSE(
      owner.on_stream_info(ActiveHandshakeRig::stream_info_with_width(720)));
  const auto replayed = owner.on_crypto_installed();
  ASSERT_TRUE(replayed);
  EXPECT_EQ(replayed->resolutions.front().width, 1280U);
  EXPECT_EQ(owner.state(),
            remote_play::ActiveHandshakeState::validating_stream_info);
  EXPECT_FALSE(owner.on_crypto_installed());
  EXPECT_FALSE(
      owner.on_stream_info(ActiveHandshakeRig::stream_info_with_width(1920)));
  auto validated = remote_play::StreamInfoCodec::validate(*replayed);
  ASSERT_TRUE(validated);
  auto ready = owner.on_stream_info_validated(std::move(validated.value()));
  ASSERT_TRUE(ready);
  EXPECT_EQ(ready.value().info.audio.channels, 2U);
  EXPECT_TRUE(std::holds_alternative<takion::StreamInfoAcknowledgement>(
      ready.value().ack.message));
  EXPECT_EQ(owner.state(), remote_play::ActiveHandshakeState::ready);
  const auto stale =
      owner.on_stream_info_validated(remote_play::ValidatedStreamInfo{});
  ASSERT_FALSE(stale);
  EXPECT_EQ(
      stale.error(),
      remote_play::ActiveHandshakeError::unexpected_stream_info_validation);
  owner.on_terminal_failure();
  EXPECT_FALSE(
      owner.on_stream_info(ActiveHandshakeRig::stream_info_with_width(1280)));
}

INSTANTIATE_TEST_SUITE_P(Versions, ActiveHandshakeBangFlow,
                         ::testing::Values(BangFlowCase{12, 32},
                                           BangFlowCase{20, 66}));

class ActiveHandshakeBangFailure : public ::testing::Test {
 protected:
  static auto verifying(std::uint32_t version) -> remote_play::ActiveHandshake {
    auto owner = ActiveHandshakeRig::owner(version);
    owner.on_connected();
    EXPECT_TRUE(owner.on_profile_committed(version));
    EXPECT_TRUE(owner.on_bang(takion::Bang{}));
    return owner;
  }

  test_support::RefusingTakionCrypto provider;
  const std::array<std::byte, 16> key{};
};

TEST_F(ActiveHandshakeBangFailure, ASecretOfTheWrongSizeFailsTheInstall) {
  auto invalid = verifying(20);
  const auto bad = invalid.on_bang_verified(
      provider, key, ActiveHandshakeRig::shared_secret(32));
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error(),
            remote_play::ActiveHandshakeError::crypto_install_failure);
  EXPECT_EQ(invalid.state(), remote_play::ActiveHandshakeState::failed);
  EXPECT_FALSE(invalid.on_crypto_installed());
  EXPECT_EQ(invalid.state(), remote_play::ActiveHandshakeState::failed);
}

TEST_F(ActiveHandshakeBangFailure, AShortHandshakeKeyFailsTheInstall) {
  auto short_key = verifying(12);
  const auto short_result =
      short_key.on_bang_verified(provider, std::span(key).first(15),
                                 ActiveHandshakeRig::shared_secret(32));
  ASSERT_FALSE(short_result);
  EXPECT_EQ(short_result.error(),
            remote_play::ActiveHandshakeError::crypto_install_failure);
  EXPECT_EQ(short_key.state(), remote_play::ActiveHandshakeState::failed);
}

TEST_F(ActiveHandshakeBangFailure, AFailedVerificationIsTerminal) {
  auto rejected = verifying(12);
  rejected.on_bang_verification_failed();
  EXPECT_EQ(rejected.state(), remote_play::ActiveHandshakeState::failed);
  EXPECT_FALSE(rejected.on_bang(takion::Bang{}));
  EXPECT_FALSE(rejected.on_bang_verified(
      provider, key, ActiveHandshakeRig::shared_secret(32)));
}

TEST_F(ActiveHandshakeBangFailure, ARejectedBangWaitsForTheNextOne) {
  auto retried = verifying(12);
  retried.on_bang_rejected();
  EXPECT_EQ(retried.state(), remote_play::ActiveHandshakeState::awaiting_bang);
  EXPECT_TRUE(retried.on_bang(takion::Bang{}));
  EXPECT_EQ(retried.state(), remote_play::ActiveHandshakeState::verifying_bang);
}

TEST_F(ActiveHandshakeBangFailure, AVerificationAfterCloseIsIgnored) {
  auto closed = verifying(12);
  closed.on_closed();
  EXPECT_FALSE(closed.on_bang_verified(provider, key,
                                       ActiveHandshakeRig::shared_secret(32)));
  EXPECT_EQ(closed.state(), remote_play::ActiveHandshakeState::closed);
}

}  // namespace
}  // namespace remote_play_test
