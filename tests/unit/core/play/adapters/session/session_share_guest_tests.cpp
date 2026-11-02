#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_share_case.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace play_test {

using test_support::play::SessionRig;
namespace {

namespace play = libchiaking::play;
namespace controllers = libchiaking::controllers;
using controllers::ControllerKind;
using controllers::SeatNumber;
using libchiaking::AccountId;
using play::GuestTag;
using play::InputRefusal;
using play::SeatRefusal;
using Request = play::testing::ScriptedLink::SeatRequest;

class PlaySessionShareGuests : public ShareCase {
 protected:
  void SetUp() override {
    rig_.start_playing();
    auto begun = rig_.session->share().begin(audience_);
    ASSERT_TRUE(begun);
    sharing_.emplace(std::move(begun.value()));
  }

  auto rig() -> SessionRig& { return rig_; }
  auto share() -> play::SessionShare& { return rig_.session->share(); }
  auto audience() -> RecordedAudience& { return audience_; }
  auto sharing() -> play::Sharing& { return *sharing_; }

 private:
  SessionRig rig_;
  RecordedAudience audience_;
  std::optional<play::Sharing> sharing_;
};

TEST_F(PlaySessionShareGuests, CannotBeSeatedWithoutAnAudience) {
  SessionRig rig;
  rig.start_playing();
  EXPECT_EQ(rig.session->share().seat_guest({GuestTag{7}}).error().kind(),
            SeatRefusal::Kind::not_sent);
}

TEST_F(PlaySessionShareGuests, AreSeatedOnAFreeSeatUntilTheConsoleConfirms) {
  auto guest = share().seat_guest({GuestTag{7}, ControllerKind::dualsense});
  ASSERT_TRUE(guest);
  EXPECT_EQ(guest.value().number(), seat(1));
  EXPECT_EQ(guest.value().guest(), GuestTag{7});
  const auto held = rig().session->seats().seat(seat(1));
  EXPECT_TRUE(held.is_held_by_guest());
  ASSERT_TRUE(held.holder());
  EXPECT_EQ(held.holder()->guest(), GuestTag{7});
  EXPECT_EQ(held.state(), play::SeatState::joining);
  EXPECT_EQ(held.controller(), ControllerKind::dualsense);
  SessionRig::eventually([&] { return rig().link.count("seat_join") == 1; });
  const std::vector expected{Request{Request::Kind::join, seat(1), std::nullopt,
                                     ControllerKind::dualsense}};
  EXPECT_EQ(rig().link.seat_requests(), expected);
  EXPECT_EQ(audience().count("guest_seated"), 0U);

  rig().link.join(seat(1), {0, 0, 255});
  audience().until("guest_seated");
  EXPECT_EQ(audience().seated(), std::vector{seat(1)});
  EXPECT_EQ(rig().events.joined_seat(), seat(1));
  EXPECT_EQ(rig().session->seats().seat(seat(1)).state(),
            play::SeatState::playing);
  EXPECT_EQ(rig().session->seats().remove_player(seat(1)).error(),
            InputRefusal::not_owner);
}

TEST_F(PlaySessionShareGuests, RunOutOfFreeSeats) {
  auto guest = share().seat_guest({GuestTag{7}, ControllerKind::dualsense});
  ASSERT_TRUE(guest);
  rig().link.join(seat(1), {0, 0, 255});
  audience().until("guest_seated");
  auto second = share().seat_guest({GuestTag{8}, ControllerKind::dualshock4});
  ASSERT_TRUE(second);
  EXPECT_EQ(second.value().number(), seat(2));
  EXPECT_TRUE(
      rig().session->seats().add_player(seat(3), ControllerKind::dualsense));
  rig().link.join(seat(3), {1, 2, 3});
  rig().events.until("seat_joined", 2);
  EXPECT_EQ(audience().count("guest_seated"), 1U);
  EXPECT_EQ(share().seat_guest({GuestTag{9}}).error(),
            SeatRefusal::no_free_seat());
  EXPECT_EQ(audience().count("guest_seat_refused"), 0U);
  EXPECT_EQ(audience().threads(), rig().events.threads());
}

TEST_F(PlaySessionShareGuests, CanAskForASeatAndAConsoleAccount) {
  const AccountId account{0x1122334455667788ULL};
  auto guest = share().seat_guest(
      {GuestTag{3}, ControllerKind::dualshock4, account, seat(3)});
  ASSERT_TRUE(guest);
  EXPECT_EQ(guest.value().number(), seat(3));
  SessionRig::eventually([&] { return rig().link.count("seat_join") == 1; });
  const std::vector expected{
      Request{Request::Kind::choose_account, seat(3), account, std::nullopt},
      Request{Request::Kind::join, seat(3), std::nullopt,
              ControllerKind::dualshock4}};
  EXPECT_EQ(rig().link.seat_requests(), expected);

  EXPECT_EQ(share()
                .seat_guest({GuestTag{4}, ControllerKind::dualsense,
                             std::nullopt, seat(3)})
                .error(),
            SeatRefusal::no_free_seat());
  EXPECT_EQ(share()
                .seat_guest({GuestTag{4}, ControllerKind::dualsense,
                             std::nullopt, SeatNumber::first()})
                .error(),
            SeatRefusal::no_free_seat());
  EXPECT_TRUE(
      rig().session->seats().add_player(seat(1), ControllerKind::dualsense));
  EXPECT_EQ(share()
                .seat_guest({GuestTag{4}, ControllerKind::dualsense,
                             std::nullopt, seat(1)})
                .error(),
            SeatRefusal::no_free_seat());
  auto other = share().seat_guest({GuestTag{4}});
  ASSERT_TRUE(other);
  EXPECT_EQ(other.value().number(), seat(2));
  EXPECT_EQ(rig().link.count("seat_account"), 1U);
}

TEST_F(PlaySessionShareGuests, AreToldWhenTheConsoleRefusesThem) {
  controllers::ControllerState state;
  state.buttons = 1;
  {
    auto guest = share().seat_guest({GuestTag{5}});
    ASSERT_TRUE(guest);
    EXPECT_EQ(guest.value().number(), seat(1));
    SessionRig::eventually([&] { return rig().link.count("seat_join") == 1; });
    rig().link.refuse(SeatRefusal::by_console(seat(1), 5));
    audience().until("guest_seat_refused");
    EXPECT_EQ(audience().refused(),
              std::vector{SeatRefusal::by_console(seat(1), 5)});
    EXPECT_EQ(rig().events.refusals(), audience().refused());
    EXPECT_EQ(audience().count("guest_seated"), 0U);
    EXPECT_TRUE(rig().session->seats().seat(seat(1)).is_empty());
    EXPECT_EQ(guest.value().send(state).error(), InputRefusal::not_owner);
  }
  EXPECT_EQ(rig().link.count("seat_remove"), 0U);

  auto unknown = share().seat_guest(
      {GuestTag{6}, ControllerKind::dualsense, AccountId{9}});
  ASSERT_TRUE(unknown);
  EXPECT_EQ(unknown.value().number(), seat(1));
  SessionRig::eventually([&] { return rig().link.count("seat_join") == 2; });
  rig().link.refuse(SeatRefusal::account_not_on_console(seat(1)));
  audience().until("guest_seat_refused", 2);
  ASSERT_FALSE(audience().refused().empty());
  EXPECT_EQ(audience().refused().back(),
            SeatRefusal::account_not_on_console(seat(1)));

  EXPECT_TRUE(
      rig().session->seats().add_player(seat(2), ControllerKind::dualsense));
  SessionRig::eventually([&] { return rig().link.count("seat_join") == 3; });
  rig().link.refuse(SeatRefusal::by_console(seat(2), 6));
  rig().events.until("seat_refused", 3);
  EXPECT_EQ(audience().count("guest_seat_refused"), 2U);
}

TEST_F(PlaySessionShareGuests, AreToldWhenTheirRequestCouldNotBeSent) {
  SessionRig unsent;
  unsent.link.refuse_seat_requests(play::EndReason::Cause::control_failed);
  unsent.start_playing();
  RecordedAudience other;
  auto shared = unsent.session->share().begin(other);
  ASSERT_TRUE(shared);
  auto queued = unsent.session->share().seat_guest({GuestTag{1}});
  EXPECT_TRUE(queued);
  other.until("guest_seat_refused");
  ASSERT_FALSE(other.refused().empty());
  EXPECT_EQ(
      other.refused().front(),
      SeatRefusal::not_sent(seat(1), play::EndReason::Cause::control_failed));
  EXPECT_TRUE(unsent.session->seats().seat(seat(1)).is_empty());
}

TEST_F(PlaySessionShareGuests, FreeTheirSeatWithTheirToken) {
  {
    auto guest = share().seat_guest({GuestTag{1}});
    ASSERT_TRUE(guest);
    EXPECT_EQ(guest.value().number(), seat(1));
    rig().link.join(seat(1), {});
    audience().until("guest_seated");
    play::SharedSeat moved{std::move(guest.value())};
    EXPECT_EQ(moved.number(), seat(1));
    EXPECT_EQ(rig().link.count("seat_remove"), 0U);
    EXPECT_TRUE(rig().session->seats().seat(seat(1)).is_held_by_guest());
  }
  EXPECT_TRUE(rig().session->seats().seat(seat(1)).is_empty());
  SessionRig::eventually([&] { return rig().link.count("seat_remove") == 1; });
  EXPECT_EQ(
      rig().link.seat_requests().back(),
      (Request{Request::Kind::remove, seat(1), std::nullopt, std::nullopt}));
  EXPECT_TRUE(
      rig().session->seats().add_player(seat(1), ControllerKind::dualsense));
  EXPECT_TRUE(rig().session->seats().remove_player(seat(1)));
  SessionRig::eventually([&] { return rig().link.count("seat_remove") == 2; });
}

TEST_F(PlaySessionShareGuests, LoseTheirSeatsWhenSharingStops) {
  auto first = share().seat_guest({GuestTag{2}});
  auto second = share().seat_guest({GuestTag{3}});
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first.value().number(), seat(1));
  EXPECT_EQ(second.value().number(), seat(2));
  controllers::ControllerState state;
  state.buttons = 2;
  EXPECT_TRUE(first.value().send(state));
  sharing().stop();
  EXPECT_TRUE(rig().session->seats().seat(seat(1)).is_empty());
  EXPECT_TRUE(rig().session->seats().seat(seat(2)).is_empty());
  SessionRig::eventually([&] { return rig().link.count("seat_remove") == 2; });
  EXPECT_EQ(first.value().send(state).error(), InputRefusal::not_owner);
  EXPECT_EQ(second.value().send(state).error(), InputRefusal::not_owner);
  EXPECT_EQ(share().seat_guest({GuestTag{4}}).error().kind(),
            SeatRefusal::Kind::not_sent);
  EXPECT_TRUE(rig().session->controllers().send(seat(1), state));
  EXPECT_EQ(rig().link.count("seat_remove"), 2U);

