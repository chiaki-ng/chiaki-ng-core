#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <variant>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
namespace lifecycle = remote_play::ctrl_lifecycle_detail;
using rudp_ctrl::SpyCrypto;
using rudp_ctrl::StubChannel;
using Effect = lifecycle::Effect;
using StepError = internet::remote_ctrl::OutboundStepError;

class PinRig : public ::testing::Test {
 protected:
  static constexpr auto target = remote_play::RegistrationTarget::ps5_1;
  static constexpr std::array<std::byte, 4> secret_pin{
      std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}};

  SpyCrypto crypto_;
  StubChannel channel_;
};

class RemoteCtrlConnectorPin : public PinRig {
 protected:
  auto send(Effect effect, std::span<const std::byte> pin) {
    return internet::remote_ctrl::Connector::send_pin(
        crypto_, target, material_, channel_, initial_, std::move(effect), pin);
  }

  void expect_nothing_sent() {
    EXPECT_EQ(crypto_.calls(), 0U);
    EXPECT_EQ(channel_.sends(), 0);
  }

 private:
  remote_play::AuthMaterial material_;
  internet::remote_ctrl::OutboundStepState initial_{9, 0x0100, 7};
};

class RemoteCtrlSessionPin : public PinRig {
 protected:
  void SetUp() override {
    internet::remote_ctrl::StartupResult startup{};
    startup.final.next_local_counter = 0x0100;
    auto created =
        internet::remote_ctrl::Session::create(std::move(startup), 7);
    ASSERT_TRUE(created);
    owner_ = std::move(created.value());
    remote_play::RudpCtrlMessageResult pin_message{};
    pin_message.message = remote_play::CtrlLoginPinRequest{};
    ASSERT_TRUE(owner_->inbox().accept({std::move(pin_message), 7, false}));
    request_ = owner_->inbox().front_effect();
    ASSERT_NE(request_, nullptr);
    ASSERT_TRUE(std::holds_alternative<lifecycle::RequestPin>(*request_));
  }

  auto send_once(std::span<const std::byte> pin) {
    return owner_->send_pin_once(channel_, crypto_, target, pin);
  }

  auto owner() -> internet::remote_ctrl::Session& { return *owner_; }
  auto request() const -> const Effect* { return request_; }

 private:
  std::unique_ptr<internet::remote_ctrl::Session> owner_;
  const Effect* request_{};
};

TEST_F(RemoteCtrlConnectorPin, SealsThePinWithTheCounterBeforeIt) {
  const auto pin_sent = send(lifecycle::RequestPin{7}, secret_pin);
  ASSERT_TRUE(pin_sent);
  EXPECT_EQ(pin_sent.value().crypt_counter, 10U);
  EXPECT_EQ(pin_sent.value().local_rudp_counter, 0x0101);
  EXPECT_EQ(crypto_.calls(), 1U);
  EXPECT_EQ(crypto_.counter().back(), std::byte{8});
  EXPECT_EQ(channel_.sends(), 1);
}

TEST_F(RemoteCtrlConnectorPin, ReportsAShortSend) {
  channel_.short_send(true);
  const auto pin_short = send(lifecycle::RequestPin{7}, secret_pin);
  ASSERT_FALSE(pin_short);
  EXPECT_EQ(pin_short.error(), StepError::short_send);
}

TEST_F(RemoteCtrlConnectorPin, RefusesAnEmptyPinWithoutSending) {
  const auto pin_empty = send(lifecycle::RequestPin{7}, {});
  ASSERT_FALSE(pin_empty);
  EXPECT_EQ(pin_empty.error(), StepError::invalid_pin);
  expect_nothing_sent();
}

TEST_F(RemoteCtrlConnectorPin, RefusesAnOversizedPinWithoutSending) {
  const std::array<std::byte, remote_play::ctrl_frame_max_payload + 1>
      oversize{};
  const auto pin_huge = send(lifecycle::RequestPin{7}, oversize);
  ASSERT_FALSE(pin_huge);
  EXPECT_EQ(pin_huge.error(), StepError::invalid_pin);
  expect_nothing_sent();
}

TEST_F(RemoteCtrlConnectorPin, RefusesARequestFromAnotherAttempt) {
  const auto pin_stale = send(lifecycle::RequestPin{8}, secret_pin);
  ASSERT_FALSE(pin_stale);
  EXPECT_EQ(pin_stale.error(), StepError::stale_attempt);
  expect_nothing_sent();
}

TEST_F(RemoteCtrlConnectorPin, RefusesAnEffectOtherThanAPinRequest) {
  const auto pin_wrong = send(lifecycle::SendHeartbeatReply{7}, secret_pin);
  ASSERT_FALSE(pin_wrong);
  EXPECT_EQ(pin_wrong.error(), StepError::unsupported_effect);
  expect_nothing_sent();
}

TEST_F(RemoteCtrlSessionPin, KeepsTheRequestQueuedForAnEmptyPin) {
  const auto invalid = send_once({});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), StepError::invalid_pin);
  EXPECT_EQ(owner().inbox().front_effect(), request());
}

TEST_F(RemoteCtrlSessionPin, KeepsTheRequestAndCountersWhenTheSendIsShort) {
  channel_.short_send(true);
  const auto pin_blocked = send_once(secret_pin);
  ASSERT_FALSE(pin_blocked);
  EXPECT_EQ(pin_blocked.error(), StepError::short_send);
  EXPECT_EQ(owner().inbox().front_effect(), request());
  EXPECT_EQ(owner().outbound_state().local_rudp_counter, 0x0100);
  EXPECT_EQ(owner().pump_state().local_counter, 0x0100);
}

TEST_F(RemoteCtrlSessionPin, SendsThePinOnTheNextTryAndClearsTheRequest) {
  const auto sends_before = channel_.sends();
  const auto calls_before = crypto_.calls();
  EXPECT_FALSE(send_once({}));
  channel_.short_send(true);
  EXPECT_FALSE(send_once(secret_pin));
  channel_.short_send(false);
  const auto pin_delivered = send_once(secret_pin);
  ASSERT_TRUE(pin_delivered);
  EXPECT_EQ(pin_delivered.value().crypt_counter, 1U);
  EXPECT_EQ(pin_delivered.value().local_rudp_counter, 0x0101);
  EXPECT_EQ(owner().pump_state().local_counter, 0x0101);
  EXPECT_FALSE(owner().inbox().front_effect());
  EXPECT_EQ(channel_.sends(), sends_before + 2);
  EXPECT_EQ(crypto_.calls(), calls_before + 2);
  const auto pin_frames = rudp::FrameCodec::decode(channel_.last());
  ASSERT_TRUE(pin_frames);
  ASSERT_EQ(pin_frames.value().size(), 1U);
  EXPECT_EQ(pin_frames.value()[0].type, 0x0230);
  const auto pin_ctrl = remote_play::CtrlCodec::decode_frame(
      std::span<const std::byte>(pin_frames.value()[0].payload).subspan(2));
  ASSERT_TRUE(pin_ctrl);
  EXPECT_EQ(pin_ctrl.value().type, 0x8004);
  EXPECT_EQ(pin_ctrl.value().payload.size(), secret_pin.size());
}

}  // namespace
}  // namespace play_test
