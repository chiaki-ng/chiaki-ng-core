#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/seats_console_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace command = remote_play::command;
namespace event = remote_play::event;

class ConsoleSeatsPs4 : public ConsoleSeatsTest {
 protected:
  auto run(remote_play::StreamCommand command) {
    return couch_.execute(command, ctrl_, &delivery_, &active_, 0);
  }

  auto couch() -> Couch& { return couch_; }
  auto ctrl() -> Ctrl& { return ctrl_; }
  auto active() -> RecordingActive& { return active_; }
  auto delivery() -> Delivery& { return delivery_; }

 private:
  Couch couch_{Kind::remote_play_ps4};
  Ctrl ctrl_;
  RecordingActive active_;
  Delivery delivery_{active_, 0};
};

TEST_F(ConsoleSeatsPs4, AnnounceAJoinBeforeAnyInputAndLeaveInTheSameOrder) {
  remote_play::ControllerState held;
  held.controller_id = 2;
  held.buttons = 1;
  held.l2 = 255;
  couch().store(held);
  EXPECT_TRUE(run(command::CouchJoin{2, 6}));
  EXPECT_EQ(active().log(),
            (Log{"type2=6", "admit2", "announce2id", "history:2"}));
  EXPECT_TRUE(delivery().is_active(2));
  auto frames = drain(ctrl());
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0], Codec::encode_couch_pad_join(2, 3).value());
  EXPECT_TRUE(refused(run(command::CouchJoin{2, 6}), Error::invalid_state));
  active().forget();
  EXPECT_TRUE(delivery().submit(held, 1));
  EXPECT_TRUE(active().log().empty());
  EXPECT_TRUE(run(command::CouchRemove{2}));
  EXPECT_EQ(active().log(), (Log{"disconnect2", "history:2,2", "release2"}));
  EXPECT_FALSE(delivery().is_active(2));
  frames = drain(ctrl());
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0], Codec::encode_couch_leave(2).value());
  active().forget();
  EXPECT_TRUE(run(command::CouchRemove{2}));
  EXPECT_TRUE(active().log().empty());
  EXPECT_TRUE(drain(ctrl()).empty());

  EXPECT_TRUE(run(command::CouchJoinPs4{1, 0}));
  EXPECT_EQ(active().log(), (Log{"admit1", "announce1id", "history:1,2,2"}));
  EXPECT_TRUE(run(command::CouchLeavePs4{1}));
  frames = drain(ctrl());
  ASSERT_EQ(frames.size(), 2U);
  EXPECT_EQ(frames[0], Codec::encode_couch_pad_join(1, 3).value());
  EXPECT_EQ(frames[1], Codec::encode_couch_leave(1).value());
}

TEST_F(ConsoleSeatsPs4, RefuseTheHostAnAliasedPadAndAPasscode) {
  EXPECT_TRUE(refused(run(command::CouchJoin{0, 0}), Error::invalid_argument));
  EXPECT_TRUE(refused(run(command::CouchRemove{4}), Error::invalid_argument));
  EXPECT_TRUE(refused(run(command::CouchPasscode{1, access_code("1234")}),
                      Error::invalid_state));
  EXPECT_TRUE(drain(ctrl()).empty());
}

TEST_F(ConsoleSeatsPs4, WaitForTheTelevisionAndRecoverFromRefusals) {
  EXPECT_TRUE(run(command::CouchJoin{1, 0}));
  Couch::Binding later(couch(), &delivery(), &active(), 120'000'000);
  std::vector<Event> events;
  later.tick(events, ctrl());
  EXPECT_TRUE(events.empty());
  EXPECT_EQ(couch().seat_state(1), SeatState::join_requested);

  events = notice(ctrl(), later, 0x61, {std::byte{1}});
  EXPECT_TRUE(events.empty());
  events = notice(ctrl(), later, 0x8068, {std::byte{1}, std::byte{0}});
  EXPECT_TRUE(events.empty());
  events = notice(ctrl(), later, 0x8008, {std::byte{1}, std::byte{0}});
  const auto* announced = event_at<event::PadAnnounced>(events, 0);
  ASSERT_NE(announced, nullptr);
  EXPECT_EQ(announced->index, 1U);
  EXPECT_EQ(announced->led[0], 10U);
  events = notice(ctrl(), later, 0x8008, {std::byte{1}, std::byte{1}});
  const auto* failed = event_at<event::PadJoinFailed>(events, 0);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->status, 1U);
  EXPECT_EQ(couch().seat_state(1), SeatState::empty);
  EXPECT_FALSE(delivery().is_active(1));
  events = notice(ctrl(), later, 0x8008, {std::byte{1}, std::byte{1}});
  EXPECT_TRUE(events.empty());
  EXPECT_TRUE(run(command::CouchJoin{1, 0}));
  EXPECT_EQ(frame_types(ctrl()), (Types{0x08, 0x09, 0x08}));
  events = notice(ctrl(), later, 0x8008, {std::byte{3}, std::byte{0}});
  EXPECT_TRUE(events.empty());
}

TEST_F(ConsoleSeatsPs4, ResetASeatTheConsoleDroppedAndWithdrawItOnce) {
  Couch::Binding streaming(couch(), &delivery(), &active(), 0);
  EXPECT_TRUE(run(command::CouchJoin{1, 0}));
  drain(ctrl());
  active().forget();
  auto events = notice(ctrl(), streaming, 0xa, {std::byte{0}});
  EXPECT_TRUE(events.empty());
  EXPECT_TRUE(drain(ctrl()).empty());
  events = notice(ctrl(), streaming, 0xa, {std::byte{1}});
  ASSERT_EQ(events.size(), 1U);
  const auto* dropped = event_at<event::PadDropped>(events, 0);
  ASSERT_NE(dropped, nullptr);
  EXPECT_EQ(dropped->index, 1U);
  EXPECT_FALSE(delivery().is_active(1));
  EXPECT_EQ(couch().seat_state(1), SeatState::empty);
  EXPECT_EQ(active().log(), (Log{"release1"}));
  EXPECT_EQ(frame_types(ctrl()), (Types{0x09}));
  EXPECT_TRUE(run(command::CouchRemove{1}));
  EXPECT_TRUE(drain(ctrl()).empty());
  EXPECT_TRUE(run(command::CouchJoin{1, 0}));
  EXPECT_EQ(frame_types(ctrl()), (Types{0x08}));
  events = notice(ctrl(), streaming, 0xa, {std::byte{3}});
  EXPECT_TRUE(events.empty());
  EXPECT_EQ(frame_types(ctrl()), (Types{0x09}));
  events = notice(ctrl(), streaming, 0x8009, {std::byte{1}, std::byte{0}});
  EXPECT_TRUE(events.empty());
  EXPECT_TRUE(drain(ctrl()).empty());
}

}  // namespace
}  // namespace play_test