  RecordedAudience next;
  auto again = share().begin(next);
  ASSERT_TRUE(again);
  auto third = share().seat_guest({GuestTag{5}});
  ASSERT_TRUE(third);
  EXPECT_EQ(third.value().number(), seat(1));
  first.value() = std::move(third.value());
  EXPECT_TRUE(rig().session->seats().seat(seat(1)).is_held_by_guest());
  EXPECT_EQ(audience().count("session_ended"), 0U);
}

TEST_F(PlaySessionShareGuests, SendInputToTheirOwnSeat) {
  auto left = share().seat_guest({GuestTag{1}});
  auto right = share().seat_guest(
      {GuestTag{2}, ControllerKind::dualsense, std::nullopt, seat(3)});
  ASSERT_TRUE(left);
  ASSERT_TRUE(right);
  controllers::ControllerState pressed;
  pressed.buttons =
      static_cast<std::uint32_t>(controllers::ControllerButton::cross);
  pressed.left_x = -200;
  controllers::ControllerState other;
  other.buttons =
      static_cast<std::uint32_t>(controllers::ControllerButton::moon);
  other.r2 = 77;
  EXPECT_TRUE(left.value().send(pressed));
  EXPECT_TRUE(right.value().send(other));
  EXPECT_TRUE(right.value().send(pressed));
  SessionRig::eventually([&] {
    return rig().link.controller_samples(seat(1)).size() == 1 &&
           rig().link.controller_samples(seat(3)).size() == 2;
  });
  EXPECT_EQ(rig().link.controller_samples(seat(1)).front().buttons,
            pressed.buttons);
  EXPECT_EQ(rig().link.controller_samples(seat(1)).front().left_x, -200);
  EXPECT_EQ(rig().link.controller_samples(seat(3)).front().r2, 77);
  EXPECT_EQ(rig().link.controller_samples(seat(3)).back().buttons,
            pressed.buttons);
  EXPECT_TRUE(rig().link.controller_samples(seat(2)).empty());
  EXPECT_TRUE(rig().link.controller_samples(SeatNumber::first()).empty());
  EXPECT_EQ(rig().session->controllers().send(seat(1), pressed).error(),
            InputRefusal::not_owner);
  controllers::ControllerState invalid;
  invalid.buttons = 0x10000;
  EXPECT_EQ(left.value().send(invalid).error(), InputRefusal::invalid);
}

