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

class SeatTableResizing : public SeatTableTest {};

TEST_F(SeatTableResizing, RevokesFromTheTopWhenCapacityShrinks) {
  auto seats = table(6, 1);
  const auto a = granted(seats);
  const auto b = reserved(seats);
  const auto c = granted(seats);
  const auto d = reserved(seats);
  const auto e = reserved(seats);
  EXPECT_EQ(a.seat, 2);
  EXPECT_EQ(b.seat, 3);
  EXPECT_EQ(c.seat, 4);
  EXPECT_EQ(d.seat, 5);
  EXPECT_EQ(e.seat, 6);

  const auto none = seats.set_capacity(6);
  ASSERT_TRUE(none);
  EXPECT_TRUE(none.value().empty());
  const auto bigger = seats.set_capacity(7);
  ASSERT_TRUE(bigger);
  EXPECT_TRUE(bigger.value().empty());
  EXPECT_EQ(seats.capacity(), 7);
  EXPECT_TRUE(seats.set_capacity(6));

  const auto revoked = seats.set_capacity(4);
  ASSERT_TRUE(revoked);
  EXPECT_EQ(revoked.value(), std::vector<SeatBinding>({e, d}));
  EXPECT_EQ(seats.capacity(), 4);
  EXPECT_TRUE(seats.holds(a));
  EXPECT_TRUE(seats.holds(b));
  EXPECT_TRUE(seats.holds(c));
  EXPECT_FALSE(seats.holds(d));
  EXPECT_FALSE(seats.holds(e));
  EXPECT_EQ(seats.remote_seats(), 2);
  EXPECT_EQ(seats.reserved_seats(), 1);
  invariant(seats);

  const auto more = seats.set_capacity(2);
  ASSERT_TRUE(more);
  EXPECT_EQ(more.value(), std::vector<SeatBinding>({b, c}));
  EXPECT_TRUE(seats.holds(a));
  EXPECT_FALSE(seats.holds(b));
  EXPECT_FALSE(seats.holds(c));
  invariant(seats);

  EXPECT_FALSE(seats.reserve());
  EXPECT_TRUE(seats.release(a));
  const auto regrown = seats.set_capacity(3);
  ASSERT_TRUE(regrown);
  EXPECT_TRUE(regrown.value().empty());
  const auto fresh = reserved(seats);
  EXPECT_EQ(fresh.seat, 2);
  EXPECT_EQ(fresh.generation, 6);
}

TEST_F(SeatTableResizing, RevokesPendingBeforeGrantedWhenCapacityShrinks) {
  auto seats = table(4, 1);
  const auto low_granted = granted(seats);
  const auto mid_pending = reserved(seats);
  const auto high_granted = granted(seats);
  const auto revoked = seats.set_capacity(1);
  ASSERT_TRUE(revoked);
  EXPECT_EQ(revoked.value(),
            std::vector<SeatBinding>({mid_pending, high_granted, low_granted}));
  EXPECT_EQ(seats.remote_seats(), 0);
  EXPECT_EQ(seats.reserved_seats(), 0);
  EXPECT_EQ(seats.capacity(), 1);
  invariant(seats);
}

TEST_F(SeatTableResizing, NeverDisplacesLocalSeatsForCapacity) {
  auto seats = table(5, 3);
  const auto remote = granted(seats);
  const auto pending = reserved(seats);
  const auto before = seats.summary();

  const auto below = seats.set_capacity(2);
  ASSERT_FALSE(below);
  EXPECT_EQ(below.error(), Error::no_capacity);
  EXPECT_EQ(seats.capacity(), 5);
  EXPECT_TRUE(seats.holds(remote));
  EXPECT_TRUE(seats.holds(pending));
  EXPECT_EQ(seats.summary().remote_seats, before.remote_seats);
  EXPECT_EQ(seats.summary().reserved_seats, before.reserved_seats);

  EXPECT_FALSE(seats.set_capacity(0));
  EXPECT_EQ(seats.capacity(), 5);

  const auto exact = seats.set_capacity(3);
  ASSERT_TRUE(exact);
  EXPECT_EQ(exact.value(), std::vector<SeatBinding>({pending, remote}));
  EXPECT_EQ(seats.local_seats(), 3);
  EXPECT_EQ(seats.capacity(), 3);
  EXPECT_FALSE(seats.reserve());
  invariant(seats);

  auto single = table(1, 1);
  EXPECT_TRUE(single.set_capacity(1));
  EXPECT_FALSE(single.set_capacity(0));

  auto wide = table(255, 255);
  EXPECT_TRUE(wide.set_capacity(255));
  EXPECT_FALSE(wide.set_capacity(254));
  EXPECT_EQ(wide.capacity(), 255);
}

