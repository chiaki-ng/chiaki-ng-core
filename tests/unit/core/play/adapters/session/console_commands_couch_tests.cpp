#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/console_commands_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <variant>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace command = libchiaking::net::protocol::remote_play::command;
using libchiaking::controllers::ControllerKind;
using libchiaking::controllers::SeatNumber;
using play::InputRefusal;
using play::SeatRefusal;

class ConsoleCouchCommands : public testing::Test {
 protected:
  static auto account() -> libchiaking::AccountId {
    constexpr std::array<std::byte, 8> raw{
        std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
        std::byte{0x89}, std::byte{0xab}, std::byte{0xcd}, std::byte{0xef}};
    return libchiaking::AccountId::from_bytes(raw);
  }

  auto console() -> ConsoleRig& { return console_; }
  auto couch() -> play::Seats::Core& { return console_.seats(); }

 private:
  ConsoleRig console_;
};

TEST_F(ConsoleCouchCommands, QueueEightRequestsAtMost) {
  EXPECT_EQ(play::Seats::Core::request_depth, 8U);
  for (std::size_t index = 0; index < play::Seats::Core::request_depth;
       ++index) {
    SCOPED_TRACE(index);
    EXPECT_TRUE(couch().remove_player(ConsoleRig::seat(1)));
  }
  EXPECT_TRUE(ConsoleRig::refused(couch().remove_player(ConsoleRig::seat(1)),
                                  InputRefusal::busy));
}

TEST_F(ConsoleCouchCommands, AreRefusedBeforeTheConsoleRuns) {
  ConsoleRig idle{ConsoleRig::Venue::home(), false};
  EXPECT_TRUE(ConsoleRig::refused(
      idle.seats().add_player(ConsoleRig::seat(1), ControllerKind::dualsense,
                              account()),
      InputRefusal::not_running));
}

TEST_F(ConsoleCouchCommands, LeaveTheHostSeatAlone) {
  EXPECT_TRUE(ConsoleRig::refused(
      couch().add_player(SeatNumber::first(), ControllerKind::dualsense,
                         account()),
      InputRefusal::invalid));
  EXPECT_TRUE(ConsoleRig::refused(couch().remove_player(SeatNumber::first()),
                                  InputRefusal::invalid));
}

TEST_F(ConsoleCouchCommands, RunInOrderOnlyWhileStreaming) {
  libchiaking::controllers::ControllerState stale;
  stale.buttons = 4;
  EXPECT_TRUE(console().queue().send(ConsoleRig::seat(1), stale));
  EXPECT_TRUE(couch().add_player(ConsoleRig::seat(1), ControllerKind::dualsense,
                                 account()));
  EXPECT_TRUE(couch().add_player(ConsoleRig::seat(2),
                                 ControllerKind::dualshock4, std::nullopt));
  EXPECT_TRUE(couch().remove_player(ConsoleRig::seat(1)));
  EXPECT_EQ(couch().queued(), 3U);
  EXPECT_TRUE(console().commands().pending(true));

  console().negotiating();
  EXPECT_EQ(couch().queued(), 3U);
  EXPECT_TRUE(console().session().commands.empty());
  const auto step = console().streaming();
  const auto& sent = console().session().commands;
  EXPECT_TRUE(step.refusals.empty());
  ASSERT_EQ(sent.size(), 4U);
  EXPECT_EQ(couch().queued(), 0U);

  const auto* named = std::get_if<command::CouchSetAccountId>(&sent[0]);
  ASSERT_NE(named, nullptr);
  EXPECT_EQ(named->pad, 1);
  EXPECT_EQ(named->account_id,
            libchiaking::AccountId::from_base64("ASNFZ4mrze8="));
  const auto* first = std::get_if<command::CouchJoin>(&sent[1]);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->pad, 1);
  EXPECT_EQ(first->controller_type, 6);
  const auto* second = std::get_if<command::CouchJoin>(&sent[2]);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->pad, 2);
  EXPECT_EQ(second->controller_type, 2);
  const auto* removed = std::get_if<command::CouchRemove>(&sent[3]);
  ASSERT_NE(removed, nullptr);
  EXPECT_EQ(removed->pad, 1);
  EXPECT_EQ(console().queue().pending(ConsoleRig::seat(1)), 0U);
}

TEST_F(ConsoleCouchCommands, ReportARefusedJoinAndFreeItsSeat) {
  console().session().refused_command =
      ConsoleRig::index_of<command::CouchJoin>();
  EXPECT_TRUE(couch().add_player(ConsoleRig::seat(3), ControllerKind::dualsense,
                                 account()));
  EXPECT_EQ(couch().seat(ConsoleRig::seat(3)).state(),
            play::SeatState::joining);
  const auto step = console().streaming();
  ASSERT_EQ(step.refusals.size(), 1U);
  EXPECT_EQ(step.refusals[0].seat(), ConsoleRig::seat(3));
  EXPECT_EQ(step.refusals[0].kind(), SeatRefusal::Kind::not_sent);
  EXPECT_EQ(step.refusals[0].cause(), play::EndReason::Cause::invalid_argument);
  EXPECT_FALSE(step.refusals[0].console_status());
  EXPECT_TRUE(couch().seat(ConsoleRig::seat(3)).is_empty());
  EXPECT_EQ(console().logged(
                "couch join for pad 3 was refused: result=invalid_argument"),
            1U);
}

