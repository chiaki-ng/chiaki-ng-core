#include "tests/unit/core/net/protocol/remote_play/ctrl_owner_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <variant>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace lifecycle = remote_play::ctrl_lifecycle_detail;

class LocalSessionIdRepliesOverOwner : public CtrlOwnerRig {
 protected:
  using Progress = remote_play::LocalCtrlEffectProgress;

  static auto step(LocalSession& local, std::uint64_t attempt)
      -> std::optional<Progress> {
    const auto stepped = LocalSession::step_effect(local, attempt);
    EXPECT_TRUE(stepped);
    if (!stepped) return std::nullopt;
    return stepped.value().progress;
  }
};

TEST_F(LocalSessionIdRepliesOverOwner, RunsHeartbeatRepliesWithoutBlocking) {
  CtrlOwnerScript script;
  script.send_limits = {2, std::nullopt, 3, 3};
  auto begun = reader(script, crypto);
  ASSERT_NE(begun, nullptr);
  auto* raw = begun.get();
  auto started = std::make_unique<Owner>(std::move(begun));
  EXPECT_TRUE(started->start(7));
  EXPECT_TRUE(started->on_message(7, remote_play::CtrlHeartbeatRequest{true}));
  EXPECT_TRUE(started->on_message(7, remote_play::CtrlHeartbeatRequest{true}));
  auto local = local_session(std::move(started), 7);
  EXPECT_FALSE(local.pump_once().value());
  EXPECT_TRUE(is<lifecycle::SendHeartbeatReply>(local.front_effect()));
  EXPECT_FALSE(local.heartbeat_pending());
  EXPECT_FALSE(local.pin_pending());
  EXPECT_TRUE(local.run_next_heartbeat().value());
  EXPECT_TRUE(local.heartbeat_pending());
  EXPECT_FALSE(local.pin_pending());
  ASSERT_TRUE(is<lifecycle::SendHeartbeatReply>(local.front_effect()));
  EXPECT_EQ(
      std::get<lifecycle::SendHeartbeatReply>(*local.front_effect()).attempt_id,
      7U);
  EXPECT_FALSE(local.take_effect());
  EXPECT_FALSE(local.run_next_heartbeat().value());
  EXPECT_FALSE(local.pump_heartbeat_outbound().value());
  EXPECT_FALSE(local.pump_heartbeat_outbound().value());
  EXPECT_TRUE(local.pump_heartbeat_outbound().value());
  EXPECT_FALSE(local.heartbeat_pending());
  EXPECT_TRUE(is<lifecycle::SendHeartbeatReply>(local.front_effect()));
  EXPECT_FALSE(local.pump_heartbeat_outbound());
  EXPECT_TRUE(local.run_next_heartbeat().value());
  EXPECT_FALSE(local.run_next_heartbeat().value());
  EXPECT_EQ(script.sent, frames({{0x1fe, {}}, {0x1fe, {}}}));
  EXPECT_EQ(raw->local_counter(), 5U);
  EXPECT_FALSE(local.take_effect());
  ASSERT_TRUE(local.owner());
  EXPECT_TRUE(local.owner()->stop(7));
  EXPECT_TRUE(local.owner()->run_cleanup(7));
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(LocalSessionIdRepliesOverOwner, DropsAHeartbeatReplyWhoseSendFails) {
  CtrlOwnerScript script;
  script.send_fail = true;
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(9));
  EXPECT_TRUE(started->on_message(9, remote_play::CtrlHeartbeatRequest{true}));
  auto failing = local_session(std::move(started), 9);
  EXPECT_FALSE(failing.pump_once().value());
  const auto failure = failing.run_next_heartbeat();
  ASSERT_TRUE(failure);
  EXPECT_TRUE(failure.value());
  EXPECT_FALSE(failing.heartbeat_pending());
  EXPECT_FALSE(failing.take_effect());
  EXPECT_EQ(script.closes, 0U);
  ASSERT_TRUE(failing.owner());
  EXPECT_TRUE(failing.owner()->stop(9));
  EXPECT_TRUE(is<lifecycle::Cleanup>(failing.owner()->take_effect()));
  EXPECT_TRUE(failing.owner()->run_cleanup(9));
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(LocalSessionIdRepliesOverOwner, RunsPinRepliesWithoutBlocking) {
  CtrlOwnerScript script;
  script.send_limits = {4, std::nullopt, 8};
  auto begun = reader(script, crypto);
  ASSERT_NE(begun, nullptr);
  auto* raw = begun.get();
  auto started = std::make_unique<Owner>(std::move(begun));
  EXPECT_TRUE(started->start(7));
  EXPECT_TRUE(started->on_message(7, remote_play::CtrlLoginPinRequest{}));
  auto local = local_session(std::move(started), 7);
  const std::array<std::byte, 505> oversized{};
  const auto too_large = local.run_next_pin_reply(oversized);
  ASSERT_FALSE(too_large);
  EXPECT_EQ(too_large.error(),
            remote_play::LocalCtrlSessionIdError::pin_too_large);
  EXPECT_TRUE(script.sent.empty());
  EXPECT_EQ(raw->local_counter(), 5U);
  EXPECT_TRUE(local.run_next_pin_reply(pin).value());
  EXPECT_TRUE(local.pin_pending());
  EXPECT_FALSE(local.heartbeat_pending());
  EXPECT_FALSE(local.run_next_pin_reply(pin).value());
  EXPECT_FALSE(local.run_next_heartbeat().value());
  EXPECT_FALSE(local.pump_pin_outbound().value());
  EXPECT_TRUE(local.pump_pin_outbound().value());
  EXPECT_FALSE(local.pin_pending());
  EXPECT_FALSE(local.pump_pin_outbound());
  EXPECT_EQ(script.sent, frame(0x8004, pin));
  EXPECT_EQ(raw->local_counter(), 6U);
  EXPECT_FALSE(local.take_effect());
  ASSERT_TRUE(local.owner());
  EXPECT_TRUE(local.owner()->stop(7));
  EXPECT_TRUE(local.run_next_cleanup().value());
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(LocalSessionIdRepliesOverOwner, DropsAPinReplyWhoseCryptoFails) {
  CtrlOwnerScript script;
  CtrlOwnerCrypto failing_crypto;
  failing_crypto.fail_hmac = true;
  auto started = owner(script, failing_crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(9));
  EXPECT_TRUE(started->on_message(9, remote_play::CtrlLoginPinRequest{}));
  auto failing = local_session(std::move(started), 9);
  const auto failure = failing.run_next_pin_reply(pin);
  ASSERT_TRUE(failure);
  EXPECT_TRUE(failure.value());
  EXPECT_FALSE(failing.pin_pending());
  EXPECT_EQ(failing.owner()->dropped_sends(), 1U);
  EXPECT_EQ(script.closes, 0U);
  EXPECT_TRUE(failing.owner()->stop(9));
  EXPECT_TRUE(failing.run_next_cleanup().value());
  EXPECT_EQ(script.closes, 1U);
  EXPECT_TRUE(is<lifecycle::PublishStopped>(failing.take_effect()));
}

TEST_F(LocalSessionIdRepliesOverOwner, DropsAPinReplyWhoseFlushFails) {
  CtrlOwnerScript script;
  script.send_limits = {4};
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(12));
  EXPECT_TRUE(started->on_message(12, remote_play::CtrlLoginPinRequest{}));
  auto pump_failing = local_session(std::move(started), 12);
  EXPECT_TRUE(pump_failing.run_next_pin_reply(pin).value());
  script.send_fail = true;
  const auto pump_error = pump_failing.pump_pin_outbound();
  ASSERT_TRUE(pump_error);
  EXPECT_TRUE(pump_error.value());
  EXPECT_FALSE(pump_failing.pin_pending());
  EXPECT_EQ(script.closes, 0U);
  EXPECT_TRUE(pump_failing.owner()->stop(12));
  EXPECT_TRUE(pump_failing.run_next_cleanup().value());
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(LocalSessionIdRepliesOverOwner,
       LeavesAPinRequestOfAnotherAttemptUnanswered) {
  CtrlOwnerScript script;
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(10));
  EXPECT_TRUE(started->on_message(10, remote_play::CtrlLoginPinRequest{}));
  auto stale = local_session(std::move(started), 11);
  EXPECT_FALSE(stale.run_next_pin_reply(pin).value());
  EXPECT_TRUE(is<lifecycle::RequestPin>(stale.take_effect()));
  EXPECT_TRUE(script.sent.empty());
  ASSERT_TRUE(stale.owner());
  EXPECT_TRUE(stale.owner()->stop(10));
  EXPECT_TRUE(stale.owner()->run_cleanup(10));
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(LocalSessionIdRepliesOverOwner, RunsCleanupOnceAndPublishesStopped) {
  CtrlOwnerScript script;
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(7));
  auto local = local_session(std::move(started), 7);
  ASSERT_TRUE(local.owner());
  EXPECT_TRUE(local.owner()->stop(7));
  EXPECT_TRUE(local.run_next_cleanup().value());
  EXPECT_EQ(script.closes, 1U);
  EXPECT_FALSE(local.run_next_cleanup().value());
  EXPECT_TRUE(is<lifecycle::PublishStopped>(local.take_effect()));
  EXPECT_FALSE(local.take_effect());
}

TEST_F(LocalSessionIdRepliesOverOwner, RunsCleanupOnceAfterAReaderFailure) {
  CtrlOwnerScript script;
  script.disconnect = true;
  auto started = owner(script, crypto, Target::ps4_9, 0);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(8));
  auto failing = local_session(std::move(started), 8);
  EXPECT_FALSE(failing.pump_once());
  EXPECT_TRUE(failing.run_next_cleanup().value());
  EXPECT_EQ(script.closes, 1U);
  EXPECT_TRUE(is<lifecycle::PublishFailed>(failing.take_effect()));
  EXPECT_FALSE(failing.run_next_cleanup().value());
}

