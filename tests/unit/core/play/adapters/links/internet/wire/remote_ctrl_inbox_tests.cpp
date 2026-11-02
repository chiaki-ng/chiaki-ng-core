#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/remote_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace lifecycle = remote_play::ctrl_lifecycle_detail;
using remote_ctrl::CtrlMessages;
using State = lifecycle::State;

class RemoteCtrlInbox : public ::testing::Test {
 protected:
  static auto inbox_with(std::byte first, std::uint64_t attempt)
      -> internet::remote_ctrl::Inbox {
    libchiaking::SecretArray<16> nonce;
    nonce[0] = first;
    return internet::remote_ctrl::Inbox(std::move(nonce), attempt);
  }

  static auto step(remote_play::CtrlMessage message, std::uint64_t attempt) {
    return CtrlMessages::step(std::move(message), attempt);
  }

  static auto session_id(std::uint64_t attempt) {
    return step(remote_play::CtrlSessionId{CtrlMessages::parsed_id()}, attempt);
  }

  template <class Alternative>
  static auto holds(const auto& effect) -> testing::AssertionResult {
    if (!effect) return testing::AssertionFailure() << "no effect";
    if (!std::holds_alternative<Alternative>(*effect))
      return testing::AssertionFailure() << "another effect";
    return testing::AssertionSuccess();
  }

  void activate() {
    ASSERT_TRUE(inbox_.start());
    ASSERT_TRUE(inbox_.accept(session_id(7)));
    ASSERT_TRUE(inbox_.take_ready_ticket());
  }

  internet::remote_ctrl::Inbox inbox_ = inbox_with(std::byte{0x44}, 7);
  internet::remote_ctrl::Inbox pin_inbox_ = inbox_with(std::byte{}, 9);
};

TEST_F(RemoteCtrlInbox, CoalescesPinRequestsUntilThePromptOpens) {
  ASSERT_TRUE(pin_inbox_.start());
  for (int i = 0; i < 3; ++i)
    EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinRequest{}, 9)));
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlHeartbeatRequest{}, 9)));
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinIncorrect{}, 9)));
  pin_inbox_.coalesce_pin_requests();
  const auto first = pin_inbox_.take_effect();
  ASSERT_TRUE(holds<lifecycle::RequestPin>(first));
  EXPECT_FALSE(std::get<lifecycle::RequestPin>(*first).incorrect);
  EXPECT_TRUE(holds<lifecycle::SendHeartbeatReply>(pin_inbox_.take_effect()));
  EXPECT_FALSE(pin_inbox_.take_effect());
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinRequest{}, 9)));
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinRequest{}, 9)));
  pin_inbox_.coalesce_pin_requests();
  const auto again = pin_inbox_.take_effect();
  ASSERT_TRUE(holds<lifecycle::RequestPin>(again));
  EXPECT_FALSE(std::get<lifecycle::RequestPin>(*again).incorrect);
  EXPECT_FALSE(pin_inbox_.take_effect());
}

TEST_F(RemoteCtrlInbox, DoesNotPromptForPinRequestsAfterTheSessionId) {
  ASSERT_TRUE(pin_inbox_.start());
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinRequest{}, 9)));
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinIncorrect{}, 9)));
  EXPECT_TRUE(pin_inbox_.accept(session_id(9)));
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinIncorrect{}, 9)));
  EXPECT_TRUE(pin_inbox_.take_ready_ticket());
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinIncorrect{}, 9)));
  const auto first = pin_inbox_.take_effect();
  ASSERT_TRUE(holds<lifecycle::RequestPin>(first));
  EXPECT_FALSE(std::get<lifecycle::RequestPin>(*first).incorrect);
  const auto second = pin_inbox_.take_effect();
  ASSERT_TRUE(holds<lifecycle::RequestPin>(second));
  EXPECT_TRUE(std::get<lifecycle::RequestPin>(*second).incorrect);
  EXPECT_FALSE(pin_inbox_.take_effect());
  EXPECT_EQ(pin_inbox_.state(), State::active);
}

TEST_F(RemoteCtrlInbox, FoldsPinRequestsAfterTheSessionIdIntoAnOpenPrompt) {
  ASSERT_TRUE(pin_inbox_.start());
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinRequest{}, 9)));
  pin_inbox_.set_pin_prompt_open(true);
  EXPECT_TRUE(pin_inbox_.accept(session_id(9)));
  EXPECT_TRUE(pin_inbox_.take_ready_ticket());
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinIncorrect{}, 9)));
  pin_inbox_.set_pin_prompt_open(false);
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinIncorrect{}, 9)));
  const auto first = pin_inbox_.take_effect();
  ASSERT_TRUE(holds<lifecycle::RequestPin>(first));
  EXPECT_FALSE(std::get<lifecycle::RequestPin>(*first).incorrect);
  const auto second = pin_inbox_.take_effect();
  ASSERT_TRUE(holds<lifecycle::RequestPin>(second));
  EXPECT_TRUE(std::get<lifecycle::RequestPin>(*second).incorrect);
  EXPECT_FALSE(pin_inbox_.take_effect());
}

