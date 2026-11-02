#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/seats_console_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace command = remote_play::command;

class ConsoleSeatsCloud : public ConsoleSeatsTest {
 protected:
  auto run(remote_play::StreamCommand command) {
    return couch_.execute(command, ctrl_, &delivery_, &active_, 0);
  }

  auto couch() -> Couch& { return couch_; }
  auto ctrl() -> Ctrl& { return ctrl_; }
  auto active() -> RecordingActive& { return active_; }
  auto delivery() -> Delivery& { return delivery_; }

 private:
  Couch couch_{Kind::cloud};
  Ctrl ctrl_;
  RecordingActive active_;
  Delivery delivery_{active_, 0};
};

TEST_F(ConsoleSeatsCloud, AddAndRemovePlayersWithoutTouchingCtrl) {
  EXPECT_TRUE(run(command::CouchJoin{1, 6}));
  EXPECT_EQ(active().log(), (Log{"type1=6", "announce1id", "admit1"}));
  EXPECT_TRUE(delivery().is_active(1));
  EXPECT_EQ(couch().seat_state(1), SeatState::joined);
  EXPECT_TRUE(refused(run(command::CouchJoin{1, 6}), Error::invalid_state));
  active().forget();
  EXPECT_TRUE(run(command::CouchRemove{1}));
  EXPECT_EQ(active().log(), (Log{"release1", "disconnect1"}));
  EXPECT_FALSE(delivery().is_active(1));
  active().forget();
  EXPECT_TRUE(run(command::CouchRemove{1}));
  EXPECT_TRUE(active().log().empty());
  EXPECT_TRUE(drain(ctrl()).empty());
}

TEST_F(ConsoleSeatsCloud, RefuseConsoleOnlyCommandsAndIgnoreDrops) {
  Couch::Binding binding(couch(), &delivery(), &active(), 0);
  EXPECT_TRUE(run(command::CouchSetAccountId{1, account("AQIDBAUGBwg=")}));
  EXPECT_TRUE(refused(run(command::CouchUserJoin{1}), Error::invalid_state));
  EXPECT_TRUE(refused(run(command::CouchPadJoin{1, 0}), Error::invalid_state));
  EXPECT_TRUE(refused(run(command::CouchLeave{1}), Error::invalid_state));
  EXPECT_TRUE(refused(run(command::CouchPasscode{1, access_code("1234")}),
                      Error::invalid_state));
  EXPECT_TRUE(refused(run(command::CouchJoinPs4{1, 0}), Error::invalid_state));
  EXPECT_TRUE(refused(run(command::CouchLeavePs4{1}), Error::invalid_state));
  const auto events = notice(ctrl(), binding, 0xa, {std::byte{1}});
  EXPECT_TRUE(events.empty());
  EXPECT_TRUE(drain(ctrl()).empty());
}

TEST_F(ConsoleSeatsCloud, LeaveTheSeatFreeWhenThePadCannotBeAnnounced) {
  active().fail_announcing(true);
  EXPECT_TRUE(refused(run(command::CouchJoin{2, 0}), Error::pipeline_failed));
  EXPECT_FALSE(delivery().is_active(2));
  EXPECT_TRUE(active().log().empty());
  EXPECT_EQ(couch().seat_state(2), SeatState::empty);
  active().fail_announcing(false);
  EXPECT_TRUE(run(command::CouchJoin{2, 0}));
  EXPECT_TRUE(delivery().is_active(2));
}

}  // namespace
}  // namespace play_test