TEST_F(PlaySessionShareGuests, LetTheAudienceHearFeedbackForTheirSeat) {
  rig().link.feed_back({seat(2), controllers::Rumble{200, 100}});
  SessionRig::eventually([&] { return rig().media.feedback().size() == 1; });
  EXPECT_EQ(audience().count("feedback"), 0U);

  auto guest = share().seat_guest(
      {GuestTag{1}, ControllerKind::dualsense, std::nullopt, seat(2)});
  ASSERT_TRUE(guest);
  rig().link.feed_back({seat(2), controllers::Rumble{10, 20}});
  audience().until("feedback");
  ASSERT_FALSE(audience().feedback().empty());
  const auto heard = audience().feedback().front();
  EXPECT_EQ(heard.kind(), controllers::ControllerFeedback::Kind::rumble);
  EXPECT_EQ(heard.seat, seat(2));
  ASSERT_NE(heard.get<controllers::Rumble>(), nullptr);
  EXPECT_EQ(*heard.get<controllers::Rumble>(), (controllers::Rumble{10, 20}));
  EXPECT_EQ(rig().media.feedback().size(), 2U);
  EXPECT_EQ(audience().threads(), rig().events.threads());
}

TEST_F(PlaySessionShareGuests, AreToldWhenTheConsoleDropsThem) {
  controllers::ControllerState state;
  state.buttons = 1;
  {
    auto guest = share().seat_guest({GuestTag{1}});
    ASSERT_TRUE(guest);
    EXPECT_EQ(guest.value().number(), seat(1));
    rig().link.join(seat(1), {});
    audience().until("guest_seated");
    EXPECT_TRUE(guest.value().send(state));
    EXPECT_EQ(audience().count("guest_unseated"), 0U);

    rig().link.drop(seat(1));
    audience().until("guest_unseated");
    EXPECT_EQ(audience().unseated(), std::vector{seat(1)});
    EXPECT_EQ(rig().events.left_seat(), seat(1));
    EXPECT_TRUE(rig().session->seats().seat(seat(1)).is_empty());
    EXPECT_EQ(guest.value().send(state).error(), InputRefusal::not_owner);
    EXPECT_EQ(guest.value().number(), seat(1));
    EXPECT_EQ(audience().count("guest_seat_refused"), 0U);
  }
  EXPECT_EQ(rig().link.count("seat_remove"), 0U);
}

