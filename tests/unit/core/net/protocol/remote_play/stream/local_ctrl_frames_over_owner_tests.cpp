#include "tests/unit/core/net/protocol/remote_play/ctrl_owner_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;

class LocalPostIdCtrlOverOwner : public CtrlOwnerRig {};

TEST_F(LocalPostIdCtrlOverOwner, SendsItsThreeFramesOnceInOrder) {
  CtrlOwnerScript script;
  script.send_limits = {2, std::nullopt, 100, 100, 2, std::nullopt, 100};
  auto owner_ptr = owner(script, crypto);
  ASSERT_NE(owner_ptr, nullptr);
  auto& owner = *owner_ptr;
  EXPECT_TRUE(owner.start(7));
  EXPECT_TRUE(owner.on_message(7, remote_play::CtrlSessionId{}));
  remote_play::LocalPostIdCtrl ctrl;
  EXPECT_FALSE(ctrl.pump(owner, 7).value());
  EXPECT_FALSE(ctrl.pump(owner, 7).value());
  EXPECT_FALSE(ctrl.pump(owner, 7).value());
  EXPECT_FALSE(ctrl.pump(owner, 7).value());
  EXPECT_TRUE(ctrl.pump(owner, 7).value());
  EXPECT_TRUE(ctrl.pump(owner, 7).value());
  const std::array<std::byte, 4> display_payload{};
  EXPECT_EQ(script.sent, frames({{0x36, mic_payload},
                                 {0x36, mic_payload},
                                 {0x910, display_payload}}));
  EXPECT_TRUE(owner.stop(7));
  EXPECT_TRUE(owner.take_effect());
  EXPECT_TRUE(owner.run_cleanup(7));
}

TEST_F(LocalPostIdCtrlOverOwner, PrependsFeatureAndConnectFramesForADualSense) {
  CtrlOwnerScript script;
  script.send_limits = {2, std::nullopt, 100};
  auto owner_ptr = owner(script, crypto);
  ASSERT_NE(owner_ptr, nullptr);
  auto& owner = *owner_ptr;
  EXPECT_TRUE(owner.start(7));
  EXPECT_TRUE(owner.on_message(7, remote_play::CtrlSessionId{}));
  remote_play::LocalPostIdCtrl ctrl(true);
  EXPECT_FALSE(ctrl.pump(owner, 7).value());
  EXPECT_FALSE(ctrl.pump(owner, 7).value());
  EXPECT_TRUE(ctrl.pump(owner, 7).value());
  EXPECT_TRUE(ctrl.pump(owner, 7).value());
  const std::array<std::byte, 3> features{std::byte{0}, std::byte{0x40},
                                          std::byte{0}};
  const std::array<std::byte, 16> connect{
      std::byte{0xa0}, std::byte{0xab}, std::byte{0x51}, std::byte{0xbd},
      std::byte{0xd1}, std::byte{0x7e}, std::byte{0},    std::byte{0},
      std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
      std::byte{0xff}, std::byte{0},    std::byte{0},    std::byte{0}};
  const std::array<std::byte, 4> display{};
  EXPECT_EQ(script.sent, frames({{0x13, features},
                                 {0x11, connect},
                                 {0x36, mic_payload},
                                 {0x36, mic_payload},
                                 {0x910, display}}));
  EXPECT_TRUE(owner.stop(7));
  EXPECT_TRUE(owner.take_effect());
  EXPECT_TRUE(owner.run_cleanup(7));
}

class LocalRuntimeCtrlOverOwner : public CtrlOwnerRig {};

TEST_F(LocalRuntimeCtrlOverOwner,
       SendsMicrophoneFramesInOrderWithinItsCapacity) {
  CtrlOwnerScript script;
  script.send_limits = {2, std::nullopt, 100, 100};
  auto owner_ptr = owner(script, crypto);
  ASSERT_NE(owner_ptr, nullptr);
  auto& owner = *owner_ptr;
  EXPECT_TRUE(owner.start(7));
  EXPECT_TRUE(owner.on_message(7, remote_play::CtrlSessionId{}));
  remote_play::LocalRuntimeCtrl ctrl;
  EXPECT_TRUE(ctrl.pump(owner, 7).value());
  EXPECT_TRUE(script.sent.empty());
  const auto connect = remote_play::CtrlCodec::microphone_connect_frame();
  const auto unmute = remote_play::CtrlCodec::microphone_mute_frame(false);
  EXPECT_TRUE(ctrl.queue(connect.type, connect.bytes()));
  EXPECT_TRUE(ctrl.queue(unmute.type, unmute.bytes()));
  EXPECT_FALSE(ctrl.pump(owner, 7).value());
  EXPECT_TRUE(ctrl.pending());
  EXPECT_FALSE(ctrl.pump(owner, 7).value());
  EXPECT_TRUE(ctrl.pump(owner, 7).value());
  EXPECT_FALSE(ctrl.pending());
  EXPECT_TRUE(ctrl.pump(owner, 7).value());
  const std::array<std::byte, 2> connect_payload{};
  const std::array<std::byte, 4> unmute_payload{std::byte{0}, std::byte{1},
                                                std::byte{0}, std::byte{89}};
  EXPECT_EQ(script.sent,
            frames({{0x30, connect_payload}, {0x36, unmute_payload}}));

  for (std::size_t i = 0; i < remote_play::LocalRuntimeCtrl::capacity; ++i)
    EXPECT_TRUE(ctrl.queue(connect.type, connect.bytes())) << "frame " << i;
  EXPECT_FALSE(ctrl.queue(connect.type, connect.bytes()));
  ctrl.clear();
  EXPECT_FALSE(ctrl.pending());
  EXPECT_TRUE(ctrl.queue(connect.type, connect.bytes()));
  ctrl.clear();
  EXPECT_TRUE(owner.stop(7));
  EXPECT_TRUE(owner.take_effect());
  EXPECT_TRUE(owner.run_cleanup(7));
}

}  // namespace
}  // namespace remote_play_test
