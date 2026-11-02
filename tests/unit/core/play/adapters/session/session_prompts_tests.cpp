#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_case.hpp"

#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace play_test {

using test_support::play::SessionRig;
namespace {

namespace play = libchiaking::play;
using namespace std::chrono_literals;
using libchiaking::catalog::ServiceGeneration;
using play::EndReason;
using play::InputRefusal;
using play::Where;

class PlaySessionPrompts : public SessionCase {
 protected:
  void SetUp() override { rig_.start_playing(); }

  auto rig() -> SessionRig& { return rig_; }

 private:
  SessionRig rig_;
};

TEST_F(PlaySessionPrompts, AskForTheLoginPin) {
  rig().link.ask_for_login_pin(true);
  rig().events.until("login_pin_needed");
  EXPECT_EQ(rig().events.pin_was_wrong(), std::optional{true});
}

TEST_F(PlaySessionPrompts, SayThePictureIsBlocked) {
  rig().link.block_picture(true);
  rig().events.until("picture_blocked");
  EXPECT_EQ(rig().events.picture_blocked(), std::optional{true});
}

TEST_F(PlaySessionPrompts, FollowTheConsoleKeyboard) {
  EXPECT_FALSE(rig().session->keyboard().active());
  rig().link.open_keyboard("hello");
  rig().events.until("keyboard_opened");
  EXPECT_EQ(rig().events.keyboard_text(), "hello");
  EXPECT_TRUE(rig().session->keyboard().active());
  rig().link.change_keyboard_text("hello there");
  rig().events.until("keyboard_text");
  EXPECT_EQ(rig().events.keyboard_text(), "hello there");
  rig().link.close_keyboard();
  rig().events.until("keyboard_closed");
  EXPECT_FALSE(rig().session->keyboard().active());
}

TEST_F(PlaySessionPrompts, AskForASeatPasscode) {
  rig().link.ask_for_passcode(seat(2), true);
  rig().events.until("seat_passcode_needed");
  EXPECT_EQ(rig().events.passcode_seat(), seat(2));
  EXPECT_TRUE(rig().events.passcode_retry());
}

TEST_F(PlaySessionPrompts, FollowSeatsJoiningAndLeaving) {
  rig().link.join(seat(1), {10, 20, 30});
  rig().events.until("seat_joined");
  EXPECT_EQ(rig().events.joined_seat(), seat(1));
  EXPECT_EQ(rig().events.joined_light(),
            (libchiaking::controllers::SeatLight{10, 20, 30}));

  rig().link.drop(seat(1));
  rig().events.until("seat_left");
  EXPECT_EQ(rig().events.left_seat(), seat(1));
}

TEST_F(PlaySessionPrompts, PassOnASeatTheConsoleRefused) {
  rig().link.refuse(play::SeatRefusal::by_console(seat(3), 5));
  rig().events.until("seat_refused");
  ASSERT_FALSE(rig().events.refusals().empty());
  EXPECT_EQ(rig().events.refusals().front(),
            play::SeatRefusal::by_console(seat(3), 5));
}

TEST_F(PlaySessionPrompts, ArriveOnTheCoresOwnThread) {
  rig().link.ask_for_login_pin(true);
  rig().events.until("login_pin_needed");
  rig().link.open_keyboard("hello");
  rig().events.until("keyboard_opened");
  rig().link.join(seat(1), {10, 20, 30});
  rig().events.until("seat_joined");
  EXPECT_TRUE(rig().session->leave());
  rig().session->wait_until_ended();
  EXPECT_EQ(rig().events.threads().size(), 1U);
  EXPECT_FALSE(rig().events.threads().contains(
      libchiaking::platform::ThreadIdentity::current()));
}

using PlaySessionLoginPin = SessionCase;

TEST_F(PlaySessionLoginPin, IsSentOnceThePlayerEntersIt) {
  SessionRig rig;
  rig.start_playing();
  rig.link.ask_for_login_pin(false);
  rig.events.until("login_pin_needed");
  EXPECT_EQ(rig.events.pin_was_wrong(), std::optional{false});
  auto& pin = rig.session->login_pin();
  const auto ticket = pin.submit(play::AccessCode::parse("1234").value());
  ASSERT_TRUE(ticket);
  EXPECT_EQ(pin.await(10s), play::LoginPin::Verdict::accepted);
  EXPECT_EQ(pin.progress(ticket.value()), play::LoginPin::Progress::accepted);
  EXPECT_EQ(rig.link.login_pin().view(), "1234");
  EXPECT_EQ(rig.link.count("login_pin"), 1U);
  EXPECT_EQ(pin.progress(play::LoginPin::Ticket{ticket.value().id() + 50}),
            play::LoginPin::Progress::rejected);
}

TEST_F(PlaySessionLoginPin, ReportsAPinTheConsoleRejected) {
  SessionRig refusing;
  refusing.link.refuse_login_pin(EndReason::Cause::login_pin_rejected);
  refusing.start_playing();
  const auto refused = refusing.session->login_pin().submit(
      play::AccessCode::parse("0000").value());
  ASSERT_TRUE(refused);
  EXPECT_EQ(refusing.session->login_pin().await(10s),
            play::LoginPin::Verdict::rejected);
  EXPECT_EQ(refusing.session->login_pin().progress(refused.value()),
            play::LoginPin::Progress::rejected);
}

TEST_F(PlaySessionLoginPin, IsRefusedBeforeTheSessionRuns) {
  SessionRig idle;
  const auto early =
      idle.session->login_pin().submit(play::AccessCode::parse("1111").value());
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(), InputRefusal::not_running);
}

using PlaySessionKeyboard = SessionCase;

TEST_F(PlaySessionKeyboard, AcceptsTextOnceTheConsoleOpensIt) {
  SessionRig rig;
  rig.start_playing();
  auto& keyboard = rig.session->keyboard();
  EXPECT_EQ(keyboard.set_text("early").error(), InputRefusal::not_ready);
  rig.link.open_keyboard("");
  rig.events.until("keyboard_opened");
  EXPECT_TRUE(keyboard.set_text("typed"));
  EXPECT_TRUE(keyboard.accept());
  SessionRig::eventually(
      [&] { return rig.link.count("keyboard_accept") == 1; });
  EXPECT_EQ(rig.link.count("keyboard_text"), 1U);
  EXPECT_FALSE(keyboard.active());
}

TEST_F(PlaySessionKeyboard, CanBeRejected) {
  SessionRig rig;
  rig.start_playing();
  auto& keyboard = rig.session->keyboard();
  rig.link.open_keyboard("");
  rig.events.until("keyboard_opened");
  EXPECT_TRUE(keyboard.accept());
  rig.link.open_keyboard("again");
  rig.events.until("keyboard_opened", 2);
  EXPECT_TRUE(keyboard.reject());
  SessionRig::eventually(
      [&] { return rig.link.count("keyboard_reject") == 1; });
}

TEST_F(PlaySessionKeyboard, IsNotOfferedInTheCloud) {
  SessionRig cloud{Where::cloud, ServiceGeneration::ps5_cloud};
  cloud.start_playing();
  EXPECT_EQ(cloud.session->keyboard().accept().error(),
            InputRefusal::wrong_route);
}

}  // namespace
}  // namespace play_test