TEST_F(ConsoleCouchCommands, DoNotReportARefusedRemoval) {
  console().session().refused_command =
      ConsoleRig::index_of<command::CouchRemove>();
  EXPECT_TRUE(couch().remove_player(ConsoleRig::seat(3)));
  EXPECT_TRUE(console().streaming().refusals.empty());
}

TEST_F(ConsoleCouchCommands,
       KeepOnlyTheNewestPadPasscodeAndSendItWhileNegotiating) {
  EXPECT_TRUE(
      couch().enter_passcode(ConsoleRig::seat(1), ConsoleRig::code("0000")));
  EXPECT_TRUE(
      couch().enter_passcode(ConsoleRig::seat(1), ConsoleRig::code("1234")));
  EXPECT_TRUE(couch().passcode_pending());
  EXPECT_TRUE(console().commands().pending(false));
  EXPECT_EQ(console().session().wakes, 2);
  console().negotiating();
  ASSERT_EQ(console().session().commands.size(), 1U);
  const auto* sent =
      std::get_if<command::CouchPasscode>(&console().session().commands[0]);
  ASSERT_NE(sent, nullptr);
  EXPECT_EQ(sent->pad, 1);
  EXPECT_EQ(sent->passcode.text(), "1234");
  console().negotiating();
  EXPECT_EQ(console().session().commands.size(), 1U);
  EXPECT_FALSE(couch().passcode_pending());
}

TEST_F(ConsoleCouchCommands, RefuseAPadPasscodeOnceTheSessionIsCancelled) {
  console().lifecycle().cancel();
  EXPECT_TRUE(ConsoleRig::refused(
      couch().enter_passcode(ConsoleRig::seat(1), ConsoleRig::code("1234")),
      InputRefusal::not_running));
}

TEST_F(ConsoleCouchCommands, KeepAGuestOffTheHostSeat) {
  const auto host_seat =
      couch().seat_guest(SeatNumber::first(), play::GuestTag{7},
                         ControllerKind::dualsense, std::nullopt);
  ASSERT_FALSE(host_seat);
  EXPECT_EQ(host_seat.error().kind(), SeatRefusal::Kind::no_free_seat);
}

TEST_F(ConsoleCouchCommands, LetAGuestClaimAPadUntilItIsReleased) {
  const play::GuestTag guest{7};
  const auto pad = ConsoleRig::seat(2);
  EXPECT_TRUE(
      couch().seat_guest(pad, guest, ControllerKind::dualsense, std::nullopt));
  const auto twice =
      couch().seat_guest(pad, guest, ControllerKind::dualsense, std::nullopt);
  ASSERT_FALSE(twice);
  EXPECT_EQ(twice.error().kind(), SeatRefusal::Kind::no_free_seat);
  EXPECT_TRUE(console().queue().diverted(pad));
  libchiaking::controllers::ControllerState input;
  EXPECT_TRUE(ConsoleRig::refused(console().queue().send(pad, input),
                                  InputRefusal::not_owner));
  console().streaming();
  const auto* joined =
      std::get_if<command::CouchJoin>(&console().session().commands.back());
  ASSERT_NE(joined, nullptr);
  EXPECT_EQ(joined->pad, 2);
  EXPECT_EQ(joined->controller_type, 6);
  EXPECT_TRUE(console().queue().diverted(pad));
  EXPECT_TRUE(
      ConsoleRig::refused(couch().remove_player(pad), InputRefusal::not_owner));
  couch().release_guest(pad);
  console().streaming();
  const auto* removed =
      std::get_if<command::CouchRemove>(&console().session().commands.back());
  ASSERT_NE(removed, nullptr);
  EXPECT_EQ(removed->pad, 2);
  EXPECT_FALSE(console().queue().diverted(pad));
  EXPECT_TRUE(console().queue().send(pad, input));
}

TEST_F(ConsoleCouchCommands, ReleaseEveryGuestWhenTheCommandsConclude) {
  const play::GuestTag guest{7};
  const auto pad = ConsoleRig::seat(3);
  EXPECT_TRUE(
      couch().seat_guest(pad, guest, ControllerKind::dualsense, std::nullopt));
  couch().release_guest(pad);
  EXPECT_FALSE(console().queue().diverted(pad));
  EXPECT_TRUE(
      couch().seat_guest(pad, guest, ControllerKind::dualsense, std::nullopt));
  console().commands().conclude();
  EXPECT_FALSE(console().queue().diverted(pad));
  EXPECT_EQ(couch().queued(), 0U);
}

}  // namespace
}  // namespace play_test