TEST_F(RemoteCtrlInbox, DoesNotPromptForPinRequestsAfterTheCtrlWait) {
  ASSERT_TRUE(pin_inbox_.start());
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinRequest{}, 9)));
  pin_inbox_.close_pin_window();
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinRequest{}, 9)));
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlHeartbeatRequest{}, 9)));
  EXPECT_TRUE(holds<lifecycle::SendHeartbeatReply>(pin_inbox_.take_effect()));
  EXPECT_FALSE(pin_inbox_.take_effect());
}

TEST_F(RemoteCtrlInbox, KeepsAnOpenPromptWhenTheCtrlWaitEnds) {
  ASSERT_TRUE(pin_inbox_.start());
  EXPECT_TRUE(pin_inbox_.accept(step(remote_play::CtrlLoginPinRequest{}, 9)));
  pin_inbox_.set_pin_prompt_open(true);
  pin_inbox_.close_pin_window();
  EXPECT_TRUE(holds<lifecycle::RequestPin>(pin_inbox_.take_effect()));
}

TEST_F(RemoteCtrlInbox, WaitsForTheSessionIdOnceStarted) {
  EXPECT_EQ(inbox_.state(), State::idle);
  EXPECT_TRUE(inbox_.start());
  EXPECT_EQ(inbox_.state(), State::waiting_id);
  EXPECT_FALSE(inbox_.take_ready_ticket());
}

TEST_F(RemoteCtrlInbox, CountsASessionIdFromAnotherAttemptAsStale) {
  ASSERT_TRUE(inbox_.start());
  EXPECT_FALSE(inbox_.accept(session_id(6)));
  EXPECT_EQ(inbox_.stale_events(), 1U);
  EXPECT_FALSE(inbox_.take_ready_ticket());
}

TEST_F(RemoteCtrlInbox, HandsOverOneReadyTicketForTheSessionId) {
  ASSERT_TRUE(inbox_.start());
  EXPECT_TRUE(inbox_.accept(session_id(7)));
  EXPECT_EQ(inbox_.state(), State::active);
  const auto ticket = inbox_.take_ready_ticket();
  ASSERT_TRUE(ticket);
  EXPECT_EQ(ticket->attempt_id, 7U);
  EXPECT_EQ(ticket->session_id.view(), "AAAAAAAAAAAAAAAAAAAAAAAA");
  EXPECT_EQ(ticket->nonce[0], std::byte{0x44});
  EXPECT_FALSE(inbox_.take_ready_ticket());
}

TEST_F(RemoteCtrlInbox, QueuesOneReplyPerHeartbeat) {
  ASSERT_NO_FATAL_FAILURE(activate());
  EXPECT_TRUE(inbox_.accept(step(remote_play::CtrlHeartbeatRequest{}, 7)));
  EXPECT_TRUE(holds<lifecycle::SendHeartbeatReply>(inbox_.take_effect()));
  EXPECT_FALSE(inbox_.take_effect());
}

TEST_F(RemoteCtrlInbox, RefusesAHeartbeatMarkedAsASwitchNotification) {
  ASSERT_NO_FATAL_FAILURE(activate());
  auto inconsistent = step(remote_play::CtrlHeartbeatRequest{}, 7);
  inconsistent.switch_notification = true;
  EXPECT_FALSE(inbox_.accept(std::move(inconsistent)));
  EXPECT_FALSE(inbox_.take_effect());
}

TEST_F(RemoteCtrlInbox, PublishesOnlyTheFirstStreamSwitchAck) {
  ASSERT_NO_FATAL_FAILURE(activate());
  EXPECT_TRUE(inbox_.accept(step(remote_play::CtrlStreamSwitchAck{}, 7)));
  EXPECT_TRUE(holds<lifecycle::PublishSwitchAck>(inbox_.take_effect()));
  EXPECT_TRUE(inbox_.accept(step(remote_play::CtrlStreamSwitchAck{}, 7)));
  EXPECT_FALSE(inbox_.take_effect());
}

TEST_F(RemoteCtrlInbox, StopsAndClosesOnlyForItsOwnAttempt) {
  ASSERT_NO_FATAL_FAILURE(activate());
  EXPECT_FALSE(inbox_.stop(6));
  EXPECT_TRUE(inbox_.stop(7));
  EXPECT_TRUE(holds<lifecycle::Cleanup>(inbox_.take_effect()));
  EXPECT_FALSE(inbox_.take_effect());
  EXPECT_FALSE(inbox_.closed(6, true));
  EXPECT_TRUE(inbox_.closed(7, true));
  EXPECT_TRUE(holds<lifecycle::PublishStopped>(inbox_.take_effect()));
  EXPECT_EQ(inbox_.state(), State::stopped);
  EXPECT_FALSE(inbox_.closed(7, true));
}

