#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/pair_rig.hpp"

#include <gtest/gtest.h>

namespace kakusan_test {
namespace {

class SharedVisitReservation : public PairStageTest {
 protected:
  auto give(SeatNumber wanted) -> bool {
    return static_cast<bool>(stage.share->give_seat(
        stage.guest_id,
        {libchiaking::controllers::ControllerKind::dualsense,
         libchiaking::AccountId{PairStage::guest_account}, wanted}));
  }
};

TEST_F(SharedVisitReservation, IsHeardByTheHostBeforeTheConsoleJoins) {
  ASSERT_NO_FATAL_FAILURE(stage.verified());
  EXPECT_EQ(stage.host.reservations(), 0U);
  const auto waiting = stage.share->players();
  ASSERT_EQ(waiting.size(), 1U);
  EXPECT_FALSE(waiting.front().reserved_seat);
  EXPECT_FALSE(waiting.front().console_seat);

  EXPECT_TRUE(give(seat(2)));
  ASSERT_NO_FATAL_FAILURE(
      stage.until([&] { return stage.host.reservations() == 1; }));
  const auto* reserved = stage.host.seen.last<SharedSession::PlayerChanged>();
  ASSERT_NE(reserved, nullptr);
  EXPECT_EQ(reserved->player.guest, stage.guest_id);
  EXPECT_EQ(reserved->player.progress, Progress::waiting_for_seat);
  EXPECT_EQ(reserved->player.reserved_seat, seat(2));
  EXPECT_FALSE(reserved->player.console_seat);
  ASSERT_TRUE(reserved->player.grant.has_value());
  EXPECT_EQ(reserved->player.device, stage.pair->guest().this_device().key());
  EXPECT_FALSE(reserved->player.refusal);
  EXPECT_FALSE(reserved->player.ended);
  const auto grant = *reserved->player.grant;
  EXPECT_EQ(stage.host.seen.count<SharedSession::PlayerSeated>(), 0U);
  const auto held = stage.session->seats().seat(seat(2));
  EXPECT_TRUE(held.is_held_by_guest());
  EXPECT_EQ(held.state(), libchiaking::play::SeatState::joining);

  ASSERT_NO_FATAL_FAILURE(
      stage.until([&] { return stage.link.count("seat_join") == 1; }));
  stage.quiet(200);
  EXPECT_EQ(stage.host.reservations(), 1U);
  EXPECT_EQ(stage.host.seen.count<SharedSession::PlayerSeated>(), 0U);
  EXPECT_TRUE(stage.host.reached(Progress::waiting_for_seat));
  const auto before = stage.share->players();
  ASSERT_EQ(before.size(), 1U);
  EXPECT_EQ(before.front().guest, stage.guest_id);
  EXPECT_EQ(before.front().reserved_seat, seat(2));
  EXPECT_FALSE(before.front().console_seat);
  EXPECT_EQ(before.front().grant, grant);

  stage.link.join(seat(2), {0, 0, 255});
  ASSERT_NO_FATAL_FAILURE(stage.until([&] {
    return stage.host.reached(Progress::playing) &&
           stage.guest->reached(Progress::playing);
  }));
  EXPECT_EQ(stage.host.seen.count<SharedSession::PlayerSeated>(), 1U);
  const auto* seated = stage.host.seen.last<SharedSession::PlayerSeated>();
  ASSERT_NE(seated, nullptr);
  EXPECT_EQ(seated->guest, stage.guest_id);
  EXPECT_EQ(seated->seat, seat(2));
  EXPECT_EQ(stage.host.reservations(), 1U);
  const auto* playing = stage.host.seen.last<SharedSession::PlayerChanged>();
  ASSERT_NE(playing, nullptr);
  EXPECT_EQ(playing->player.console_seat, seat(2));
  EXPECT_FALSE(playing->player.reserved_seat);
  EXPECT_EQ(playing->player.grant, grant);
  const auto after = stage.share->players();
  ASSERT_EQ(after.size(), 1U);
  EXPECT_EQ(after.front().console_seat, seat(2));
  EXPECT_FALSE(after.front().reserved_seat);
  EXPECT_EQ(after.front().grant, grant);
}

TEST_F(SharedVisitReservation, IsDroppedWhenTheConsoleRefusesTheSeat) {
  ASSERT_NO_FATAL_FAILURE(stage.verified());
  EXPECT_TRUE(give(seat(1)));
  ASSERT_NO_FATAL_FAILURE(stage.until([&] {
    return stage.host.reservations() == 1 && stage.link.count("seat_join") == 1;
  }));
  EXPECT_EQ(stage.host.seen.last<SharedSession::PlayerChanged>()
                ->player.reserved_seat,
            seat(1));
  ASSERT_FALSE(stage.share->players().empty());
  EXPECT_EQ(stage.share->players().front().reserved_seat, seat(1));

  stage.link.refuse(libchiaking::play::SeatRefusal::by_console(seat(1), 3));
  ASSERT_NO_FATAL_FAILURE(stage.finish_visit());
  ASSERT_NO_FATAL_FAILURE(
      stage.until([&] { return stage.host.reached(Progress::gone); }));
  const auto* gone = stage.host.seen.last<SharedSession::PlayerChanged>();
  ASSERT_NE(gone, nullptr);
  EXPECT_EQ(gone->player.guest, stage.guest_id);
  EXPECT_FALSE(gone->player.reserved_seat);
  EXPECT_FALSE(gone->player.console_seat);
  EXPECT_EQ(gone->player.refusal, Refusal::Kind::console_refused_seat);
  EXPECT_FALSE(gone->player.by_them);
  EXPECT_EQ(stage.host.reservations(), 1U);
  EXPECT_TRUE(stage.share->players().empty());
  EXPECT_EQ(stage.host.seen.count<SharedSession::PlayerSeated>(), 0U);
  EXPECT_EQ(stage.host.seen.count<SharedSession::PlayerUnseated>(), 0U);
  EXPECT_FALSE(stage.session->seats().seat(seat(1)).is_held_by_guest());
  const auto* told = stage.guest->seen.last<GuestVisit::VisitChanged>();
  ASSERT_NE(told, nullptr);
  EXPECT_EQ(told->progress, Progress::gone);
  EXPECT_EQ(told->refusal, Refusal::Kind::console_refused_seat);
  EXPECT_EQ(stage.outcome->status(), VisitOutcome::Status::visit_failed);
  EXPECT_EQ(stage.outcome->refusal(), Refusal::Kind::console_refused_seat);
  EXPECT_EQ(stage.share->seats(),
            (libchiaking::kakusan::SeatSummary{4, 1, 0, 0}));
  EXPECT_EQ(stage.host.seen.count<SharedSession::SharingEnded>(), 0U);
  EXPECT_TRUE(stage.share->invite_player());
}

}  // namespace
}  // namespace kakusan_test
