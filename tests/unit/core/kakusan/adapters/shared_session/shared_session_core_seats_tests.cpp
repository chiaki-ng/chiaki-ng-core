#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_session/shared_session_core_rig.hpp"

#include <gtest/gtest.h>

namespace kakusan_test::session_core {
namespace {

class SharedSessionCoreSeats : public SharedSessionCoreTest {};
class SharedSessionCoreSourceJoin : public SharedSessionCoreTest {};

TEST_F(SharedSessionCoreSeats, RunningOutFailsOnlyTheNewcomer) {
  CoreRig rig{*SharingRules::create(2, 1, button_mask)};
  ASSERT_TRUE(rig.session);
  ASSERT_NO_FATAL_FAILURE(rig.admit(0x61, 1));
  const auto first = rig.party;
  ASSERT_NO_FATAL_FAILURE(rig.authenticate(0x62, 2));
  const auto second = rig.party;
  const auto approved_at = rig.outputs.size();
  EXPECT_TRUE(rig.session->approve(second.id, rig.now));
  rig.drain();
  ASSERT_TRUE(rig.run_until(
      [&] { return rig.host_state(second.id) == Progress::gone; }));
  ASSERT_TRUE(rig.last_state(second.id));
  EXPECT_EQ(rig.last_state(second.id)->error, Error::no_capacity);
  rig.run(50);
  EXPECT_EQ(second.guest->error, Error::no_capacity);
  EXPECT_TRUE(second.guest->by_peer);
  EXPECT_TRUE(rig.all<CoreRig::Core::JoinRequested>(approved_at).empty());
  EXPECT_EQ(rig.host_state(first.id), Progress::playing);
  EXPECT_EQ(first.guest->state(), Progress::playing);
  EXPECT_EQ(rig.session->seats().remote_seats, 1U);
}

TEST_F(SharedSessionCoreSourceJoin, ThatFailsEndsTheGuestAndFreesTheSeat) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  rig.auto_join = false;
  ASSERT_NO_FATAL_FAILURE(rig.authenticate(0x63, 1));
  const auto party = rig.party;
  EXPECT_TRUE(rig.session->approve(party.id, rig.now));
  rig.drain();
  ASSERT_EQ(rig.held_joins.size(), 1U);
  EXPECT_EQ(rig.session->seats().reserved_seats, 1U);
  const auto seat = rig.held_joins.front().seat;
  EXPECT_FALSE(
      rig.session->source_joined(SeatBinding{3, 99}, pad_seat(1), rig.now));
  EXPECT_TRUE(rig.session->source_join_failed(seat, rig.now));
  rig.drain();
  rig.run(1500);
  EXPECT_EQ(rig.host_state(party.id), Progress::gone);
  ASSERT_TRUE(rig.last_state(party.id));
  EXPECT_EQ(rig.last_state(party.id)->error, Error::source_join_failed);
  EXPECT_EQ(party.guest->error, Error::source_join_failed);
  const auto removed = rig.removals(party.id);
  ASSERT_EQ(removed.size(), 1U);
  EXPECT_FALSE(removed.front().console_seat);
  EXPECT_EQ(removed.front().seat, seat);
  EXPECT_EQ(rig.session->seats().reserved_seats, 0U);
  EXPECT_EQ(rig.session->seats().remote_seats, 0U);
  EXPECT_FALSE(rig.session->source_join_failed(seat, rig.now));
}

TEST_F(SharedSessionCoreSourceJoin, ThatArrivesAfterTheGuestLeftIsUndone) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  rig.auto_join = false;
  ASSERT_NO_FATAL_FAILURE(rig.authenticate(0x63, 1));
  const auto party = rig.party;
  EXPECT_TRUE(rig.session->approve(party.id, rig.now));
  rig.drain();
  ASSERT_EQ(rig.held_joins.size(), 1U);
  const auto seat = rig.held_joins.front().seat;
  static_cast<void>(
      rig.session->source_joined(SeatBinding{3, 99}, pad_seat(1), rig.now));
  EXPECT_TRUE(rig.session->source_join_failed(seat, rig.now));
  rig.drain();
  rig.run(1500);

