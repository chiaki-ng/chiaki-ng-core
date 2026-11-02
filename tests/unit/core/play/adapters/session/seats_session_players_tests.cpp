#include "tests/support/enum_printing.hpp"
#include "tests/support/play/session_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace play_test {

namespace play = libchiaking::play;

using test_support::play::SessionRig;
namespace {

using libchiaking::AccountId;
using libchiaking::controllers::ControllerKind;
using libchiaking::controllers::SeatNumber;
using play::InputRefusal;
using play::SeatRefusal;
using play::SeatState;
using Request = play::testing::ScriptedLink::SeatRequest;

class SessionSeats : public ::testing::Test {
 protected:
  static auto seat(std::size_t index) -> SeatNumber {
    return SeatNumber::create(index).value();
  }
};

TEST_F(SessionSeats, GiveTheFirstSeatToThePlayerForTheWholeSession) {
  SessionRig rig;
  auto& seats = rig.session->seats();
  const auto before = seats.seat(SeatNumber::first());
  EXPECT_EQ(before.state(), SeatState::joining);
  ASSERT_TRUE(before.holder());
  EXPECT_TRUE(before.holder()->is_local_player());
  EXPECT_EQ(before.controller(), ControllerKind::dualsense);
  EXPECT_TRUE(seats.seat(seat(1)).is_empty());
  rig.start_playing();
  EXPECT_EQ(seats.seat(SeatNumber::first()).state(), SeatState::playing);
  const auto all = seats.all();
  ASSERT_EQ(all.size(), 4U);
  EXPECT_EQ(all[0].number(), SeatNumber::first());
  EXPECT_EQ(all[0].state(), SeatState::playing);
  EXPECT_TRUE(all[1].is_empty());
  EXPECT_TRUE(all[2].is_empty());
  EXPECT_TRUE(all[3].is_empty());
  EXPECT_EQ(all[3].number(), seat(3));
  EXPECT_TRUE(rig.session->leave());
  rig.session->wait_until_ended();
  EXPECT_TRUE(seats.seat(SeatNumber::first()).is_empty());
}

TEST_F(SessionSeats, SendEachControllerKindAsItsWireType) {
  EXPECT_EQ(play::Seats::Core::wire(ControllerKind::console_default), 0);
  EXPECT_EQ(play::Seats::Core::wire(ControllerKind::dualshock4), 2);
  EXPECT_EQ(play::Seats::Core::wire(ControllerKind::dualsense), 6);
}

TEST_F(SessionSeats, SeatAPlayerTheConsoleConfirms) {
  SessionRig rig;
  auto& seats = rig.session->seats();
  EXPECT_EQ(seats.add_player(seat(1), ControllerKind::dualsense).error(),
            InputRefusal::not_running);
  EXPECT_TRUE(seats.seat(seat(1)).is_empty());
  rig.start_playing();
  EXPECT_EQ(
      seats.add_player(SeatNumber::first(), ControllerKind::dualsense).error(),
      InputRefusal::invalid);
  EXPECT_EQ(seats.remove_player(SeatNumber::first()).error(),
            InputRefusal::invalid);

  EXPECT_TRUE(seats.add_player(seat(1), ControllerKind::dualsense));
  const auto joining = seats.seat(seat(1));
  EXPECT_EQ(joining.state(), SeatState::joining);
  ASSERT_TRUE(joining.holder());
  EXPECT_TRUE(joining.holder()->is_local_player());
  EXPECT_EQ(joining.controller(), ControllerKind::dualsense);
  EXPECT_FALSE(joining.is_held_by_guest());
  EXPECT_EQ(seats.add_player(seat(1), ControllerKind::dualshock4).error(),
            InputRefusal::busy);
  SessionRig::eventually([&] { return rig.link.count("seat_join") == 1; });
  const std::vector expected{Request{Request::Kind::join, seat(1), std::nullopt,
                                     ControllerKind::dualsense}};
  EXPECT_EQ(rig.link.seat_requests(), expected);
  EXPECT_EQ(rig.link.count("seat_account"), 0U);

  rig.link.join(seat(1), {0, 0, 255});
  rig.events.until("seat_joined");
  EXPECT_EQ(rig.events.joined_seat(), seat(1));
  EXPECT_EQ(rig.events.joined_light(),
            (libchiaking::controllers::SeatLight{0, 0, 255}));
  EXPECT_EQ(seats.seat(seat(1)).state(), SeatState::playing);

  EXPECT_TRUE(seats.remove_player(seat(1)));
  EXPECT_TRUE(seats.seat(seat(1)).is_empty());
  SessionRig::eventually([&] { return rig.link.count("seat_remove") == 1; });
  EXPECT_EQ(
      rig.link.seat_requests().back(),
      (Request{Request::Kind::remove, seat(1), std::nullopt, std::nullopt}));
  rig.link.drop(seat(1));
  rig.events.until("seat_left");
  EXPECT_EQ(rig.events.left_seat(), seat(1));
  EXPECT_TRUE(seats.seat(seat(1)).is_empty());

  EXPECT_TRUE(seats.add_player(seat(2), ControllerKind::dualshock4));
  EXPECT_TRUE(seats.add_player(seat(3), ControllerKind::console_default));
  SessionRig::eventually([&] { return rig.link.count("seat_join") == 3; });
  const auto requests = rig.link.seat_requests();
  ASSERT_GE(requests.size(), 4U);
  EXPECT_EQ(requests[2].controller, ControllerKind::dualshock4);
  EXPECT_EQ(requests[2].seat, seat(2));
  EXPECT_EQ(requests[3].controller, ControllerKind::console_default);
  EXPECT_EQ(requests[3].seat, seat(3));
}

TEST_F(SessionSeats, ChooseThePlayersConsoleAccountBeforeTheJoin) {
  SessionRig rig;
  rig.start_playing();
  auto& seats = rig.session->seats();
  const AccountId account{0x1122334455667788ULL};
  EXPECT_TRUE(seats.add_player(seat(2), ControllerKind::dualsense, account));
  SessionRig::eventually([&] { return rig.link.count("seat_join") == 1; });
  const std::vector expected{
      Request{Request::Kind::choose_account, seat(2), account, std::nullopt},
      Request{Request::Kind::join, seat(2), std::nullopt,
              ControllerKind::dualsense}};
  EXPECT_EQ(rig.link.seat_requests(), expected);
  EXPECT_EQ(rig.link.count("seat_account"), 1U);

  rig.link.refuse(SeatRefusal::account_not_on_console(seat(2)));
  rig.events.until("seat_refused");
  const auto refusals = rig.events.refusals();
  ASSERT_FALSE(refusals.empty());
  const auto& refusal = refusals.front();
  EXPECT_EQ(refusal.kind(), SeatRefusal::Kind::account_not_on_console);
  EXPECT_EQ(refusal.seat(), seat(2));
  EXPECT_FALSE(refusal.console_status());
  EXPECT_FALSE(refusal.cause());
  EXPECT_TRUE(seats.seat(seat(2)).is_empty());

  EXPECT_TRUE(seats.add_player(seat(2), ControllerKind::dualsense));
  SessionRig::eventually([&] { return rig.link.count("seat_join") == 2; });
  EXPECT_EQ(rig.link.count("seat_account"), 1U);
}

TEST_F(SessionSeats, AskForAPasscodeUntilTheAttemptsRunOut) {
  SessionRig rig;
  rig.start_playing();
  auto& seats = rig.session->seats();
  EXPECT_TRUE(
      seats.add_player(seat(1), ControllerKind::dualsense, AccountId{7}));
  SessionRig::eventually([&] { return rig.link.count("seat_join") == 1; });

  rig.link.ask_for_passcode(seat(1), false);
  rig.events.until("seat_passcode_needed");
  EXPECT_EQ(rig.events.passcode_seat(), seat(1));
  EXPECT_FALSE(rig.events.passcode_retry());
  EXPECT_EQ(seats.seat(seat(1)).state(), SeatState::entering_passcode);
  EXPECT_TRUE(
      seats.enter_passcode(seat(1), play::AccessCode::parse("4321").value()));
  SessionRig::eventually([&] { return rig.link.count("seat_passcode") == 1; });
  EXPECT_EQ(rig.link.seat_requests().back().kind, Request::Kind::passcode);
  EXPECT_EQ(rig.link.seat_requests().back().seat, seat(1));

  rig.link.ask_for_passcode(seat(1), true);
  rig.events.until("seat_passcode_needed", 2);
  EXPECT_TRUE(rig.events.passcode_retry());
  EXPECT_EQ(seats.seat(seat(1)).state(), SeatState::entering_passcode);
  EXPECT_TRUE(
      seats.enter_passcode(seat(1), play::AccessCode::parse("0000").value()));
  SessionRig::eventually([&] { return rig.link.count("seat_passcode") == 2; });

  rig.link.refuse(
      SeatRefusal::by_console(seat(1), SeatRefusal::passcode_attempts_status));
  rig.events.until("seat_refused");
  const auto refusals = rig.events.refusals();
  ASSERT_FALSE(refusals.empty());
  const auto& refusal = refusals.front();
  EXPECT_EQ(refusal.kind(), SeatRefusal::Kind::passcode_attempts_used);
  EXPECT_EQ(refusal.seat(), seat(1));
  EXPECT_FALSE(refusal.console_status());
  EXPECT_FALSE(refusal.cause());
  EXPECT_TRUE(seats.seat(seat(1)).is_empty());
  EXPECT_TRUE(seats.add_player(seat(1), ControllerKind::dualsense));
}

TEST_F(SessionSeats, FreeASeatTheConsoleTimedOutOrRefused) {
  SessionRig rig;
  rig.start_playing();
  auto& seats = rig.session->seats();
  EXPECT_TRUE(seats.add_player(seat(3), ControllerKind::dualsense));
  SessionRig::eventually([&] { return rig.link.count("seat_join") == 1; });
  EXPECT_EQ(seats.seat(seat(3)).state(), SeatState::joining);
  rig.link.refuse(
      SeatRefusal::by_console(seat(3), SeatRefusal::timed_out_status));
  rig.events.until("seat_refused");
  const auto first = rig.events.refusals();
  ASSERT_FALSE(first.empty());
  EXPECT_EQ(first.front().kind(), SeatRefusal::Kind::timed_out);
  EXPECT_EQ(first.front().seat(), seat(3));
  EXPECT_FALSE(first.front().console_status());
  EXPECT_TRUE(seats.seat(seat(3)).is_empty());

  EXPECT_TRUE(seats.add_player(seat(3), ControllerKind::dualsense));
  rig.link.refuse(SeatRefusal::by_console(seat(3), 0x12));
  rig.events.until("seat_refused", 2);
  const auto second = rig.events.refusals();
  ASSERT_FALSE(second.empty());
  EXPECT_EQ(second.back().kind(), SeatRefusal::Kind::refused_by_console);
  EXPECT_EQ(second.back().console_status(), 0x12);
  EXPECT_FALSE(second.back().cause());
  EXPECT_TRUE(seats.seat(seat(3)).is_empty());
}

TEST_F(SessionSeats, RefuseAJoinTheLinkCannotSendWithItsCause) {
  SessionRig rig;
  rig.link.refuse_seat_requests(play::EndReason::Cause::control_failed);
  rig.start_playing();
  auto& seats = rig.session->seats();
  EXPECT_TRUE(seats.add_player(seat(1), ControllerKind::dualsense));
  rig.events.until("seat_refused");
  const auto refusals = rig.events.refusals();
  ASSERT_FALSE(refusals.empty());
  const auto& refusal = refusals.front();
  EXPECT_EQ(refusal, SeatRefusal::not_sent(
                         seat(1), play::EndReason::Cause::control_failed));
  EXPECT_EQ(refusal.cause(), play::EndReason::Cause::control_failed);
  EXPECT_TRUE(seats.seat(seat(1)).is_empty());
  EXPECT_EQ(rig.events.threads().size(), 1U);
}

}  // namespace
}  // namespace play_test