TEST_F(PlaySessionShareGuests, AreNotToldAboutSeatsTheyNoLongerHold) {
  EXPECT_TRUE(
      rig().session->seats().add_player(seat(2), ControllerKind::dualsense));
  rig().link.join(seat(2), {});
  rig().events.until("seat_joined");
  rig().link.drop(seat(2));
  rig().events.until("seat_left");
  EXPECT_EQ(audience().count("guest_unseated"), 0U);

  {
    auto leaving = share().seat_guest({GuestTag{2}});
    ASSERT_TRUE(leaving);
    EXPECT_EQ(leaving.value().number(), seat(1));
    rig().link.join(seat(1), {});
    audience().until("guest_seated");
  }
  SessionRig::eventually([&] { return rig().link.count("seat_remove") == 1; });
  rig().link.drop(seat(1));
  rig().events.until("seat_left", 2);
  EXPECT_EQ(audience().count("guest_unseated"), 0U);

  auto kept = share().seat_guest({GuestTag{3}});
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept.value().number(), seat(1));
  rig().link.join(seat(1), {});
  audience().until("guest_seated", 2);
  sharing().stop();
  rig().link.drop(seat(1));
  rig().events.until("seat_left", 3);
  EXPECT_EQ(audience().count("guest_unseated"), 0U);
  EXPECT_EQ(audience().threads(), rig().events.threads());
}

}  // namespace
}  // namespace play_test