  ASSERT_NO_FATAL_FAILURE(rig.authenticate(0x64, 2));
  const auto late = rig.party;
  EXPECT_TRUE(rig.session->approve(late.id, rig.now));
  rig.drain();
  ASSERT_EQ(rig.held_joins.size(), 2U);
  const auto late_seat = rig.held_joins.back().seat;
  EXPECT_GT(late_seat.generation, seat.generation);
  EXPECT_TRUE(rig.session->remove(late.id, rig.now));
  rig.drain();
  const auto cancelled = rig.removals(late.id);
  ASSERT_EQ(cancelled.size(), 1U);
  EXPECT_FALSE(cancelled.front().console_seat);
  const auto joined_late =
      rig.session->source_joined(late_seat, pad_seat(2), rig.now);
  ASSERT_FALSE(joined_late);
  EXPECT_EQ(joined_late.error(), Error::source_join_failed);
  rig.drain();
  const auto undone = rig.removals(late.id);
  ASSERT_EQ(undone.size(), 2U);
  EXPECT_EQ(undone.back().console_seat, pad_seat(2));
  EXPECT_EQ(undone.back().seat, late_seat);
  rig.run(1500);
  EXPECT_EQ(rig.host_state(late.id), Progress::gone);
  EXPECT_FALSE(rig.session->next_deadline_us());
}

TEST_F(SharedSessionCoreSourceJoin, OntoATakenConsoleSeatFailsTheNewcomer) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  ASSERT_NO_FATAL_FAILURE(rig.admit(0x71, 1));
  const auto first = rig.party;
  rig.auto_join = false;
  ASSERT_NO_FATAL_FAILURE(rig.authenticate(0x72, 2));
  const auto second = rig.party;
  const auto approved_at = rig.outputs.size();
  EXPECT_TRUE(rig.session->approve(second.id, rig.now));
  rig.drain();
  ASSERT_EQ(rig.held_joins.size(), 1U);
  const auto seat = rig.held_joins.front().seat;
  const auto joined = rig.session->source_joined(seat, pad_seat(1), rig.now);
  ASSERT_FALSE(joined);
  EXPECT_EQ(joined.error(), Error::source_join_failed);
  rig.drain();
  rig.run(1500);
  EXPECT_EQ(rig.host_state(second.id), Progress::gone);
  ASSERT_TRUE(rig.last_state(second.id));
  EXPECT_EQ(rig.last_state(second.id)->error, Error::source_join_failed);
  EXPECT_EQ(second.guest->error, Error::source_join_failed);
  for (const auto& removal : rig.removals(second.id, approved_at))
    EXPECT_FALSE(removal.console_seat);
  EXPECT_TRUE(rig.removals(first.id).empty());
  EXPECT_EQ(rig.host_state(first.id), Progress::playing);
  EXPECT_EQ(first.guest->state(), Progress::playing);
  EXPECT_EQ(rig.session->seats().remote_seats, 1U);
  EXPECT_EQ(rig.session->seats().reserved_seats, 0U);
  EXPECT_FALSE(rig.session->source_join_failed(seat, rig.now));
}

TEST_F(SharedSessionCoreSeats, TakenBackForLocalPlayersRevokeAGrantedGuest) {
  CoreRig rig{*SharingRules::create(2, 1, button_mask)};
  ASSERT_TRUE(rig.session);
  ASSERT_NO_FATAL_FAILURE(rig.admit(0x71, 1));
  const auto party = rig.party;
  EXPECT_FALSE(rig.session->set_local_seats(3, rig.now));
  const auto revoked_at = rig.outputs.size();
  EXPECT_TRUE(rig.session->set_local_seats(2, rig.now));
  rig.drain();
  rig.run(1500);
  const auto state = rig.last_state(party.id);
  ASSERT_TRUE(state);
  EXPECT_EQ(state->state, Progress::gone);
  EXPECT_EQ(state->reason, control::Reason::capacity);
  EXPECT_EQ(party.guest->reason, control::Reason::capacity);
  EXPECT_EQ(party.guest->state(), Progress::gone);
  const auto inputs = rig.inputs(1, revoked_at);
  ASSERT_FALSE(inputs.empty());
  EXPECT_TRUE(is_neutral(inputs.front()));
  const auto removed = rig.removals(party.id, revoked_at);
  ASSERT_EQ(removed.size(), 1U);
  EXPECT_EQ(removed.front().console_seat, pad_seat(1));
  const auto seats = rig.session->seats();
  EXPECT_EQ(seats.local_seats, 2U);
  EXPECT_EQ(seats.remote_seats, 0U);
  EXPECT_EQ(seats.reserved_seats, 0U);
  EXPECT_EQ(rig.all<CoreRig::Core::SeatsChanged>().back().seats.local_seats,
            2U);
}

}  // namespace
}  // namespace kakusan_test::session_core