TEST_F(SeatTableResizing, GivesNewLocalSeatsFreeSeatsFirst) {
  auto seats = table(6, 1);
  const auto a = granted(seats);
  const auto b = reserved(seats);
  EXPECT_EQ(a.seat, 2);
  EXPECT_EQ(b.seat, 3);

  const auto same = seats.set_local_seats(1);
  ASSERT_TRUE(same);
  EXPECT_TRUE(same.value().empty());

  const auto raised = seats.set_local_seats(3);
  ASSERT_TRUE(raised);
  EXPECT_TRUE(raised.value().empty());
  EXPECT_EQ(seats.local_seats(), 3);
  EXPECT_TRUE(seats.is_local(1));
  EXPECT_TRUE(seats.is_local(4));
  EXPECT_TRUE(seats.is_local(5));
  EXPECT_FALSE(seats.is_local(2));
  EXPECT_FALSE(seats.is_local(3));
  EXPECT_FALSE(seats.is_local(6));
  EXPECT_TRUE(seats.holds(a));
  EXPECT_TRUE(seats.holds(b));
  EXPECT_EQ(seats.remote_seats(), 1);
  EXPECT_EQ(seats.reserved_seats(), 1);
  EXPECT_EQ(reserved(seats).seat, 6);
  invariant(seats);
}

TEST_F(SeatTableResizing, RevokesPendingBeforeGrantedForNewLocalSeats) {
  auto seats = table(6, 1);
  const auto a = granted(seats);
  const auto b = reserved(seats);
  const auto c = granted(seats);
  const auto d = reserved(seats);
  EXPECT_EQ(a.seat, 2);
  EXPECT_EQ(b.seat, 3);
  EXPECT_EQ(c.seat, 4);
  EXPECT_EQ(d.seat, 5);

  const auto first = seats.set_local_seats(2);
  ASSERT_TRUE(first);
  EXPECT_TRUE(first.value().empty());
  EXPECT_TRUE(seats.is_local(6));

  const auto second = seats.set_local_seats(3);
  ASSERT_TRUE(second);
  EXPECT_EQ(second.value(), std::vector<SeatBinding>({d}));
  EXPECT_TRUE(seats.is_local(5));
  EXPECT_FALSE(seats.holds(d));
  EXPECT_TRUE(seats.holds(a));
  EXPECT_TRUE(seats.holds(b));
  EXPECT_TRUE(seats.holds(c));

  const auto third = seats.set_local_seats(4);
  ASSERT_TRUE(third);
  EXPECT_EQ(third.value(), std::vector<SeatBinding>({b}));
  EXPECT_TRUE(seats.is_local(3));

  const auto fourth = seats.set_local_seats(5);
  ASSERT_TRUE(fourth);
  EXPECT_EQ(fourth.value(), std::vector<SeatBinding>({c}));
  EXPECT_TRUE(seats.is_local(4));
  EXPECT_TRUE(seats.holds(a));
  EXPECT_EQ(seats.remote_seats(), 1);
  EXPECT_EQ(seats.reserved_seats(), 0);
  EXPECT_EQ(seats.local_seats(), 5);
  invariant(seats);
  EXPECT_FALSE(seats.reserve());
}

