#include "src/core/net/protocol/remote_play/ctrl_lifecycle.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace ctrl = remote_play::ctrl_lifecycle_detail;
using remote_play::CtrlMessage;

static_assert(!std::is_copy_constructible_v<remote_play::SessionId>);

class CtrlLifecycle : public ::testing::Test {
 protected:
  static constexpr std::uint64_t attempt = 7;

  template <class T>
  static auto count(const ctrl::Context& of) -> std::size_t {
    std::size_t result = 0;
    for (const auto& effect : of.effects)
      result += std::holds_alternative<T>(effect) ? 1U : 0U;
    return result;
  }

  template <class T>
  auto count() const -> std::size_t {
    return count<T>(context);
  }

  static auto valid_id() -> CtrlMessage {
    constexpr char raw[] = "Jabcdefghijklmnopqrstuvwx";
    return remote_play::CtrlCodec::decode_message(
        0x33, {reinterpret_cast<const std::byte*>(raw), sizeof(raw) - 1});
  }

  void receive(CtrlMessage& message) {
    EXPECT_TRUE(machine.process(ctrl::Received{attempt, message}));
  }

  void receive(CtrlMessage&& message) { receive(message); }

  auto last_pin_was_incorrect() const -> bool {
    return std::get<ctrl::RequestPin>(context.effects.back()).incorrect;
  }

  ctrl::Context context;
  ctrl::Lifecycle machine{context};
};

TEST_F(CtrlLifecycle, PublishesTheSessionIdOnce) {
  EXPECT_EQ(machine.state(), ctrl::State::idle);
  ASSERT_TRUE(machine.process(ctrl::Start{attempt}));
  EXPECT_EQ(machine.state(), ctrl::State::waiting_id);
  receive(valid_id());
  EXPECT_EQ(machine.state(), ctrl::State::active);
  EXPECT_EQ(count<ctrl::PublishSessionId>(), 1U);
  ASSERT_FALSE(context.effects.empty());
  EXPECT_EQ(
      std::get<ctrl::PublishSessionId>(context.effects.front()).value.view(),
      "abcdefghijklmnopqrstuvwx");
  receive(valid_id());
  EXPECT_EQ(count<ctrl::PublishSessionId>(), 1U);
}

TEST_F(CtrlLifecycle, FallsBackOnAnInvalidSessionIdAndKeepsWaiting) {
  ASSERT_TRUE(machine.process(ctrl::Start{attempt}));
  receive(remote_play::CtrlCodec::decode_message(0x33, {}));
  EXPECT_EQ(machine.state(), ctrl::State::waiting_id);
  EXPECT_EQ(count<ctrl::PublishSessionIdFallback>(), 1U);
  receive(valid_id());
  EXPECT_EQ(machine.state(), ctrl::State::active);
  EXPECT_EQ(count<ctrl::PublishSessionId>(), 1U);
}

TEST_F(CtrlLifecycle, HandlesHeartbeatsPinLoginAndOneSwitchAck) {
  ASSERT_TRUE(machine.process(ctrl::Start{attempt}));
  CtrlMessage heartbeat = remote_play::CtrlHeartbeatRequest{true};
  receive(heartbeat);
  EXPECT_EQ(machine.state(), ctrl::State::waiting_id);
  receive(remote_play::CtrlLoginPinRequest{true});
  EXPECT_TRUE(context.pin_requested);
  EXPECT_FALSE(last_pin_was_incorrect());
  CtrlMessage incorrect = remote_play::CtrlLoginPinIncorrect{};
  receive(incorrect);
  EXPECT_EQ(count<ctrl::RequestPin>(), 2U);
  EXPECT_TRUE(last_pin_was_incorrect());
  receive(remote_play::CtrlLoginSuccess{});
  EXPECT_FALSE(context.pin_requested);
  receive(incorrect);
  EXPECT_EQ(count<ctrl::RequestPin>(), 2U);
  CtrlMessage switch_ack = remote_play::CtrlStreamSwitchAck{true};
  receive(switch_ack);
  receive(switch_ack);
  EXPECT_EQ(count<ctrl::PublishSwitchAck>(), 1U);
  receive(valid_id());
  receive(heartbeat);
  EXPECT_EQ(count<ctrl::SendHeartbeatReply>(), 2U);
  receive(switch_ack);
  EXPECT_EQ(count<ctrl::PublishSwitchAck>(), 1U);
  receive(remote_play::CtrlCodec::decode_message(0x9999, {}));
  EXPECT_EQ(machine.state(), ctrl::State::active);
}

