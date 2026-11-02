#include "tests/unit/core/net/protocol/remote_play/ctrl_owner_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace lifecycle = remote_play::ctrl_lifecycle_detail;

class CtrlOwnerTest : public CtrlOwnerRig {};

TEST_F(CtrlOwnerTest, DecodesAFrameAndOrdersItsEffects) {
  CtrlOwnerScript script;
  script.incoming = session_id_frame();
  auto owner_ptr = owner(script, crypto);
  ASSERT_NE(owner_ptr, nullptr);
  auto& owner = *owner_ptr;
  EXPECT_FALSE(owner.last_frame_type());
  EXPECT_TRUE(owner.start(7));
  EXPECT_FALSE(owner.start(8));
  EXPECT_TRUE(owner.pump_once().value());
  EXPECT_EQ(owner.last_frame_type(), 0x33);
  auto effect = owner.take_effect();
  ASSERT_TRUE(is<lifecycle::PublishSessionId>(effect));
  EXPECT_EQ(std::get<lifecycle::PublishSessionId>(*effect).value.view(),
            "AAAAAAAAAAAAAAAAAAAAAAAA");
  EXPECT_EQ(owner.state(), lifecycle::State::active);
  EXPECT_FALSE(owner.take_effect());
  EXPECT_FALSE(owner.pump_once().value());
  EXPECT_EQ(owner.last_frame_type(), 0x33);
  EXPECT_FALSE(owner.on_message(8, remote_play::CtrlHeartbeatRequest{}));
  EXPECT_FALSE(owner.take_effect());
  EXPECT_TRUE(owner.on_message(7, remote_play::CtrlHeartbeatRequest{true}));
  EXPECT_TRUE(is<lifecycle::SendHeartbeatReply>(owner.take_effect()));
  EXPECT_TRUE(owner.stop(7));
  EXPECT_TRUE(is<lifecycle::Cleanup>(owner.take_effect()));
  EXPECT_TRUE(owner.run_cleanup(7));
  EXPECT_TRUE(is<lifecycle::PublishStopped>(owner.take_effect()));
  EXPECT_FALSE(owner.take_effect());
  EXPECT_EQ(script.closes, 1U);
  EXPECT_FALSE(owner.run_cleanup(7));
}

TEST_F(CtrlOwnerTest, AReaderFailureEmitsCleanupAndThenFails) {
  CtrlOwnerScript script;
  script.disconnect = true;
  auto owner_ptr = owner(script, crypto, Target::ps4_9, 0);
  ASSERT_NE(owner_ptr, nullptr);
  auto& owner = *owner_ptr;
  EXPECT_TRUE(owner.start(7));
  const auto failed = owner.pump_once();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), remote_play::CtrlReaderError::disconnected);
  EXPECT_EQ(owner.state(), lifecycle::State::closing);
  EXPECT_TRUE(is<lifecycle::Cleanup>(owner.take_effect()));
  EXPECT_EQ(script.closes, 1U);
  EXPECT_FALSE(owner.pump_once());
  EXPECT_TRUE(owner.run_cleanup(7));
  const auto effect = owner.take_effect();
  ASSERT_TRUE(is<lifecycle::PublishFailed>(effect));
  EXPECT_EQ(std::get<lifecycle::PublishFailed>(*effect).reason,
            lifecycle::FailureReason::transport);
}

