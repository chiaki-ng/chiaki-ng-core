#include "tests/unit/core/net/protocol/remote_play/stream/active_handshake_rig.hpp"
#include "tests/support/crypto/refusing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <utility>
#include <variant>

namespace remote_play_test {
namespace {

class ActiveHandshakeStreamInfo : public ::testing::Test {
 protected:
  test_support::RefusingTakionCrypto provider;
  const std::array<std::byte, 16> key{};
};

TEST_F(ActiveHandshakeStreamInfo, AValidatedStreamInfoMakesTheHandshakeReady) {
  auto accepted = ActiveHandshakeRig::awaiting_stream_info(provider, key);
  auto info =
      accepted.on_stream_info(ActiveHandshakeRig::stream_info_with_width(1920));
  ASSERT_TRUE(info);
  EXPECT_EQ(accepted.state(),
            remote_play::ActiveHandshakeState::validating_stream_info);
  EXPECT_FALSE(accepted.on_stream_info(
      ActiveHandshakeRig::stream_info_with_width(1280)));
  auto parsed = remote_play::StreamInfoCodec::validate(std::move(*info));
  ASSERT_TRUE(parsed);
  auto ready = accepted.on_stream_info_validated(std::move(parsed.value()));
  ASSERT_TRUE(ready);
  EXPECT_EQ(accepted.state(), remote_play::ActiveHandshakeState::ready);
  EXPECT_TRUE(std::holds_alternative<takion::StreamInfoAcknowledgement>(
      ready.value().ack.message));
  EXPECT_FALSE(
      accepted.on_stream_info(ActiveHandshakeRig::stream_info_with_width(640)));
}

TEST_F(ActiveHandshakeStreamInfo, AnInvalidAudioHeaderFailsTheHandshake) {
  auto invalid = ActiveHandshakeRig::awaiting_stream_info(provider, key);
  auto bad = ActiveHandshakeRig::stream_info_with_width(1280);
  bad.audio_header.pop_back();
  auto received = invalid.on_stream_info(std::move(bad));
  ASSERT_TRUE(received);
  const auto rejected =
      remote_play::StreamInfoCodec::validate(std::move(*received));
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(),
            remote_play::StreamInfoError::invalid_audio_header);
  invalid.on_stream_info_validation_failed();
  EXPECT_EQ(invalid.state(), remote_play::ActiveHandshakeState::failed);
  const auto late =
      invalid.on_stream_info_validated(remote_play::ValidatedStreamInfo{});
  ASSERT_FALSE(late);
  EXPECT_EQ(
      late.error(),
      remote_play::ActiveHandshakeError::unexpected_stream_info_validation);
}

TEST_F(ActiveHandshakeStreamInfo, ARejectedStreamInfoWaitsForTheNextOne) {
  auto retried_info = ActiveHandshakeRig::awaiting_stream_info(provider, key);
  EXPECT_TRUE(retried_info.on_stream_info(
      ActiveHandshakeRig::stream_info_with_width(1280)));
  retried_info.on_stream_info_rejected();
  EXPECT_EQ(retried_info.state(),
            remote_play::ActiveHandshakeState::awaiting_stream_info);
  EXPECT_TRUE(retried_info.on_stream_info(
      ActiveHandshakeRig::stream_info_with_width(1280)));
  EXPECT_EQ(retried_info.state(),
            remote_play::ActiveHandshakeState::validating_stream_info);
}

TEST(ActiveHandshakeEarlyStreamInfo, IsDroppedWhenTheHandshakeCloses) {
  auto closed = ActiveHandshakeRig::owner(12);
  closed.on_connected();
  EXPECT_TRUE(closed.on_profile_committed(12));
  EXPECT_FALSE(
      closed.on_stream_info(ActiveHandshakeRig::stream_info_with_width(1280)));
  closed.on_closed();
  EXPECT_FALSE(closed.on_crypto_installed());
  EXPECT_FALSE(
      closed.on_stream_info(ActiveHandshakeRig::stream_info_with_width(1920)));
}

TEST(ActiveHandshakeEarlyStreamInfo, IsDroppedAfterATerminalFailure) {
  auto failed = ActiveHandshakeRig::owner(20);
  failed.on_connected();
  EXPECT_TRUE(failed.on_profile_committed(20));
  EXPECT_FALSE(
      failed.on_stream_info(ActiveHandshakeRig::stream_info_with_width(1280)));
  failed.on_terminal_failure();
  EXPECT_FALSE(failed.on_crypto_installed());
  EXPECT_FALSE(
      failed.on_stream_info(ActiveHandshakeRig::stream_info_with_width(1920)));
}

}  // namespace
}  // namespace remote_play_test