TEST_F(CtrlLifecycle, APinRequestAfterTheIdFlagsCtrlFailedButKeepsCtrl) {
  ASSERT_TRUE(machine.process(ctrl::Start{attempt}));
  receive(valid_id());
  receive(remote_play::CtrlLoginPinRequest{});
  EXPECT_EQ(machine.state(), ctrl::State::active);
  EXPECT_EQ(count<ctrl::Cleanup>(), 0U);
  EXPECT_EQ(count<ctrl::PublishCtrlFailed>(), 1U);
  receive(remote_play::CtrlHeartbeatRequest{});
  EXPECT_EQ(count<ctrl::SendHeartbeatReply>(), 1U);
  EXPECT_TRUE(machine.process(ctrl::Stop{attempt}));
  EXPECT_EQ(machine.state(), ctrl::State::closing);
  EXPECT_TRUE(machine.process(ctrl::Closed{attempt}));
  EXPECT_EQ(machine.state(), ctrl::State::stopped);
}

TEST_F(CtrlLifecycle, APinRequestEffectReportsOnlyItsSourceMessage) {
  ASSERT_TRUE(machine.process(ctrl::Start{attempt}));
  CtrlMessage pin = remote_play::CtrlLoginPinRequest{};
  receive(pin);
  EXPECT_FALSE(last_pin_was_incorrect());
  receive(pin);
  EXPECT_FALSE(last_pin_was_incorrect());
  CtrlMessage incorrect = remote_play::CtrlLoginPinIncorrect{};
  receive(incorrect);
  EXPECT_TRUE(last_pin_was_incorrect());
  receive(pin);
  EXPECT_FALSE(last_pin_was_incorrect());
  receive(remote_play::CtrlLoginSuccess{});
  receive(incorrect);
  receive(pin);
  EXPECT_FALSE(last_pin_was_incorrect());
  EXPECT_EQ(count<ctrl::RequestPin>(), 5U);
}

TEST_F(CtrlLifecycle,
       AnIncorrectPinAfterTheIdStillRequestsAPinWhileOneIsAsked) {
  ASSERT_TRUE(machine.process(ctrl::Start{attempt}));
  receive(remote_play::CtrlLoginPinRequest{});
  receive(valid_id());
  receive(remote_play::CtrlLoginPinIncorrect{});
  EXPECT_EQ(machine.state(), ctrl::State::active);
  EXPECT_EQ(count<ctrl::RequestPin>(), 2U);
  EXPECT_TRUE(last_pin_was_incorrect());
}

TEST_F(CtrlLifecycle, CountsUnexpectedAndStaleEvents) {
  EXPECT_FALSE(machine.process(ctrl::Start{0}));
  EXPECT_EQ(context.unexpected_events, 1U);
  CtrlMessage heartbeat = remote_play::CtrlHeartbeatRequest{};
  EXPECT_FALSE(machine.process(ctrl::Received{attempt, heartbeat}));
  EXPECT_EQ(context.stale_events, 1U);
  ASSERT_TRUE(machine.process(ctrl::Start{attempt}));
  EXPECT_FALSE(machine.process(ctrl::Received{attempt + 1, heartbeat}));
  EXPECT_EQ(context.stale_events, 2U);
}

TEST_F(CtrlLifecycle, AFailureWhileClosingEndsFailedWithOneCleanup) {
  ASSERT_TRUE(machine.process(ctrl::Start{attempt}));
  EXPECT_TRUE(machine.process(ctrl::Stop{attempt}));
  EXPECT_EQ(machine.state(), ctrl::State::closing);
  EXPECT_TRUE(
      machine.process(ctrl::Failure{attempt, ctrl::FailureReason::transport}));
  EXPECT_TRUE(machine.process(ctrl::Closed{attempt, false}));
  EXPECT_EQ(machine.state(), ctrl::State::failed);
  EXPECT_EQ(count<ctrl::Cleanup>(), 1U);
  EXPECT_EQ(count<ctrl::PublishFailed>(), 1U);
  EXPECT_EQ(count<ctrl::PublishStopped>(), 0U);
  const auto stale_before = context.stale_events;
  EXPECT_FALSE(machine.process(ctrl::Closed{attempt}));
  EXPECT_EQ(context.stale_events, stale_before + 1);
}