TEST_F(CtrlOwnerTest, FinishesHeartbeatRepliesAcrossShortSends) {
  CtrlOwnerScript script;
  script.send_limits = {2, std::nullopt, 3, 3};
  auto begun = reader(script, crypto);
  ASSERT_NE(begun, nullptr);
  auto* raw = begun.get();
  remote_play::CtrlOwner owner(std::move(begun));
  EXPECT_TRUE(owner.start(7));
  EXPECT_FALSE(owner.run_heartbeat_reply(8));
  EXPECT_FALSE(owner.run_heartbeat_reply(7).value());
  EXPECT_FALSE(owner.run_heartbeat_reply(7).value());
  EXPECT_EQ(owner.state(), lifecycle::State::waiting_id);
  EXPECT_FALSE(owner.take_effect());
  EXPECT_FALSE(owner.pump_outbound().value());
  EXPECT_FALSE(owner.pump_outbound().value());
  EXPECT_TRUE(owner.pump_outbound().value());
  EXPECT_EQ(script.sent, frames({{0x1fe, {}}, {0x1fe, {}}}));
  EXPECT_EQ(raw->local_counter(), 5U);
  EXPECT_FALSE(owner.pump_outbound());
  EXPECT_TRUE(owner.stop(7));
  EXPECT_TRUE(is<lifecycle::Cleanup>(owner.take_effect()));
  EXPECT_TRUE(owner.run_cleanup(7));
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(CtrlOwnerTest, AFailedHeartbeatSendKeepsTheSocket) {
  CtrlOwnerScript script;
  script.send_fail = true;
  auto owner_ptr = owner(script, crypto, Target::ps4_9, 2);
  ASSERT_NE(owner_ptr, nullptr);
  auto& owner = *owner_ptr;
  EXPECT_TRUE(owner.start(9));
  const auto failed = owner.run_heartbeat_reply(9);
  ASSERT_TRUE(failed);
  EXPECT_TRUE(failed.value());
  EXPECT_EQ(owner.state(), lifecycle::State::waiting_id);
  EXPECT_FALSE(owner.take_effect());
  EXPECT_EQ(script.closes, 0U);
  script.send_fail = false;
  EXPECT_TRUE(owner.run_heartbeat_reply(9).value());
  EXPECT_EQ(script.sent.size(), remote_play::ctrl_frame_header_size);
  EXPECT_TRUE(owner.stop(9));
  EXPECT_TRUE(is<lifecycle::Cleanup>(owner.take_effect()));
  EXPECT_TRUE(owner.run_cleanup(9));
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(CtrlOwnerTest, SendsAPinReplyOnlyWhenAskedAndWithinBounds) {
  CtrlOwnerScript script;
  script.send_limits = {4, std::nullopt, 8};
  auto begun = reader(script, crypto);
  ASSERT_NE(begun, nullptr);
  auto* raw = begun.get();
  remote_play::CtrlOwner owner(std::move(begun));
  EXPECT_TRUE(owner.start(7));
  EXPECT_FALSE(owner.run_pin_reply(8, pin));
  EXPECT_FALSE(owner.run_pin_reply(7, pin));
  EXPECT_TRUE(owner.on_message(7, remote_play::CtrlLoginPinRequest{}));
  EXPECT_TRUE(is<lifecycle::RequestPin>(owner.take_effect()));
  const std::array<std::byte, 505> oversized{};
  const auto too_large = owner.run_pin_reply(7, oversized);
  ASSERT_FALSE(too_large);
  EXPECT_EQ(too_large.error(), remote_play::CtrlReaderError::too_large);
  EXPECT_EQ(raw->local_counter(), 5U);
  EXPECT_FALSE(owner.run_pin_reply(7, pin).value());
  EXPECT_EQ(raw->local_counter(), 6U);
  EXPECT_FALSE(owner.pump_outbound().value());
  EXPECT_TRUE(owner.pump_outbound().value());
  EXPECT_EQ(script.sent, frame(0x8004, pin));
  EXPECT_FALSE(owner.take_effect());
  EXPECT_TRUE(owner.stop(7));
  EXPECT_TRUE(is<lifecycle::Cleanup>(owner.take_effect()));
  EXPECT_TRUE(owner.run_cleanup(7));
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(CtrlOwnerTest, DropsAPinReplyWhoseCryptoFailsAndKeepsCtrl) {
  CtrlOwnerScript script;
  CtrlOwnerCrypto failing;
  failing.fail_hmac = true;
  auto owner_ptr = owner(script, failing);
  ASSERT_NE(owner_ptr, nullptr);
  auto& owner = *owner_ptr;
  EXPECT_TRUE(owner.start(9));
  EXPECT_TRUE(owner.on_message(9, remote_play::CtrlLoginPinRequest{}));
  EXPECT_TRUE(owner.take_effect());
  const auto crypto_error = owner.run_pin_reply(9, pin);
  ASSERT_TRUE(crypto_error);
  EXPECT_TRUE(crypto_error.value());
  EXPECT_EQ(owner.dropped_sends(), 1U);
  EXPECT_EQ(owner.state(), lifecycle::State::waiting_id);
  EXPECT_FALSE(owner.take_effect());
  EXPECT_EQ(script.closes, 0U);
  EXPECT_TRUE(owner.stop(9));
  EXPECT_TRUE(is<lifecycle::Cleanup>(owner.take_effect()));
  EXPECT_TRUE(owner.run_cleanup(9));
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(CtrlOwnerTest, SerializesControlFramesAfterTheSessionId) {
  CtrlOwnerScript script;
  script.send_limits = {2, std::nullopt, 100};
  auto begun = reader(script, crypto);
  ASSERT_NE(begun, nullptr);
  auto* raw = begun.get();
  remote_play::CtrlOwner owner(std::move(begun));
  EXPECT_TRUE(owner.start(7));
  EXPECT_FALSE(owner.run_control_frame(7, 0x36, mic_payload));
  EXPECT_TRUE(owner.on_message(7, remote_play::CtrlSessionId{}));
  EXPECT_EQ(owner.state(), lifecycle::State::active);
  EXPECT_FALSE(owner.run_control_frame(8, 0x36, mic_payload));
  EXPECT_FALSE(owner.run_control_frame(7, 0x36, mic_payload).value());
  EXPECT_FALSE(owner.run_control_frame(7, 0x36, mic_payload).value());
  EXPECT_TRUE(owner.pump_outbound().value());
  EXPECT_EQ(script.sent, frames({{0x36, mic_payload}, {0x36, mic_payload}}));
  EXPECT_EQ(raw->local_counter(), 7U);
  EXPECT_TRUE(owner.stop(7));
  EXPECT_TRUE(owner.take_effect());
  EXPECT_TRUE(owner.run_cleanup(7));
}

TEST_F(CtrlOwnerTest,
       QueuesAHeartbeatBehindAFlushingFrameAndSurvivesSendErrors) {
  CtrlOwnerScript script;
  script.send_limits = {2, std::nullopt};
  auto owner_ptr = owner(script, crypto);
  ASSERT_NE(owner_ptr, nullptr);
  auto& owner = *owner_ptr;
  EXPECT_TRUE(owner.start(7));
  EXPECT_TRUE(owner.on_message(7, remote_play::CtrlSessionId{}));
  (void)owner.take_effect();
  EXPECT_FALSE(owner.run_control_frame(7, 0x36, mic_payload).value());
  EXPECT_TRUE(owner.outbound_pending());
  const auto queued = owner.run_heartbeat_reply(7);
  ASSERT_TRUE(queued);
  EXPECT_FALSE(queued.value());
  EXPECT_TRUE(owner.pump_outbound().value());
  EXPECT_EQ(script.sent, frames({{0x36, mic_payload}, {0x1fe, {}}}));

  script.send_fail = true;
  const auto dropped = owner.run_control_frame(7, 0x36, mic_payload);
  ASSERT_TRUE(dropped);
  EXPECT_TRUE(dropped.value());
  EXPECT_EQ(owner.state(), lifecycle::State::active);
  EXPECT_FALSE(owner.take_effect());
  EXPECT_EQ(script.closes, 0U);
  EXPECT_TRUE(owner.on_message(7, remote_play::CtrlHeartbeatRequest{}));
  EXPECT_TRUE(is<lifecycle::SendHeartbeatReply>(owner.take_effect()));
  EXPECT_TRUE(owner.run_heartbeat_reply(7).value());
  EXPECT_TRUE(owner.stop(7));
  EXPECT_TRUE(owner.take_effect());
  EXPECT_TRUE(owner.run_cleanup(7));
}

}  // namespace
}  // namespace remote_play_test
