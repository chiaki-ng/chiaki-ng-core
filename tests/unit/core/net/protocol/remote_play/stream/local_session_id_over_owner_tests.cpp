#include "tests/unit/core/net/protocol/remote_play/ctrl_owner_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <variant>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace lifecycle = remote_play::ctrl_lifecycle_detail;

class LocalSessionIdOverOwner : public CtrlOwnerRig {};

TEST_F(LocalSessionIdOverOwner, KeepsOtherEffectsAndTheNonce) {
  CtrlOwnerScript script;
  script.incoming = session_id_frame();
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(7));
  EXPECT_TRUE(started->on_message(7, remote_play::CtrlHeartbeatRequest{true}));
  auto local = local_session(std::move(started), 7, std::byte{0x42});
  EXPECT_TRUE(local.pump_once().value());
  const auto id = local.take_session_id();
  ASSERT_TRUE(id);
  EXPECT_EQ(id->view(), "AAAAAAAAAAAAAAAAAAAAAAAA");
  EXPECT_FALSE(local.take_session_id());
  const auto nonce = local.take_nonce();
  ASSERT_TRUE(nonce);
  EXPECT_EQ((*nonce)[0], std::byte{0x42});
  EXPECT_FALSE(local.take_nonce());
  EXPECT_TRUE(is<lifecycle::SendHeartbeatReply>(local.take_effect()));
  EXPECT_FALSE(local.take_effect());
  EXPECT_TRUE(script.sent.empty());
  ASSERT_TRUE(local.owner());
  EXPECT_TRUE(local.owner()->stop(7));
  EXPECT_TRUE(local.owner()->run_cleanup(7));
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(LocalSessionIdOverOwner, DropsPinRequestsAfterTheSessionId) {
  CtrlOwnerScript script;
  script.incoming = session_id_frame();
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(7));
  EXPECT_TRUE(started->on_message(7, remote_play::CtrlLoginPinRequest{}));
  auto local = local_session(std::move(started), 7);
  EXPECT_TRUE(local.pump_once().value());
  EXPECT_TRUE(local.take_ready_ticket());
  EXPECT_TRUE(
      local.owner()->on_message(7, remote_play::CtrlLoginPinIncorrect{}));
  EXPECT_TRUE(
      local.owner()->on_message(7, remote_play::CtrlHeartbeatRequest{true}));
  local.promote_heartbeat_replies();
  EXPECT_TRUE(is<lifecycle::SendHeartbeatReply>(local.take_effect()));
  const auto pin_effect = local.take_effect();
  ASSERT_TRUE(is<lifecycle::RequestPin>(pin_effect));
  EXPECT_FALSE(std::get<lifecycle::RequestPin>(*pin_effect).incorrect);
  EXPECT_FALSE(local.take_effect());
  EXPECT_TRUE(local.owner()->stop(7));
  EXPECT_TRUE(local.run_next_cleanup().value());
}

TEST_F(LocalSessionIdOverOwner,
       KeepsPinRequestsAfterTheSessionIdWhilePrompted) {
  CtrlOwnerScript script;
  script.incoming = session_id_frame();
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(7));
  EXPECT_TRUE(started->on_message(7, remote_play::CtrlLoginPinRequest{}));
  auto local = local_session(std::move(started), 7);
  local.set_pin_prompt_open(true);
  EXPECT_TRUE(local.pump_once().value());
  EXPECT_TRUE(local.take_ready_ticket());
  EXPECT_EQ(local.owner()->state(), lifecycle::State::active);
  EXPECT_TRUE(
      local.owner()->on_message(7, remote_play::CtrlLoginPinIncorrect{}));
  EXPECT_TRUE(local.run_next_pin_reply(pin).value());
  while (local.pin_pending()) ASSERT_TRUE(local.pump_pin_outbound());
  EXPECT_EQ(script.sent, frame(0x8004, pin));
  const auto* front = local.front_effect();
  ASSERT_TRUE(is<lifecycle::RequestPin>(front));
  EXPECT_TRUE(std::get<lifecycle::RequestPin>(*front).incorrect);
  local.set_pin_prompt_open(false);
  EXPECT_TRUE(
      local.owner()->on_message(7, remote_play::CtrlLoginPinIncorrect{}));
  EXPECT_TRUE(local.run_next_pin_reply(pin).value());
  while (local.pin_pending()) ASSERT_TRUE(local.pump_pin_outbound());
  EXPECT_FALSE(local.take_effect());
  EXPECT_TRUE(local.owner()->stop(7));
  EXPECT_TRUE(local.run_next_cleanup().value());
}