TEST_F(SeatTableResizing, RevokesExactlyEnoughForSeveralNewLocalSeats) {
  auto seats = table(5, 1);
  const auto a = granted(seats);
  const auto b = reserved(seats);
  const auto c = granted(seats);
  const auto d = reserved(seats);
  const auto raised = seats.set_local_seats(4);
  ASSERT_TRUE(raised);
  EXPECT_EQ(raised.value(), std::vector<SeatBinding>({d, b, c}));
  EXPECT_TRUE(seats.is_local(3));
  EXPECT_TRUE(seats.is_local(4));
  EXPECT_TRUE(seats.is_local(5));
  EXPECT_TRUE(seats.holds(a));
  EXPECT_FALSE(seats.is_local(2));
  invariant(seats);

  auto mixed = table(5, 1);
  const auto m_a = granted(mixed);
  const auto m_b = reserved(mixed);
  EXPECT_EQ(m_a.seat, 2);
  EXPECT_EQ(m_b.seat, 3);
  const auto both = mixed.set_local_seats(3);
  ASSERT_TRUE(both);
  EXPECT_TRUE(both.value().empty());
  EXPECT_TRUE(mixed.is_local(4));
  EXPECT_TRUE(mixed.is_local(5));
  EXPECT_TRUE(mixed.holds(m_a));
  EXPECT_TRUE(mixed.holds(m_b));
  const auto partial = mixed.set_local_seats(4);
  ASSERT_TRUE(partial);
  EXPECT_EQ(partial.value(), std::vector<SeatBinding>({m_b}));
  EXPECT_TRUE(mixed.holds(m_a));
  EXPECT_TRUE(mixed.is_local(3));
  const auto rest = mixed.set_local_seats(5);
  ASSERT_TRUE(rest);
  EXPECT_EQ(rest.value(), std::vector<SeatBinding>({m_a}));
  EXPECT_EQ(mixed.remote_seats(), 0);
  EXPECT_EQ(mixed.local_seats(), 5);
}

TEST_F(SeatTableResizing, FreesTheHighestLocalSeatFirst) {
  auto seats = table(4, 1);
  const auto raised = seats.set_local_seats(3);
  ASSERT_TRUE(raised);
  EXPECT_TRUE(raised.value().empty());
  EXPECT_TRUE(seats.is_local(2));
  EXPECT_TRUE(seats.is_local(3));
  const auto first = reserved(seats);
  EXPECT_EQ(first, SeatBinding({4, 1}));
  EXPECT_TRUE(seats.release(first));

  const auto lowered = seats.set_local_seats(2);
  ASSERT_TRUE(lowered);
  EXPECT_TRUE(lowered.value().empty());
  EXPECT_TRUE(seats.is_local(1));
  EXPECT_TRUE(seats.is_local(2));
  EXPECT_FALSE(seats.is_local(3));
  const auto reused = reserved(seats);
  EXPECT_EQ(reused, SeatBinding({3, 2}));
  EXPECT_FALSE(seats.holds(first));
  invariant(seats);

  const auto bottom = seats.set_local_seats(1);
  ASSERT_TRUE(bottom);
  EXPECT_TRUE(bottom.value().empty());
  EXPECT_TRUE(seats.is_local(1));
  EXPECT_FALSE(seats.is_local(2));
  EXPECT_TRUE(seats.holds(reused));
  EXPECT_EQ(reserved(seats).seat, 2);
  EXPECT_FALSE(seats.set_local_seats(0));
  EXPECT_TRUE(seats.is_local(1));
}

TEST_F(SeatTableResizing, FreesLocalSeatsWithoutRevoking) {
  auto seats = table(4, 3);
  const auto first = reserved(seats);
  EXPECT_EQ(first.seat, 4);
  const auto lowered = seats.set_local_seats(1);
  ASSERT_TRUE(lowered);
  EXPECT_TRUE(lowered.value().empty());
  EXPECT_TRUE(seats.holds(first));
  EXPECT_EQ(reserved(seats).seat, 2);
  EXPECT_EQ(reserved(seats).seat, 3);
  EXPECT_FALSE(seats.reserve());
  invariant(seats);
}

TEST_F(SeatTableResizing, RelocatesDisplacedLocalSeats) {
  auto seats = table(4, 1);
  const auto a = granted(seats);
  const auto b = granted(seats);
  const auto c = granted(seats);
  EXPECT_EQ(a.seat, 2);
  EXPECT_EQ(b.seat, 3);
  EXPECT_EQ(c.seat, 4);
  const auto raised = seats.set_local_seats(2);
  ASSERT_TRUE(raised);
  EXPECT_EQ(raised.value(), std::vector<SeatBinding>({c}));
  EXPECT_TRUE(seats.is_local(4));
  EXPECT_FALSE(seats.is_local(2));

  EXPECT_TRUE(seats.release(a));
  const auto shrunk = seats.set_capacity(3);
  ASSERT_TRUE(shrunk);
  EXPECT_TRUE(shrunk.value().empty());
  EXPECT_EQ(seats.capacity(), 3);
  EXPECT_EQ(seats.local_seats(), 2);
  EXPECT_TRUE(seats.is_local(1));
  EXPECT_TRUE(seats.is_local(2));
  EXPECT_FALSE(seats.is_local(3));
  EXPECT_FALSE(seats.is_local(4));
  EXPECT_TRUE(seats.holds(b));
  EXPECT_FALSE(seats.reserve());
  invariant(seats);
}

