#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_case.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

namespace play_test {

using test_support::play::SessionRig;
namespace {

namespace play = libchiaking::play;
namespace controllers = libchiaking::controllers;
using controllers::ControllerButton;
using controllers::ControllerState;
using controllers::SeatNumber;
using play::InputRefusal;

class PlaySessionControllers : public SessionCase {
 protected:
  static auto pressed() -> ControllerState {
    ControllerState state;
    state.buttons = static_cast<std::uint32_t>(ControllerButton::cross);
    state.left_x = -200;
    state.touches[0] = {3, 10, 20};
    state.gyro_z = 0.5F;
    return state;
  }

  static auto holding(ControllerButton button) -> ControllerState {
    ControllerState state;
    state.buttons = static_cast<std::uint32_t>(button);
    return state;
  }
};

TEST_F(PlaySessionControllers, RefuseSamplesBeforeTheSessionRuns) {
  SessionRig rig;
  EXPECT_EQ(
      rig.session->controllers().send(SeatNumber::first(), pressed()).error(),
      InputRefusal::not_running);
}

TEST_F(PlaySessionControllers, SendSamplesToTheLinkBySeat) {
  SessionRig rig;
  rig.start_playing();
  const auto state = pressed();
  EXPECT_TRUE(rig.session->controllers().send(SeatNumber::first(), state));
  auto second = state;
  second.buttons = static_cast<std::uint32_t>(ControllerButton::moon);
  EXPECT_TRUE(rig.session->controllers().send(seat(2), second));
  SessionRig::eventually([&] {
    return rig.link.controller_samples(SeatNumber::first()).size() == 1 &&
           rig.link.controller_samples(seat(2)).size() == 1;
  });
  const auto first = rig.link.controller_samples(SeatNumber::first()).front();
  EXPECT_EQ(first.buttons, state.buttons);
  EXPECT_EQ(first.left_x, -200);
  EXPECT_EQ(first.touches[0].id, 3);
  EXPECT_EQ(first.touches[0].x, 10);
  EXPECT_EQ(first.touches[0].y, 20);
  EXPECT_EQ(first.gyro_z, 0.5F);
  EXPECT_EQ(first.accel_y, 1.0F);
  EXPECT_EQ(rig.link.controller_samples(seat(2)).front().buttons,
            second.buttons);
  EXPECT_TRUE(rig.link.controller_samples(seat(1)).empty());
  EXPECT_EQ(rig.link.count("controller"), 2U);
  EXPECT_EQ(rig.session->controllers().pending(), 0U);
  EXPECT_EQ(rig.session->controllers().pending(seat(2)), 0U);
}

TEST_F(PlaySessionControllers, RefuseAnInvalidSample) {
  SessionRig rig;
  rig.start_playing();
  ControllerState invalid;
  invalid.buttons = ControllerState::button_mask + 1;
  EXPECT_EQ(
      rig.session->controllers().send(SeatNumber::first(), invalid).error(),
      InputRefusal::invalid);
}

TEST_F(PlaySessionControllers, RefuseSamplesOnceTheSessionEnded) {
  SessionRig rig;
  rig.start_playing();
  EXPECT_TRUE(rig.session->leave());
  rig.session->wait_until_ended();
  EXPECT_EQ(
      rig.session->controllers().send(SeatNumber::first(), pressed()).error(),
      InputRefusal::not_running);
}

TEST_F(PlaySessionControllers, QueueSixtyFourSamplesByDefault) {
  EXPECT_EQ(play::Controllers::default_depth, 64U);
}

TEST_F(PlaySessionControllers, DiscardTheQueuedInputOfOneSeat) {
  SessionRig rig;
  rig.start_playing();
  auto& controllers = rig.session->controllers();
  std::atomic<bool> checked{false};
  rig.link.then([&](play::testing::ScriptedLink&) {
    for (const auto button : {ControllerButton::cross, ControllerButton::moon,
                              ControllerButton::box}) {
      EXPECT_TRUE(controllers.send(seat(1), holding(button)));
      EXPECT_TRUE(controllers.send(seat(2), holding(button)));
    }
    EXPECT_EQ(controllers.pending(seat(1)), 3U);
    EXPECT_EQ(controllers.pending(seat(2)), 3U);
    EXPECT_EQ(controllers.pending(), 6U);
    controllers.discard_pending(seat(1));
    EXPECT_EQ(controllers.pending(seat(1)), 0U);
    EXPECT_EQ(controllers.pending(seat(2)), 3U);
    EXPECT_EQ(controllers.pending(), 3U);
    controllers.discard_pending(seat(3));
    EXPECT_EQ(controllers.pending(), 3U);
    const ControllerState released;
    EXPECT_TRUE(controllers.send(seat(1), released));
    checked = true;
  });
  SessionRig::eventually([&] {
    return checked.load() && rig.link.controller_samples(seat(2)).size() == 3 &&
           rig.link.controller_samples(seat(1)).size() == 1;
  });
  EXPECT_EQ(rig.link.controller_samples(seat(1)).front().buttons, 0U);
  EXPECT_EQ(rig.link.controller_samples(seat(2)).back().buttons,
            static_cast<std::uint32_t>(ControllerButton::box));
  EXPECT_EQ(rig.link.count("controller"), 4U);
  EXPECT_EQ(controllers.pending(), 0U);
  EXPECT_TRUE(rig.session->leave());
  rig.session->wait_until_ended();
  controllers.discard_pending(seat(2));
  EXPECT_EQ(controllers.pending(), 0U);
}

}  // namespace
}  // namespace play_test
