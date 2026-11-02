#include "tests/support/enum_printing.hpp"
#include "tests/support/play/session_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>

namespace play_test {

namespace play = libchiaking::play;

using test_support::play::SessionRig;
namespace {

using libchiaking::controllers::ControllerKind;
using libchiaking::controllers::SeatNumber;
using play::InputRefusal;
using play::SeatState;

class SessionGuestSeats : public ::testing::Test {
 protected:
  static auto seat(std::size_t index) -> SeatNumber {
    return SeatNumber::create(index).value();
  }
};

TEST_F(SessionGuestSeats, TakeAFreeSeatAndGiveItBack) {
  SessionRig rig;
  rig.start_playing();
  auto& rules = rig.core->seat_rules();
  EXPECT_EQ(rules.free_seat(), seat(1));
  EXPECT_TRUE(rules.seat_guest(seat(1), play::GuestTag{42},
                               ControllerKind::dualsense, std::nullopt));
  const auto held = rig.session->seats().seat(seat(1));
  EXPECT_TRUE(held.is_held_by_guest());
  ASSERT_TRUE(held.holder());
  EXPECT_EQ(held.holder()->guest(), play::GuestTag{42});
  EXPECT_EQ(held.state(), SeatState::joining);
  EXPECT_EQ(rules.free_seat(), seat(2));
  EXPECT_FALSE(rules.seat_guest(seat(1), play::GuestTag{43},
                                ControllerKind::dualsense, std::nullopt));
  EXPECT_EQ(rig.session->seats().remove_player(seat(1)).error(),
            InputRefusal::not_owner);
  libchiaking::controllers::ControllerState state;
  state.buttons = 1;
  EXPECT_EQ(rig.session->controllers().send(seat(1), state).error(),
            InputRefusal::not_owner);
  EXPECT_TRUE(rig.core->controller_rules().forward(seat(1), state));
  SessionRig::eventually(
      [&] { return rig.link.controller_samples(seat(1)).size() == 1; });

  rules.release_guest(seat(1));
  EXPECT_TRUE(rig.session->seats().seat(seat(1)).is_empty());
  EXPECT_EQ(rules.free_seat(), seat(1));
  SessionRig::eventually([&] { return rig.link.count("seat_remove") == 1; });
  EXPECT_TRUE(
      rig.session->seats().add_player(seat(1), ControllerKind::dualsense));
  EXPECT_FALSE(rules.seat_guest(SeatNumber::first(), play::GuestTag{44},
                                ControllerKind::dualsense, std::nullopt));
}

TEST_F(SessionGuestSeats, SurviveALateDropMeantForThePreviousHolder) {
  SessionRig rig;
  rig.start_playing();
  auto& seats = rig.session->seats();
  auto& rules = rig.core->seat_rules();
  EXPECT_TRUE(seats.add_player(seat(1), ControllerKind::dualsense));
  SessionRig::eventually([&] { return rig.link.count("seat_join") == 1; });
  rig.link.join(seat(1), {1, 2, 3});
  rig.events.until("seat_joined");
  EXPECT_TRUE(seats.remove_player(seat(1)));
  EXPECT_TRUE(seats.add_player(seat(1), ControllerKind::dualshock4));
  SessionRig::eventually([&] { return rig.link.count("seat_join") == 2; });

  rig.link.drop(seat(1));
  rig.link.ask_for_passcode(seat(3), false);
  rig.events.until("seat_passcode_needed");
  EXPECT_EQ(rig.events.count("seat_left"), 0U);
  const auto rejoining = seats.seat(seat(1));
  EXPECT_EQ(rejoining.state(), SeatState::joining);
  EXPECT_FALSE(rejoining.is_held_by_guest());
  rig.link.join(seat(1), {4, 5, 6});
  rig.events.until("seat_joined", 2);
  EXPECT_EQ(seats.seat(seat(1)).state(), SeatState::playing);
  rig.link.drop(seat(1));
  rig.events.until("seat_left");
  EXPECT_EQ(rig.events.left_seat(), seat(1));
  EXPECT_TRUE(seats.seat(seat(1)).is_empty());

  EXPECT_TRUE(rules.seat_guest(seat(2), play::GuestTag{7},
                               ControllerKind::dualsense, std::nullopt));
  rig.link.join(seat(2), {7, 7, 7});
  rig.events.until("seat_joined", 3);
  rules.release_guest(seat(2));
  EXPECT_TRUE(rules.seat_guest(seat(2), play::GuestTag{8},
                               ControllerKind::dualsense, std::nullopt));
  rig.link.drop(seat(2));
  rig.link.ask_for_passcode(seat(3), true);
  rig.events.until("seat_passcode_needed", 2);
  EXPECT_EQ(rig.events.count("seat_left"), 1U);
  const auto newer = seats.seat(seat(2));
  EXPECT_EQ(newer.state(), SeatState::joining);
  ASSERT_TRUE(newer.holder());
  EXPECT_EQ(newer.holder()->guest(), play::GuestTag{8});
  EXPECT_FALSE(rules.seat_guest(seat(2), play::GuestTag{9},
                                ControllerKind::dualsense, std::nullopt));
  rig.link.join(seat(2), {8, 8, 8});
  rig.events.until("seat_joined", 4);
  EXPECT_EQ(seats.seat(seat(2)).state(), SeatState::playing);
  ASSERT_TRUE(seats.seat(seat(2)).holder());
  EXPECT_EQ(seats.seat(seat(2)).holder()->guest(), play::GuestTag{8});
}

}  // namespace
}  // namespace play_test