TEST_F(CtrlLifecycle, KeepsTheFirstFailureReason) {
  ASSERT_TRUE(machine.process(ctrl::Start{attempt}));
  EXPECT_TRUE(
      machine.process(ctrl::Failure{attempt, ctrl::FailureReason::protocol}));
  EXPECT_TRUE(
      machine.process(ctrl::Failure{attempt, ctrl::FailureReason::transport}));
  EXPECT_TRUE(machine.process(ctrl::Closed{attempt, true}));
  EXPECT_EQ(machine.state(), ctrl::State::failed);
  EXPECT_EQ(count<ctrl::Cleanup>(), 1U);
  ASSERT_FALSE(context.effects.empty());
  EXPECT_EQ(std::get<ctrl::PublishFailed>(context.effects.back()).reason,
            ctrl::FailureReason::protocol);
}

TEST_F(CtrlLifecycle, ACleanStopPublishesStopped) {
  ASSERT_TRUE(machine.process(ctrl::Start{attempt}));
  EXPECT_TRUE(machine.process(ctrl::Stop{attempt}));
  EXPECT_TRUE(machine.process(ctrl::Closed{attempt}));
  EXPECT_EQ(machine.state(), ctrl::State::stopped);
  EXPECT_EQ(count<ctrl::PublishStopped>(), 1U);
}

TEST_F(CtrlLifecycle, PublishesNoticesBeforeAndAfterTheSessionId) {
  ASSERT_TRUE(machine.process(ctrl::Start{attempt}));
  const std::vector<std::byte> pad{std::byte{2}};
  auto message = remote_play::CtrlCodec::decode_message(0xa, pad);
  EXPECT_TRUE(std::holds_alternative<remote_play::CtrlNoticeMessage>(message));
  EXPECT_TRUE(machine.process(ctrl::Received{attempt, message}));
  ASSERT_EQ(context.effects.size(), 1U);
  const auto* notice =
      std::get_if<ctrl::PublishNotice>(&context.effects.front());
  ASSERT_NE(notice, nullptr);
  EXPECT_EQ(notice->attempt_id, attempt);
  EXPECT_EQ(std::get<remote_play::CtrlCouchPadDropped>(notice->notice).pad, 2);
  EXPECT_EQ(machine.state(), ctrl::State::waiting_id);
  context.effects.clear();

  std::vector<std::byte> id{std::byte{0x4a}};
  id.insert(id.end(), 24, std::byte{'A'});
  auto session_id = remote_play::CtrlCodec::decode_message(0x33, id);
  EXPECT_TRUE(machine.process(ctrl::Received{attempt, session_id}));
  context.effects.clear();
  auto duplicate = remote_play::CtrlCodec::decode_message(0x33, id);
  EXPECT_TRUE(machine.process(ctrl::Received{attempt, duplicate}));
  const std::vector<std::byte> one{std::byte{1}};
  auto invalid = remote_play::CtrlCodec::decode_message(0x33, one);
  EXPECT_TRUE(machine.process(ctrl::Received{attempt, invalid}));
  ASSERT_EQ(context.effects.size(), 2U);
  EXPECT_TRUE(
      std::holds_alternative<ctrl::RepeatPostIdFeatures>(context.effects[0]));
  EXPECT_TRUE(
      std::holds_alternative<ctrl::RepeatPostIdFeatures>(context.effects[1]));
  context.effects.clear();
  const std::vector<std::byte> display_payload{std::byte{0}, std::byte{0}};
  auto display = remote_play::CtrlCodec::decode_message(0x16, display_payload);
  EXPECT_TRUE(machine.process(ctrl::Received{attempt, display}));
  ASSERT_EQ(context.effects.size(), 1U);
  EXPECT_TRUE(
      std::holds_alternative<ctrl::PublishNotice>(context.effects.front()));
  EXPECT_EQ(machine.state(), ctrl::State::active);
}

}  // namespace
}  // namespace remote_play_test
