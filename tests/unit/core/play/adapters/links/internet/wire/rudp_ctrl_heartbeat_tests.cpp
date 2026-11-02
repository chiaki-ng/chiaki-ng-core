#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace lifecycle = remote_play::ctrl_lifecycle_detail;
using rudp_ctrl::SpyCrypto;
using rudp_ctrl::StubChannel;
using Effect = lifecycle::Effect;
using StepError = internet::remote_ctrl::OutboundStepError;

class HeartbeatRig : public ::testing::Test {
 protected:
  static constexpr auto target = remote_play::RegistrationTarget::ps5_1;

  static auto golden() -> std::vector<std::byte> {
    constexpr std::array<std::byte, 18> bytes{
        std::byte{0xc0}, std::byte{0x12}, std::byte{0x24}, std::byte{0x4f},
        std::byte{0x24}, std::byte{0x4f}, std::byte{0x02}, std::byte{0x30},
        std::byte{0x12}, std::byte{0x34}, std::byte{0},    std::byte{0},
        std::byte{0},    std::byte{0},    std::byte{0x01}, std::byte{0xfe},
        std::byte{0},    std::byte{0}};
    return {bytes.begin(), bytes.end()};
  }

  SpyCrypto crypto_;
  StubChannel channel_;
  remote_play::AuthMaterial material_;
};

class RemoteCtrlConnectorHeartbeat : public HeartbeatRig {
 protected:
  auto send(Effect effect) {
    return internet::remote_ctrl::Connector::send_heartbeat(
        crypto_, target, material_, channel_, initial_, std::move(effect));
  }

 private:
  internet::remote_ctrl::OutboundStepState initial_{5, 0x1234, 42};
};

class RemoteCtrlSessionHeartbeat : public HeartbeatRig {
 protected:
  void SetUp() override {
    internet::remote_ctrl::StartupResult startup{};
    startup.final.next_local_counter = 0x1234;
    auto created =
        internet::remote_ctrl::Session::create(std::move(startup), 42);
    ASSERT_TRUE(created);
    owner_ = std::move(created.value());
  }

  auto accept(remote_play::CtrlMessage message, bool switch_notification)
      -> bool {
    remote_play::RudpCtrlMessageResult decoded{};
    decoded.message = std::move(message);
    return owner_->inbox().accept(
        {std::move(decoded), 42, switch_notification});
  }

  auto send_once() {
    return owner_->send_heartbeat_once(channel_, crypto_, target);
  }

  auto owner() -> internet::remote_ctrl::Session& { return *owner_; }

 private:
  std::unique_ptr<internet::remote_ctrl::Session> owner_;
};

TEST_F(RemoteCtrlConnectorHeartbeat, SendsTheReplyInTheClear) {
  const auto sent = send(lifecycle::SendHeartbeatReply{42});
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value().crypt_counter, 5U);
  EXPECT_EQ(sent.value().local_rudp_counter, 0x1235);
  EXPECT_EQ(channel_.last(), golden());
  EXPECT_EQ(channel_.sends(), 1);
}

TEST_F(RemoteCtrlConnectorHeartbeat, CountsAShortSendAsSent) {
  channel_.short_send(true);
  const auto short_result = send(lifecycle::SendHeartbeatReply{42});
  ASSERT_TRUE(short_result);
  EXPECT_EQ(short_result.value().local_rudp_counter, 0x1235);
  EXPECT_EQ(channel_.sends(), 1);
}

TEST_F(RemoteCtrlConnectorHeartbeat, RefusesAnEffectOtherThanAReply) {
  const auto unsupported = send(lifecycle::RequestPin{42});
  ASSERT_FALSE(unsupported);
  EXPECT_EQ(unsupported.error(), StepError::unsupported_effect);
  EXPECT_EQ(channel_.sends(), 0);
}

TEST_F(RemoteCtrlConnectorHeartbeat, RefusesAReplyForAnotherAttempt) {
  const auto stale = send(lifecycle::SendHeartbeatReply{43});
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error(), StepError::stale_attempt);
  EXPECT_EQ(channel_.sends(), 0);
}

TEST_F(RemoteCtrlSessionHeartbeat, SendsEachQueuedReplyAndAdvancesItsCounter) {
  ASSERT_TRUE(accept(remote_play::CtrlHeartbeatRequest{}, false));
  const auto* queued = owner().inbox().front_effect();
  ASSERT_NE(queued, nullptr);
  EXPECT_TRUE(std::holds_alternative<lifecycle::SendHeartbeatReply>(*queued));
  channel_.short_send(true);
  const auto dropped = send_once();
  ASSERT_TRUE(dropped);
  EXPECT_EQ(dropped.value().local_rudp_counter, 0x1235);
  EXPECT_EQ(channel_.last(), golden());
  EXPECT_FALSE(owner().inbox().front_effect());
  EXPECT_EQ(owner().pump_state().local_counter, 0x1235);
  channel_.short_send(false);
  ASSERT_TRUE(accept(remote_play::CtrlHeartbeatRequest{}, false));
  const auto delivered = send_once();
  ASSERT_TRUE(delivered);
  EXPECT_EQ(delivered.value().local_rudp_counter, 0x1236);
  EXPECT_FALSE(owner().inbox().front_effect());
  EXPECT_EQ(owner().pump_state().local_counter, 0x1236);
}

TEST_F(RemoteCtrlSessionHeartbeat, LeavesAnotherEffectQueuedWithoutSending) {
  ASSERT_TRUE(accept(remote_play::CtrlStreamSwitchAck{}, true));
  const auto before = channel_.sends();
  const auto not_heartbeat = send_once();
  ASSERT_FALSE(not_heartbeat);
  EXPECT_EQ(not_heartbeat.error(), StepError::unsupported_effect);
  EXPECT_EQ(channel_.sends(), before);
  EXPECT_TRUE(owner().inbox().front_effect());
}

}  // namespace
}  // namespace play_test
