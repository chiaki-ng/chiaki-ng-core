#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/shared_session_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace kakusan_test {
namespace {

using libchiaking::kakusan::EndReason;
using libchiaking::kakusan::SharedSession;
using manual::Progress;

class SharedSessionSeat : public SharedSessionTest {};

TEST_F(SharedSessionSeat, IsTheOneTheApprovalAsksFor) {
  ShareRig rig;
  rig.recorder.seating = libchiaking::kakusan::GuestSeating{
      libchiaking::controllers::ControllerKind::dualsense, std::nullopt,
      seat(3)};
  ASSERT_NO_FATAL_FAILURE(rig.play());
  ASSERT_NO_FATAL_FAILURE(rig.stream());
  ASSERT_NO_FATAL_FAILURE(rig.seated(0x42, seat(3)));
  using Request = libchiaking::play::testing::ScriptedLink::SeatRequest;
  const std::vector expected{
      Request{Request::Kind::join, seat(3), std::nullopt,
              libchiaking::controllers::ControllerKind::dualsense}};
  EXPECT_EQ(rig.session.link.seat_requests(), expected);
  ASSERT_NE(rig.recorder.find<SharedSession::PlayerSeated>(), nullptr);
  EXPECT_EQ(rig.recorder.find<SharedSession::PlayerSeated>()->seat, seat(3));
  EXPECT_TRUE(rig.session.session->seats().seat(seat(3)).is_held_by_guest());
}

TEST_F(SharedSessionSeat, DroppedByTheConsoleEndsTheGuestAndSaysWhy) {
  ShareRig rig;
  ASSERT_NO_FATAL_FAILURE(rig.play());
  ASSERT_NO_FATAL_FAILURE(rig.stream());
  ASSERT_NO_FATAL_FAILURE(rig.seated(0x43));
  auto& player = rig.newest();
  rig.session.link.drop(seat(1));
  ASSERT_TRUE(rig.until([&] {
    return rig.recorder.count<SharedSession::PlayerUnseated>() == 1;
  }));
  EXPECT_EQ(rig.recorder.find<SharedSession::PlayerUnseated>()->guest,
            rig.guest);
  EXPECT_EQ(rig.recorder.find<SharedSession::PlayerUnseated>()->seat, seat(1));
  ASSERT_TRUE(rig.until([&] {
    return rig.recorder.reached(Progress::gone) &&
           player.reached(Progress::gone);
  }));
  ASSERT_NE(rig.recorder.state(), nullptr);
  EXPECT_EQ(rig.recorder.state()->ended, EndReason::Kind::session_ended);
  EXPECT_FALSE(rig.recorder.state()->refusal);
  ASSERT_NE(player.state(), nullptr);
  EXPECT_EQ(player.state()->reason, manual::Reason::source);
  EXPECT_TRUE(player.state()->by_peer);
  EXPECT_FALSE(player.state()->error);
  EXPECT_EQ(rig.recorder.count<SharedSession::PlayerUnseated>(), 1U);
  EXPECT_FALSE(rig.session.session->seats().seat(seat(1)).is_held_by_guest());
  EXPECT_EQ(rig.recorder.count<SharedSession::SharingEnded>(), 0U);
  EXPECT_TRUE(rig.share->invite_player());
}

TEST_F(SharedSessionSeat, RefusedByTheConsoleClosesTheGuest) {
  ShareRig rig;
  ASSERT_NO_FATAL_FAILURE(rig.play());
  ASSERT_NO_FATAL_FAILURE(rig.stream());
  ASSERT_NO_FATAL_FAILURE(rig.join(0x44));
  auto& player = rig.newest();
  ASSERT_TRUE(
      rig.until([&] { return rig.session.link.count("seat_join") == 1; }));
  rig.session.link.refuse(
      libchiaking::play::SeatRefusal::by_console(seat(1), 3));
  ASSERT_TRUE(rig.until([&] {
    return rig.recorder.reached(Progress::gone) &&
           player.reached(Progress::gone);
  }));
  ASSERT_NE(rig.recorder.state(), nullptr);
  EXPECT_FALSE(rig.recorder.state()->reserved_seat);
  EXPECT_FALSE(rig.recorder.state()->console_seat);
  EXPECT_TRUE(rig.share->players().empty());
  EXPECT_EQ(rig.recorder.count<SharedSession::PlayerSeated>(), 0U);
  EXPECT_EQ(rig.recorder.count<SharedSession::PlayerUnseated>(), 0U);
  EXPECT_FALSE(rig.session.session->seats().seat(seat(1)).is_held_by_guest());
}

TEST_F(SharedSessionSeat,
       IsShownReservedUntilTheConsoleJoinsAndTheNextGuestGetsTheNext) {
  ShareRig rig;
  ASSERT_NO_FATAL_FAILURE(rig.play());
  ASSERT_NO_FATAL_FAILURE(rig.stream());
  ASSERT_NO_FATAL_FAILURE(rig.join(0x47));
  const auto first = rig.guest;
  ASSERT_TRUE(rig.until([&] {
    return rig.recorder.reserved(first) != nullptr &&
           rig.session.link.count("seat_join") == 1;
  }));
  const auto* reserved = rig.recorder.reserved(first);
  EXPECT_EQ(reserved->progress, Progress::waiting_for_seat);
  EXPECT_EQ(reserved->reserved_seat, seat(1));
  EXPECT_FALSE(reserved->console_seat);
  ASSERT_TRUE(reserved->grant.has_value());
  const auto first_grant = *reserved->grant;
  EXPECT_EQ(rig.recorder.count<SharedSession::PlayerSeated>(), 0U);
  const auto waiting = rig.share->players();
  ASSERT_EQ(waiting.size(), 1U);
  EXPECT_EQ(waiting.front().reserved_seat, seat(1));
  EXPECT_FALSE(waiting.front().console_seat);
  EXPECT_EQ(waiting.front().grant, first_grant);

  rig.session.link.join(seat(1), {0, 0, 255});
  ASSERT_TRUE(rig.until([&] {
    return rig.recorder.reached(Progress::playing) &&
           rig.players.front()->reached(Progress::playing);
  }));
  EXPECT_EQ(rig.recorder.count<SharedSession::PlayerSeated>(), 1U);
  ASSERT_NE(rig.recorder.state(), nullptr);
  EXPECT_EQ(rig.recorder.state()->console_seat, seat(1));
  EXPECT_FALSE(rig.recorder.state()->reserved_seat);
  EXPECT_EQ(rig.recorder.state()->grant, first_grant);

  ASSERT_NO_FATAL_FAILURE(rig.join(0x48));
  const auto second = rig.guest;
  EXPECT_NE(second, first);
  ASSERT_TRUE(rig.until([&] {
    return rig.recorder.reserved(second) != nullptr &&
           rig.session.link.count("seat_join") == 2;
  }));
  const auto* next = rig.recorder.reserved(second);
  EXPECT_EQ(next->reserved_seat, seat(2));
  EXPECT_FALSE(next->console_seat);
  ASSERT_TRUE(next->grant.has_value());
  EXPECT_NE(*next->grant, first_grant);
  const auto second_grant = *next->grant;
  EXPECT_EQ(rig.recorder.count<SharedSession::PlayerSeated>(), 1U);
  const auto both = rig.share->players();
  EXPECT_EQ(both.size(), 2U);
  const auto shown_first = std::ranges::find(
      both, first, &libchiaking::kakusan::PlayerSummary::guest);
  const auto shown_second = std::ranges::find(
      both, second, &libchiaking::kakusan::PlayerSummary::guest);
  ASSERT_NE(shown_first, both.end());
  ASSERT_NE(shown_second, both.end());
  EXPECT_EQ(shown_first->console_seat, seat(1));
  EXPECT_FALSE(shown_first->reserved_seat);
  EXPECT_EQ(shown_second->reserved_seat, seat(2));
  EXPECT_FALSE(shown_second->console_seat);
  EXPECT_EQ(shown_second->grant, second_grant);

  rig.session.link.join(seat(2), {0, 0, 255});
  ASSERT_TRUE(rig.until(
      [&] { return rig.recorder.count<SharedSession::PlayerSeated>() == 2; }));
  const auto* seated = rig.recorder.last<SharedSession::PlayerSeated>();
  ASSERT_NE(seated, nullptr);
  EXPECT_EQ(seated->guest, second);
  EXPECT_EQ(seated->seat, seat(2));
  ASSERT_TRUE(rig.until(
      [&] { return rig.players.back()->reached(Progress::playing); }));
  const auto joined = rig.share->players();
  EXPECT_TRUE(std::ranges::all_of(
      joined, [](const libchiaking::kakusan::PlayerSummary& player) {
        return player.console_seat && !player.reserved_seat;
      }));
}

}  // namespace
}  // namespace kakusan_test
