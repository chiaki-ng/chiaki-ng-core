#include "src/core/kakusan/adapters/shared_session/seat_table.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_session/seat_table_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

namespace kakusan_test {
namespace {

using namespace libchiaking::kakusan;
using namespace libchiaking::net::kakusan;

class SeatTableBooking : public SeatTableTest {};

TEST_F(SeatTableBooking, NeedsRoomForTheLocalSeats) {
  EXPECT_FALSE(SeatTable::create(0, 1));
  EXPECT_FALSE(SeatTable::create(4, 0));
  const auto too_many_local = SeatTable::create(2, 3);
  ASSERT_FALSE(too_many_local);
  EXPECT_EQ(too_many_local.error(), Error::no_capacity);
  EXPECT_TRUE(SeatTable::create(1, 1));
  EXPECT_TRUE(SeatTable::create(255, 255));
  EXPECT_TRUE(SeatTable::create(255, 1));

  const auto seats = table(4, 2);
  const auto snapshot = seats.summary();
  EXPECT_EQ(snapshot.capacity, 4);
  EXPECT_EQ(snapshot.local_seats, 2);
  EXPECT_EQ(snapshot.remote_seats, 0);
  EXPECT_EQ(snapshot.reserved_seats, 0);
}

TEST_F(SeatTableBooking, HandsOutTheLowestFreeNonLocalSeat) {
  auto seats = table(5, 2);
  const auto first = reserved(seats);
  EXPECT_EQ(first.seat, 3);
  EXPECT_EQ(first.generation, 1);
  const auto second = reserved(seats);
  EXPECT_EQ(second.seat, 4);
  EXPECT_EQ(second.generation, 2);
  const auto third = reserved(seats);
  EXPECT_EQ(third.seat, 5);
  EXPECT_EQ(third.generation, 3);
  invariant(seats);

  EXPECT_TRUE(seats.release(second));
  const auto refill = reserved(seats);
  EXPECT_EQ(refill.seat, 4);
  EXPECT_EQ(refill.generation, 4);
  EXPECT_TRUE(seats.release(first));
  const auto lowest = reserved(seats);
  EXPECT_EQ(lowest.seat, 3);
  EXPECT_EQ(lowest.generation, 5);
  invariant(seats);

  auto single_local = table(3, 1);
  EXPECT_EQ(reserved(single_local).seat, 2);
}

TEST_F(SeatTableBooking, FillsUpToItsCapacity) {
  auto only_local = table(1, 1);
  const auto none = only_local.reserve();
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error(), Error::no_capacity);
  EXPECT_EQ(only_local.last_generation(), 0);

  auto full_local = table(4, 4);
  EXPECT_FALSE(full_local.reserve());

  auto wide = table(255, 1);
  for (unsigned expected = 2; expected <= 255; ++expected) {
    const auto binding = reserved(wide);
    EXPECT_EQ(binding.seat, expected);
    EXPECT_EQ(binding.generation, expected - 1);
  }
  EXPECT_EQ(wide.reserved_seats(), 254);
  const auto exhausted = wide.reserve();
  ASSERT_FALSE(exhausted);
  EXPECT_EQ(exhausted.error(), Error::no_capacity);
  EXPECT_EQ(wide.last_generation(), 254);
  invariant(wide);

  auto wide_locals = table(255, 255);
  EXPECT_FALSE(wide_locals.reserve());
  EXPECT_EQ(wide_locals.summary().local_seats, 255);
}

TEST_F(SeatTableBooking, NeverOverbooksTheLastSeat) {
  auto seats = table(3, 2);
  const auto winner = reserved(seats);
  EXPECT_EQ(winner.seat, 3);
  for (int attempt = 0; attempt < 4; ++attempt) {
    const auto loser = seats.reserve();
    ASSERT_FALSE(loser);
    EXPECT_EQ(loser.error(), Error::no_capacity);
  }
  EXPECT_EQ(seats.reserved_seats(), 1);
  EXPECT_TRUE(seats.grant(winner));
  EXPECT_FALSE(seats.reserve());
  EXPECT_EQ(seats.remote_seats(), 1);
  EXPECT_EQ(seats.reserved_seats(), 0);
  invariant(seats);

  EXPECT_TRUE(seats.release(winner));
  EXPECT_EQ(reserved(seats).seat, 3);
}

TEST_F(SeatTableBooking, GivesEveryBindingANewGeneration) {
  auto seats = table(3, 1);
  const auto first = reserved(seats);
  EXPECT_EQ(first, SeatBinding({2, 1}));
  EXPECT_TRUE(seats.release(first));
  const auto second = reserved(seats);
  EXPECT_EQ(second, SeatBinding({2, 2}));
  EXPECT_NE(second.generation, first.generation);
  EXPECT_TRUE(seats.release(second));
  const auto third = reserved(seats);
  EXPECT_EQ(third, SeatBinding({2, 3}));

  const auto parallel = reserved(seats);
  EXPECT_EQ(parallel, SeatBinding({3, 4}));

  auto edge = table(3, 1, std::numeric_limits<std::uint32_t>::max() - 2);
  const auto penultimate = reserved(edge);
  EXPECT_EQ(penultimate.generation,
            std::numeric_limits<std::uint32_t>::max() - 1);
  const auto last = reserved(edge);
  EXPECT_EQ(last.generation, std::numeric_limits<std::uint32_t>::max());
  EXPECT_TRUE(edge.release(penultimate));
  const auto refused = edge.reserve();
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), Error::no_capacity);
  EXPECT_EQ(edge.last_generation(), std::numeric_limits<std::uint32_t>::max());
  EXPECT_TRUE(edge.holds(last));
  EXPECT_TRUE(edge.release(last));
  EXPECT_FALSE(edge.reserve());
  invariant(edge);
}

