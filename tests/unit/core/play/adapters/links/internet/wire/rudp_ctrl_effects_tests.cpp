#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"
#include "src/core/play/adapters/links/internet/wire/remote_ctrl_effects.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <utility>
#include <variant>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace lifecycle = remote_play::ctrl_lifecycle_detail;
using rudp_ctrl::SpyCrypto;
using rudp_ctrl::StubChannel;
using Progress = internet::remote_ctrl::EffectProgress;

TEST(RemoteCtrlEffects, ReportsAPinRequestOnceAndKeepsAnsweringHeartbeats) {
  constexpr auto target = remote_play::RegistrationTarget::ps5_1;
  SpyCrypto crypto;
  StubChannel channel;
  internet::remote_ctrl::StartupResult startup{};
  startup.final.next_local_counter = 0x2000;
  auto waiting = internet::remote_ctrl::Session::create(std::move(startup), 5);
  ASSERT_TRUE(waiting);
  remote_play::RudpCtrlMessageResult pin_request{};
  pin_request.message = remote_play::CtrlLoginPinRequest{};
  ASSERT_TRUE(
      waiting.value()->inbox().accept({std::move(pin_request), 5, false}));
  auto pin_notified = false;
  const auto first = internet::remote_ctrl::Effects::step(
      *waiting.value(), channel, crypto, target, pin_notified);
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value(), Progress::pin_required);
  pin_notified = true;
  remote_play::RudpCtrlMessageResult heartbeat_during_pin{};
  heartbeat_during_pin.message = remote_play::CtrlHeartbeatRequest{};
  ASSERT_TRUE(waiting.value()->inbox().accept(
      {std::move(heartbeat_during_pin), 5, false}));
  const auto sends_before = channel.sends();
  const auto answered = internet::remote_ctrl::Effects::step(
      *waiting.value(), channel, crypto, target, pin_notified);
  ASSERT_TRUE(answered);
  EXPECT_EQ(answered.value(), Progress::idle);
  EXPECT_EQ(channel.sends(), sends_before + 1);
  const auto* still_pin = waiting.value()->inbox().front_effect();
  ASSERT_NE(still_pin, nullptr);
  EXPECT_TRUE(std::holds_alternative<lifecycle::RequestPin>(*still_pin));
}

}  // namespace
}  // namespace play_test