TEST_F(LocalSessionIdOverOwner,
       IssuesOneReadyTicketBindingTheNonceToTheAttempt) {
  CtrlOwnerScript script;
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(7));
  auto local = local_session(std::move(started), 7, std::byte{0x42});
  EXPECT_FALSE(local.take_ready_ticket());
  script.incoming = session_id_frame();
  EXPECT_TRUE(local.pump_once().value());
  const auto ticket = local.take_ready_ticket();
  ASSERT_TRUE(ticket);
  EXPECT_EQ(ticket->session_id.view(), "AAAAAAAAAAAAAAAAAAAAAAAA");
  EXPECT_EQ(ticket->nonce[0], std::byte{0x42});
  EXPECT_EQ(ticket->attempt_id, 7U);
  EXPECT_FALSE(local.take_ready_ticket());
  EXPECT_FALSE(local.take_session_id());
  EXPECT_FALSE(local.take_nonce());
  ASSERT_TRUE(local.owner());
  EXPECT_TRUE(local.owner()->stop(7));
  EXPECT_TRUE(local.run_next_cleanup().value());
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(LocalSessionIdOverOwner,
       IssuesNoTicketForAnotherAttemptAndKeepsTheNonce) {
  CtrlOwnerScript script;
  script.incoming = session_id_frame();
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(8));
  auto stale = local_session(std::move(started), 9, std::byte{0x71});
  EXPECT_TRUE(stale.pump_once().value());
  EXPECT_FALSE(stale.take_ready_ticket());
  const auto retained = stale.take_nonce();
  ASSERT_TRUE(retained);
  EXPECT_EQ((*retained)[0], std::byte{0x71});
  ASSERT_TRUE(stale.owner());
  EXPECT_TRUE(stale.owner()->stop(8));
  EXPECT_TRUE(stale.owner()->run_cleanup(8));
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(LocalSessionIdOverOwner, KeepsTheCleanupWhenTheReaderFails) {
  CtrlOwnerScript script;
  script.disconnect = true;
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(8));
  auto local = local_session(std::move(started), 8);
  const auto failed = local.pump_once();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), remote_play::CtrlReaderError::disconnected);
  EXPECT_TRUE(is<lifecycle::Cleanup>(local.take_effect()));
  ASSERT_TRUE(local.owner());
  EXPECT_TRUE(local.owner()->run_cleanup(8));
  EXPECT_EQ(script.closes, 1U);
}

TEST_F(LocalSessionIdOverOwner, DoesNotCaptureTheSessionIdOfAnotherAttempt) {
  CtrlOwnerScript script;
  script.incoming = session_id_frame();
  auto started = owner(script, crypto);
  ASSERT_NE(started, nullptr);
  EXPECT_TRUE(started->start(7));
  EXPECT_TRUE(started->on_message(7, remote_play::CtrlHeartbeatRequest{true}));
  auto local = local_session(std::move(started), 8);
  EXPECT_TRUE(local.pump_once().value());
  EXPECT_FALSE(local.take_session_id());
  EXPECT_FALSE(local.run_next_heartbeat().value());
  EXPECT_TRUE(is<lifecycle::SendHeartbeatReply>(local.take_effect()));
  const auto effect = local.take_effect();
  ASSERT_TRUE(is<lifecycle::PublishSessionId>(effect));
  EXPECT_EQ(std::get<lifecycle::PublishSessionId>(*effect).attempt_id, 7U);
  ASSERT_TRUE(local.owner());
  EXPECT_TRUE(local.owner()->stop(7));
  EXPECT_TRUE(local.owner()->run_cleanup(7));
  EXPECT_EQ(script.closes, 1U);
}

}  // namespace
}  // namespace remote_play_test