TEST_F(SeatTableBooking, IgnoresStaleBindings) {
  auto seats = table(3, 1);
  const auto stale = reserved(seats);
  EXPECT_TRUE(seats.release(stale));
  EXPECT_FALSE(seats.release(stale));

  const auto reused = reserved(seats);
  EXPECT_EQ(reused.seat, stale.seat);
  EXPECT_NE(reused.generation, stale.generation);
  EXPECT_FALSE(seats.holds(stale));
  EXPECT_FALSE(seats.owns(stale));
  EXPECT_EQ(seats.state_of(stale), SeatState::free);
  const auto stale_grant = seats.grant(stale);
  EXPECT_FALSE(stale_grant);
  EXPECT_EQ(seats.state_of(reused), SeatState::reserved);
  EXPECT_FALSE(seats.release(stale));
  EXPECT_TRUE(seats.holds(reused));
  EXPECT_EQ(seats.reserved_seats(), 1);

  EXPECT_TRUE(seats.grant(reused));
  EXPECT_FALSE(seats.owns(stale));
  EXPECT_TRUE(seats.owns(reused));
  EXPECT_FALSE(seats.release(stale));
  EXPECT_TRUE(seats.owns(reused));
  EXPECT_FALSE(seats.grant(reused));
  invariant(seats);
}

TEST_F(SeatTableBooking, AnswersWhoOwnsASeat) {
  auto seats = table(4, 2);
  const auto binding = reserved(seats);
  EXPECT_TRUE(seats.holds(binding));
  EXPECT_FALSE(seats.owns(binding));
  EXPECT_EQ(seats.state_of(binding), SeatState::reserved);
  EXPECT_TRUE(seats.grant(binding));
  EXPECT_TRUE(seats.holds(binding));
  EXPECT_TRUE(seats.owns(binding));
  EXPECT_EQ(seats.state_of(binding), SeatState::granted);

  EXPECT_FALSE(seats.owns(SeatBinding{binding.seat, binding.generation + 1}));
  EXPECT_FALSE(seats.owns(SeatBinding{4, binding.generation}));
  for (const std::uint8_t local :
       std::initializer_list<std::uint8_t>{0, 1, 2}) {
    EXPECT_FALSE(seats.owns(SeatBinding{local, binding.generation}));
    EXPECT_FALSE(seats.holds(SeatBinding{local, 0}));
    EXPECT_FALSE(seats.release(SeatBinding{local, binding.generation}));
  }
  EXPECT_FALSE(seats.owns(SeatBinding{5, binding.generation}));
  EXPECT_FALSE(seats.owns(SeatBinding{255, binding.generation}));
  EXPECT_FALSE(seats.grant(SeatBinding{1, 1}));
  EXPECT_TRUE(seats.owns(binding));

  EXPECT_TRUE(seats.release(binding));
  EXPECT_FALSE(seats.owns(binding));
  EXPECT_EQ(seats.remote_seats(), 0);
}

TEST_F(SeatTableBooking, LeavesItsStateAloneWhenItRefuses) {
  auto seats = table(5, 3);
  const auto remote = granted(seats);
  const auto pending = reserved(seats);
  const auto before = seats.summary();
  const auto generation = seats.last_generation();

  const auto below = seats.set_capacity(2);
  ASSERT_FALSE(below);
  EXPECT_EQ(below.error(), Error::no_capacity);
  const auto above = seats.set_local_seats(6);
  ASSERT_FALSE(above);
  EXPECT_EQ(above.error(), Error::no_capacity);
  EXPECT_FALSE(seats.set_capacity(0));
  EXPECT_FALSE(seats.set_local_seats(0));

  const auto after = seats.summary();
  EXPECT_EQ(after.capacity, before.capacity);
  EXPECT_EQ(after.local_seats, before.local_seats);
  EXPECT_EQ(after.remote_seats, before.remote_seats);
  EXPECT_EQ(after.reserved_seats, before.reserved_seats);
  EXPECT_EQ(seats.last_generation(), generation);
  EXPECT_TRUE(seats.owns(remote));
  EXPECT_TRUE(seats.holds(pending));
  EXPECT_TRUE(seats.is_local(1));
  EXPECT_TRUE(seats.is_local(2));
  EXPECT_TRUE(seats.is_local(3));
  EXPECT_FALSE(seats.is_local(4));
  EXPECT_FALSE(seats.is_local(5));
}

TEST_F(SeatTableBooking, KeepsRevokedBindingsDead) {
  auto seats = table(3, 1);
  const auto binding = granted(seats);
  const auto revoked = seats.set_capacity(1);
  ASSERT_TRUE(revoked);
  EXPECT_EQ(revoked.value().size(), 1);
  EXPECT_EQ(revoked.value()[0], binding);
  EXPECT_FALSE(seats.release(binding));
  EXPECT_FALSE(seats.owns(binding));
  EXPECT_TRUE(seats.set_capacity(3));
  const auto reused = reserved(seats);
  EXPECT_EQ(reused.seat, binding.seat);
  EXPECT_NE(reused.generation, binding.generation);
  EXPECT_FALSE(seats.owns(binding));
  EXPECT_FALSE(seats.release(binding));
  EXPECT_TRUE(seats.holds(reused));
}

}  // namespace
}  // namespace kakusan_test
