#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/controllers_rig.hpp"

#include "libchiaking/detail/play/access_code.hpp"
#include "src/core/net/protocol/remote_play/stream/controller.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace play_test {
namespace {

using libchiaking::net::protocol::remote_play::Pad;
using libchiaking::play::AccessCode;
using libchiaking::play::InputRefusal;
using Core = ControllersRig::Core;
using Rig = ControllersRig;

TEST(ControllerPads, AreFourAndValidByConstruction) {
  EXPECT_EQ(Pad::count, 4U);
  EXPECT_FALSE(Pad::create(4));
  ASSERT_TRUE(Pad::create(3));
  EXPECT_EQ(Pad::create(3)->index(), 3U);
  EXPECT_FALSE(Pad::host().couch());
  ASSERT_TRUE(Pad::create(1));
  EXPECT_TRUE(Pad::create(1)->couch());
}

TEST(ControllerAccessCodes, AreFourDigitsAndValidByConstruction) {
  EXPECT_EQ(AccessCode::digits, 4U);
  EXPECT_FALSE(AccessCode::parse("12x4"));
  EXPECT_FALSE(AccessCode::parse("123"));
  EXPECT_FALSE(AccessCode::parse("12345"));
  const auto code = AccessCode::parse("4567");
  ASSERT_TRUE(code);
  EXPECT_EQ(code->text(), "4567");
  EXPECT_EQ(code->bytes().size(), 4U);
  EXPECT_EQ(code->clone().text(), "4567");
}

TEST(ControllerSeats, AreFourAndRefuseUnknownButtons) {
  Rig rig;
  rig.start();
  EXPECT_FALSE(Rig::SeatNumber::create(4));
  const auto bad_buttons = rig.queue().send(Rig::host, Rig::sample(0x10000));
  ASSERT_FALSE(bad_buttons);
  EXPECT_EQ(bad_buttons.error(), InputRefusal::invalid);
}

TEST(ControllerSeats, EachHaveTheirOwnQueueThatAGuestCanTakeOver) {
  Rig rig;
  rig.start();
  auto& queue = rig.queue();
  EXPECT_TRUE(queue.send(Rig::host, Rig::sample(1)));
  EXPECT_TRUE(queue.send(Rig::seat(2), Rig::sample(8)));
  EXPECT_EQ(queue.pending(Rig::host), 1U);
  EXPECT_EQ(queue.pending(Rig::seat(2)), 1U);
  EXPECT_TRUE(queue.flush());
  ASSERT_EQ(rig.session().controllers.size(), 2U);
  EXPECT_EQ(rig.session().controllers[0].buttons, 1U);
  EXPECT_EQ(rig.session().controllers[1].controller_id, 2U);

  const auto pad = Rig::seat(2);
  EXPECT_FALSE(queue.divert(Rig::host));
  EXPECT_TRUE(queue.send(pad, Rig::sample(4)));
  EXPECT_TRUE(queue.divert(pad));
  EXPECT_FALSE(queue.divert(pad));
  EXPECT_TRUE(queue.diverted(pad));
  EXPECT_EQ(queue.pending(pad), 0U);
  const auto taken = queue.send(pad, Rig::sample(8));
  ASSERT_FALSE(taken);
  EXPECT_EQ(taken.error(), InputRefusal::not_owner);
  EXPECT_TRUE(queue.forward(pad, Rig::sample(16)));
  ASSERT_EQ(rig.session().controllers.size(), 3U);
  EXPECT_EQ(rig.session().controllers[2].buttons, 16U);
  EXPECT_EQ(rig.logged("pad 2 guest input accepted by the session"), 1U);
  const auto not_claimed = queue.forward(Rig::seat(1), Rig::sample(16));
  const auto first = queue.forward(Rig::host, Rig::sample(16));
  ASSERT_FALSE(not_claimed);
  EXPECT_EQ(not_claimed.error(), InputRefusal::not_owner);
  ASSERT_FALSE(first);
  EXPECT_EQ(first.error(), InputRefusal::invalid);
  queue.restore(pad);
  EXPECT_FALSE(queue.diverted(pad));
  EXPECT_TRUE(queue.send(pad, Rig::sample(8)));
}

TEST(ControllerSeats, RefuseInputOnceTheSessionIsCancelled) {
  Rig rig;
  rig.start();
  rig.lifecycle().cancel();
  const auto cancelled = rig.queue().send(Rig::host, Rig::sample(1));
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error(), InputRefusal::not_running);
}

TEST(ControllerSeats, BecomeTheControllerIdOnTheWire) {
  libchiaking::controllers::ControllerState state;
  state.buttons = 0x1234;
  state.l2 = 7;
  state.touches[0] = {5, 100, 200};
  state.gyro_x = 0.5F;
  libchiaking::controllers::ControllerInput input{state};
  input.set_idle();
  EXPECT_EQ(input.state().buttons, 0U);
  EXPECT_EQ(input.next_touch_id(), 0);
  for (std::uint8_t index = 0; index < Rig::SeatNumber::count; ++index) {
    SCOPED_TRACE(+index);
    const auto seat = Rig::SeatNumber::masked(index);
    const auto wired = Core::wire(seat, state);
    EXPECT_EQ(wired.controller_id, index);
    EXPECT_EQ(wired.buttons, 0x1234U);
    EXPECT_EQ(wired.l2, 7);
    EXPECT_EQ(wired.touches[0].id, 5);
    EXPECT_EQ(wired.touches[0].x, 100);
    EXPECT_EQ(wired.touches[0].y, 200);
    EXPECT_EQ(Core::wire(seat, input.state()).controller_id, index);
    const auto merged =
        libchiaking::controllers::ControllerInput{state}.merged(input);
    EXPECT_EQ(Core::wire(seat, merged.state()).controller_id, index);
    const auto typed = Core::typed(wired);
    EXPECT_EQ(typed.buttons, state.buttons);
    EXPECT_EQ(typed.l2, state.l2);
    EXPECT_EQ(typed.gyro_x, state.gyro_x);
  }
}

}  // namespace
}  // namespace play_test
