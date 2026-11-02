#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using libchiaking::controllers::ControllerKind;
using libchiaking::controllers::SeatNumber;
using play::GuestTag;
using play::Seat;
using play::SeatHolder;
using play::SeatState;

TEST(SeatNumber, CountsFourSeats) {
  EXPECT_EQ(SeatNumber::count, 4u);
  for (std::size_t index = 0; index < SeatNumber::count; ++index) {
    SCOPED_TRACE(index);
    const auto seat = SeatNumber::create(index);
    ASSERT_TRUE(seat);
    EXPECT_EQ(seat->index(), index);
  }
  EXPECT_FALSE(SeatNumber::create(4));
  EXPECT_FALSE(SeatNumber::create(255));
}

TEST(SeatNumber, StartsAtTheFirstSeatAndOrdersTheAddedOnes) {
  EXPECT_EQ(SeatNumber{}, SeatNumber::first());
  EXPECT_TRUE(SeatNumber::first().is_first());
  EXPECT_FALSE(SeatNumber::first().is_added());
  EXPECT_TRUE(SeatNumber::create(1)->is_added());
  EXPECT_LT(*SeatNumber::create(1), *SeatNumber::create(2));
  EXPECT_NE(*SeatNumber::create(3), SeatNumber::first());
}

TEST(SeatNumber, MasksAnyByteIntoASeat) {
  EXPECT_EQ(SeatNumber::masked(5), *SeatNumber::create(1));
  EXPECT_EQ(SeatNumber::masked(0xff).index(), 3u);
}

TEST(Seat, IsEmptyWithNoHolder) {
  const auto second = *SeatNumber::create(1);
  const Seat empty{second};
  EXPECT_EQ(empty.number(), second);
  EXPECT_TRUE(empty.is_empty());
  EXPECT_EQ(empty.state(), SeatState::empty);
  EXPECT_FALSE(empty.holder());
  EXPECT_FALSE(empty.controller());
  EXPECT_FALSE(empty.is_held_by_guest());
  EXPECT_EQ(empty, Seat{second});
}

TEST(Seat, CannotBeHeldInTheEmptyState) {
  EXPECT_FALSE(Seat::held(*SeatNumber::create(1), SeatState::empty,
                          SeatHolder::local_player(),
                          ControllerKind::dualsense));
}

TEST(Seat, IsHeldByTheLocalPlayer) {
  const auto local =
      Seat::held(*SeatNumber::create(1), SeatState::playing,
                 SeatHolder::local_player(), ControllerKind::dualsense);
  ASSERT_TRUE(local);
  EXPECT_FALSE(local->is_empty());
  EXPECT_EQ(local->state(), SeatState::playing);
  ASSERT_TRUE(local->holder());
  EXPECT_TRUE(local->holder()->is_local_player());
  EXPECT_FALSE(local->holder()->guest());
  EXPECT_FALSE(local->is_held_by_guest());
  EXPECT_EQ(local->controller(), ControllerKind::dualsense);
}

TEST(Seat, IsHeldByASharedGuest) {
  const auto second = *SeatNumber::create(1);
  const Seat empty{second};
  const auto local =
      Seat::held(second, SeatState::playing, SeatHolder::local_player(),
                 ControllerKind::dualsense);
  const auto guest = Seat::held(second, SeatState::entering_passcode,
                                SeatHolder::shared_guest(GuestTag{77}),
                                ControllerKind::dualshock4);
  ASSERT_TRUE(local);
  ASSERT_TRUE(guest);
  EXPECT_TRUE(guest->is_held_by_guest());
  ASSERT_TRUE(guest->holder());
  EXPECT_EQ(guest->holder()->guest(), GuestTag{77});
  ASSERT_TRUE(guest->holder()->guest());
  EXPECT_EQ(guest->holder()->guest()->value(), 77u);
  EXPECT_EQ(guest->state(), SeatState::entering_passcode);
  EXPECT_NE(*guest, *local);
  EXPECT_NE(*guest, empty);
}

TEST(SeatHolder, TellsGuestsAndTheLocalPlayerApart) {
  EXPECT_NE(SeatHolder::shared_guest(GuestTag{1}),
            SeatHolder::shared_guest(GuestTag{2}));
  EXPECT_NE(SeatHolder::shared_guest(GuestTag{1}), SeatHolder::local_player());
  EXPECT_LT(GuestTag{1}, GuestTag{2});
}

}  // namespace
}  // namespace play_test
