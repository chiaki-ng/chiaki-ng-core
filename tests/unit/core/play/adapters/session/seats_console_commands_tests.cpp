#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/seats_console_rig.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

namespace command = libchiaking::net::protocol::remote_play::command;
using StreamCommand = libchiaking::net::protocol::remote_play::StreamCommand;
using ControllerState =
    libchiaking::net::protocol::remote_play::ControllerState;

class ConsoleSeatsCommands : public ConsoleSeatsTest {
 protected:
  auto run(StreamCommand command) {
    return couch_.execute(command, ctrl_, nullptr, nullptr, 0);
  }

  auto couch() -> Couch& { return couch_; }
  auto ctrl() -> Ctrl& { return ctrl_; }

 private:
  Couch couch_{Kind::remote_play_ps5};
  Ctrl ctrl_;
};

TEST_F(ConsoleSeatsCommands, TakeOneToFourPads) {
  EXPECT_FALSE(run(command::SetPadCount{0}));
  EXPECT_FALSE(run(command::SetPadCount{5}));
  EXPECT_TRUE(run(command::SetPadCount{3}));
  EXPECT_EQ(couch().session_pad_count(), 3U);
}

TEST_F(ConsoleSeatsCommands, QueueAValidJoinAndLeaveOnCtrl) {
  EXPECT_TRUE(run(command::SetPadCount{3}));
  EXPECT_TRUE(refused(run(command::CouchUserJoin{1}), Error::invalid_argument));
  EXPECT_FALSE(run(command::CouchSetAccountId{0, account("AQIDBAUGBwg=")}));
  EXPECT_FALSE(libchiaking::AccountId::from_base64("not-an-account"));
  EXPECT_TRUE(run(command::CouchSetAccountId{1, account("AQIDBAUGBwg=")}));
  EXPECT_TRUE(run(command::CouchUserJoin{1}));
  EXPECT_EQ(couch().seat_state(1), SeatState::join_requested);
  EXPECT_TRUE(refused(run(command::CouchUserJoin{1}), Error::invalid_state));
  EXPECT_TRUE(refused(run(command::CouchPadJoin{2, 0}), Error::invalid_state));
  EXPECT_TRUE(refused(run(command::CouchPasscode{1, access_code("1234")}),
                      Error::invalid_state));
  EXPECT_FALSE(libchiaking::play::AccessCode::parse("12a4"));
  EXPECT_TRUE(run(command::CouchLeave{3}));
  EXPECT_FALSE(run(command::CouchLeave{0}));
  const auto frames = drain(ctrl());
  ASSERT_EQ(frames.size(), 2U);
  EXPECT_EQ(frames[0],
            Codec::encode_couch_user_join(1, "AQIDBAUGBwg=").value());
  EXPECT_EQ(frames[1], Codec::encode_couch_leave(3).value());
}

TEST_F(ConsoleSeatsCommands,
       RefuseOfflinePadsHostPresenceAndUnboundAnnouncing) {
  EXPECT_TRUE(run(command::SetPadCount{3}));
  EXPECT_TRUE(refused(run(command::CouchOpenPad{2, 0}), Error::invalid_state));
  EXPECT_TRUE(
      refused(run(command::CouchPresence{0, true}), Error::invalid_argument));
  EXPECT_TRUE(refused(run(command::CouchPushPresence{0, false}),
                      Error::invalid_argument));
  EXPECT_FALSE(run(command::CouchAnnouncePads{}));
  EXPECT_TRUE(drain(ctrl()).empty());
}

TEST_F(ConsoleSeatsCommands, BeginStreamingWithOnlyThePrimaryPad) {
  RecordingActive active;
  Delivery delivery(active, 0);
  EXPECT_TRUE(run(command::SetPadCount{3}));
  EXPECT_TRUE(couch().begin_streaming(delivery, 0));
  EXPECT_TRUE(delivery.is_active(0));
  EXPECT_FALSE(delivery.is_active(1));
  EXPECT_FALSE(delivery.is_active(2));
  EXPECT_TRUE(couch().execute(command::CouchAnnouncePads{}, ctrl(), &delivery,
                              &active, 0));
  ASSERT_FALSE(active.log().empty());
  EXPECT_EQ(active.log().back(), "announce_all3");
}

class ConsoleSeatSenders : public ConsoleSeatsTest {};

TEST_F(ConsoleSeatSenders, StartWithAStateStoredBeforeStreaming) {
  Couch couch(Kind::remote_play_ps4);
  RecordingActive active;
  ControllerState pressed{};
  pressed.buttons = 1;
  pressed.left_x = 1000;
  EXPECT_TRUE(couch.store_before_streaming(pressed));
  ControllerState invalid{};
  invalid.controller_id = 4;
  EXPECT_TRUE(refused(couch.store_before_streaming(invalid),
                      Error::controller_rejected));
  Delivery delivery(active, 0);
  EXPECT_TRUE(active.log().empty());
  EXPECT_TRUE(couch.begin_streaming(delivery, 0));
  EXPECT_EQ(active.log(), (Log{"state0", "history:0"}));
}

TEST_F(ConsoleSeatSenders, StartQuietlyWithNothingStored) {
  Couch idle(Kind::remote_play_ps4);
  RecordingActive quiet;
  Delivery untouched(quiet, 0);
  EXPECT_TRUE(idle.begin_streaming(untouched, 0));
  EXPECT_TRUE(quiet.log().empty());
}

}  // namespace
}  // namespace play_test