TEST_F(SeatTableResizing, RevokesToMakeRoomForDisplacedLocalSeats) {
  auto seats = table(5, 1);
  const auto a = granted(seats);
  const auto b = reserved(seats);
  const auto c = granted(seats);
  const auto d = granted(seats);
  EXPECT_EQ(a.seat, 2);
  EXPECT_EQ(b.seat, 3);
  EXPECT_EQ(c.seat, 4);
  EXPECT_EQ(d.seat, 5);
  const auto raised = seats.set_local_seats(3);
  ASSERT_TRUE(raised);
  EXPECT_EQ(raised.value(), std::vector<SeatBinding>({b, d}));
  EXPECT_TRUE(seats.is_local(3));
  EXPECT_TRUE(seats.is_local(5));
  EXPECT_TRUE(seats.holds(a));
  EXPECT_TRUE(seats.holds(c));

  const auto shrunk = seats.set_capacity(4);
  ASSERT_TRUE(shrunk);
  EXPECT_EQ(shrunk.value(), std::vector<SeatBinding>({c}));
  EXPECT_TRUE(seats.is_local(1));
  EXPECT_TRUE(seats.is_local(3));
  EXPECT_TRUE(seats.is_local(4));
  EXPECT_TRUE(seats.holds(a));
  EXPECT_FALSE(seats.holds(c));
  EXPECT_EQ(seats.local_seats(), 3);
  EXPECT_EQ(seats.remote_seats(), 1);
  invariant(seats);

  const auto tight = seats.set_capacity(3);
  ASSERT_TRUE(tight);
  EXPECT_EQ(tight.value(), std::vector<SeatBinding>({a}));
  EXPECT_TRUE(seats.is_local(1));
  EXPECT_TRUE(seats.is_local(2));
  EXPECT_TRUE(seats.is_local(3));
  EXPECT_EQ(seats.remote_seats(), 0);
  invariant(seats);
}

TEST_F(SeatTableResizing, OrdersEveryRevocation) {
  auto seats = table(6, 1);
  const auto a = granted(seats);
  const auto b = reserved(seats);
  const auto c = granted(seats);
  const auto d = reserved(seats);
  const auto e = granted(seats);
  EXPECT_EQ(a.seat, 2);
  EXPECT_EQ(b.seat, 3);
  EXPECT_EQ(c.seat, 4);
  EXPECT_EQ(d.seat, 5);
  EXPECT_EQ(e.seat, 6);
  const auto raised = seats.set_local_seats(2);
  ASSERT_TRUE(raised);
  EXPECT_EQ(raised.value(), std::vector<SeatBinding>({d}));
  EXPECT_TRUE(seats.is_local(5));

  const auto shrunk = seats.set_capacity(4);
  ASSERT_TRUE(shrunk);
  EXPECT_EQ(shrunk.value(), std::vector<SeatBinding>({b, e}));
  EXPECT_TRUE(seats.is_local(1));
  EXPECT_TRUE(seats.is_local(3));
  EXPECT_TRUE(seats.holds(a));
  EXPECT_TRUE(seats.holds(c));
  EXPECT_EQ(seats.local_seats(), 2);
  EXPECT_EQ(seats.remote_seats(), 2);
  EXPECT_EQ(seats.capacity(), 4);
  invariant(seats);
}

TEST_F(SeatTableResizing, NeverGrowsLocalSeatsPastCapacity) {
  auto seats = table(3, 1);
  const auto remote = granted(seats);
  const auto refused = seats.set_local_seats(4);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), Error::no_capacity);
  EXPECT_EQ(seats.local_seats(), 1);
  EXPECT_TRUE(seats.holds(remote));
  EXPECT_FALSE(seats.set_local_seats(0));
  EXPECT_EQ(seats.local_seats(), 1);

  const auto full = seats.set_local_seats(3);
  ASSERT_TRUE(full);
  EXPECT_EQ(full.value(), std::vector<SeatBinding>({remote}));
  EXPECT_FALSE(seats.reserve());

  auto wide = table(255, 1);
  const auto many = wide.set_local_seats(255);
  ASSERT_TRUE(many);
  EXPECT_TRUE(many.value().empty());
  EXPECT_FALSE(wide.reserve());
  EXPECT_FALSE(wide.set_local_seats(0));
}

}  // namespace
}  // namespace kakusan_test