TEST_F(RemoteCtrlInbox, CleansUpAndThenPublishesAFailure) {
  auto failed = inbox_with(std::byte{}, 8);
  ASSERT_TRUE(failed.start());
  EXPECT_TRUE(failed.fail(8, lifecycle::FailureReason::transport));
  EXPECT_TRUE(holds<lifecycle::Cleanup>(failed.take_effect()));
  EXPECT_TRUE(failed.closed(8, true));
  EXPECT_TRUE(holds<lifecycle::PublishFailed>(failed.take_effect()));
  EXPECT_EQ(failed.state(), State::failed);
}

class RemoteCtrlInboxFallback : public RemoteCtrlInbox {
 protected:
  auto publish(std::uint64_t attempt) -> bool {
    return fallback_.publish_fallback(attempt, CtrlMessages::parsed_id());
  }

  internet::remote_ctrl::Inbox fallback_ = inbox_with(std::byte{0x77}, 12);
};

TEST_F(RemoteCtrlInboxFallback, IsRefusedBeforeTheInboxStarts) {
  EXPECT_FALSE(publish(12));
  EXPECT_FALSE(fallback_.take_ready_ticket());
}

TEST_F(RemoteCtrlInboxFallback, IsRefusedForAnotherAttempt) {
  ASSERT_TRUE(fallback_.start());
  EXPECT_FALSE(publish(11));
  EXPECT_EQ(fallback_.state(), State::waiting_id);
  EXPECT_FALSE(fallback_.take_ready_ticket());
}

TEST_F(RemoteCtrlInboxFallback, ActivatesTheInboxOnceWithOneTicket) {
  ASSERT_TRUE(fallback_.start());
  EXPECT_TRUE(publish(12));
  EXPECT_EQ(fallback_.state(), State::active);
  EXPECT_FALSE(publish(12));
  const auto ticket = fallback_.take_ready_ticket();
  ASSERT_TRUE(ticket);
  EXPECT_EQ(ticket->attempt_id, 12U);
  EXPECT_EQ(ticket->session_id.view(), "AAAAAAAAAAAAAAAAAAAAAAAA");
  EXPECT_EQ(ticket->nonce[0], std::byte{0x77});
  EXPECT_FALSE(publish(12));
  EXPECT_FALSE(fallback_.take_ready_ticket());
}

TEST_F(RemoteCtrlInboxFallback, KeepsWorkingAndIsRefusedOnceStopped) {
  ASSERT_TRUE(fallback_.start());
  ASSERT_TRUE(publish(12));
  ASSERT_TRUE(fallback_.take_ready_ticket());
  EXPECT_TRUE(fallback_.accept(step(remote_play::CtrlHeartbeatRequest{}, 12)));
  EXPECT_TRUE(holds<lifecycle::SendHeartbeatReply>(fallback_.take_effect()));
  EXPECT_TRUE(fallback_.stop(12));
  EXPECT_FALSE(publish(12));
  EXPECT_TRUE(fallback_.closed(12, true));
  EXPECT_FALSE(publish(12));
}

class RemoteCtrlSessionStartup : public ::testing::Test {
 protected:
  static auto startup() -> internet::remote_ctrl::StartupResult {
    internet::remote_ctrl::StartupResult result{};
    result.prepared.nonce[0] = std::byte{0x55};
    result.final.response.next_remote_counter = 11;
    result.final.next_local_counter = 0x1234;
    result.final.remote_counter = 0x5678;
    return result;
  }
};

TEST_F(RemoteCtrlSessionStartup, RefusesAttemptZero) {
  const auto invalid_owner =
      internet::remote_ctrl::Session::create(startup(), 0);
  ASSERT_FALSE(invalid_owner);
  EXPECT_EQ(invalid_owner.error(),
            internet::remote_ctrl::SessionError::invalid_attempt);
}

TEST_F(RemoteCtrlSessionStartup, StartsFromTheStartupCounters) {
  const auto owner = internet::remote_ctrl::Session::create(startup(), 9);
  ASSERT_TRUE(owner);
  EXPECT_EQ(owner.value()->inbox().state(), State::waiting_id);
  EXPECT_EQ(owner.value()->pump_state().remote_crypt_counter, 11U);
  EXPECT_EQ(owner.value()->pump_state().local_counter, 0x1234);
  EXPECT_EQ(owner.value()->pump_state().remote_counter, 0x5678);
  EXPECT_EQ(owner.value()->outbound_state().crypt_counter, 0U);
  EXPECT_EQ(owner.value()->outbound_state().local_rudp_counter, 0x1234);
}

TEST_F(RemoteCtrlSessionStartup, HandsOverOneTicketWithTheStartupNonce) {
  const auto owner = internet::remote_ctrl::Session::create(startup(), 9);
  ASSERT_TRUE(owner);
  EXPECT_TRUE(owner.value()->inbox().accept(CtrlMessages::step(
      remote_play::CtrlSessionId{CtrlMessages::parsed_id()}, 9)));
  const auto ready = owner.value()->take_ready_ticket();
  ASSERT_TRUE(ready);
  EXPECT_EQ(ready->attempt_id, 9U);
  EXPECT_EQ(ready->nonce[0], std::byte{0x55});
  EXPECT_FALSE(owner.value()->take_ready_ticket());
}

}  // namespace
}  // namespace play_test