TEST_F(LocalSessionIdRepliesOverOwner, ReportsAFailedCloseOnceAsFailed) {
  CtrlOwnerScript script;
  script.close_fail = true;
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(9));
  auto closing = local_session(std::move(started), 9);
  ASSERT_TRUE(closing.owner());
  EXPECT_TRUE(closing.owner()->stop(9));
  const auto close_result = closing.run_next_cleanup();
  ASSERT_FALSE(close_result);
  EXPECT_EQ(close_result.error(),
            remote_play::LocalCtrlSessionIdError::reader_failure);
  EXPECT_EQ(script.closes, 1U);
  EXPECT_TRUE(is<lifecycle::PublishFailed>(closing.take_effect()));
  EXPECT_FALSE(closing.run_next_cleanup().value());
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(LocalSessionIdRepliesOverOwner, StepsEffectsKeepingHeartbeatBeforePin) {
  CtrlOwnerScript script;
  script.send_limits = {2, std::nullopt, 6, 4, std::nullopt, 8};
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(7));
  EXPECT_TRUE(started->on_message(7, remote_play::CtrlHeartbeatRequest{true}));
  EXPECT_TRUE(started->on_message(7, remote_play::CtrlLoginPinRequest{}));
  auto local = local_session(std::move(started), 7);
  EXPECT_FALSE(local.pump_once().value());
  EXPECT_EQ(step(local, 7), Progress::advanced);
  EXPECT_TRUE(local.heartbeat_pending());
  EXPECT_EQ(step(local, 7), Progress::advanced);
  EXPECT_TRUE(local.heartbeat_pending());
  EXPECT_EQ(step(local, 7), Progress::advanced);
  EXPECT_FALSE(local.heartbeat_pending());
  EXPECT_EQ(step(local, 7), Progress::pin_required);
  EXPECT_TRUE(is<lifecycle::RequestPin>(local.front_effect()));
  EXPECT_EQ(script.sent, frame(0x1fe, {}));

  EXPECT_TRUE(local.run_next_pin_reply(pin).value());
  EXPECT_TRUE(local.pin_pending());
  EXPECT_EQ(step(local, 7), Progress::advanced);
  EXPECT_TRUE(local.pin_pending());
  EXPECT_EQ(step(local, 7), Progress::advanced);
  EXPECT_FALSE(local.pin_pending());
  EXPECT_EQ(script.sent, frames({{0x1fe, {}}, {0x8004, pin}}));

  ASSERT_TRUE(local.owner());
  EXPECT_TRUE(local.owner()->stop(7));
  EXPECT_FALSE(local.pump_once());
  EXPECT_EQ(step(local, 7), Progress::advanced);
  EXPECT_EQ(script.closes, 1U);
  const auto terminal = LocalSession::step_effect(local, 7);
  ASSERT_TRUE(terminal);
  EXPECT_EQ(terminal.value().progress, Progress::notification);
  EXPECT_TRUE(is<lifecycle::PublishStopped>(terminal.value().notification));
  EXPECT_EQ(step(local, 7), Progress::idle);
}

TEST_F(LocalSessionIdRepliesOverOwner, MarksAnEffectOfAnotherAttemptStale) {
  CtrlOwnerScript script;
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(9));
  EXPECT_TRUE(started->on_message(9, remote_play::CtrlHeartbeatRequest{true}));
  auto stale = local_session(std::move(started), 9);
  EXPECT_FALSE(stale.pump_once().value());
  EXPECT_EQ(step(stale, 10), Progress::stale);
  EXPECT_TRUE(script.sent.empty());
  EXPECT_FALSE(stale.front_effect());
  ASSERT_TRUE(stale.owner());
  EXPECT_TRUE(stale.owner()->stop(9));
  EXPECT_TRUE(stale.owner()->run_cleanup(9));
  EXPECT_EQ(script.closes, 1U);
}

}  // namespace
}  // namespace remote_play_test
