#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/seats_console_rig.hpp"

#include <gtest/gtest.h>

#include <string>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace command = remote_play::command;
namespace event = remote_play::event;
using libchiaking::play::SeatRefusal;

class ConsoleSeatsPs5 : public ConsoleSeatsTest {
 protected:
  auto run(remote_play::StreamCommand command) {
    return couch_.execute(command, ctrl_, &delivery_, &active_, now_);
  }

  auto tick_at(std::uint64_t at) -> std::vector<Event> {
    Couch::Binding binding(couch_, &delivery_, &active_, at);
    std::vector<Event> events;
    binding.tick(events, ctrl_);
    return events;
  }

  void set_now(std::uint64_t now) { now_ = now; }
  auto now() const -> std::uint64_t { return now_; }
  auto couch() -> Couch& { return couch_; }
  auto ctrl() -> Ctrl& { return ctrl_; }
  auto active() -> RecordingActive& { return active_; }
  auto delivery() -> Delivery& { return delivery_; }

 private:
  Couch couch_{Kind::remote_play_ps5};
  Ctrl ctrl_;
  RecordingActive active_;
  Delivery delivery_{active_, 0};
  std::uint64_t now_{};
};

TEST_F(ConsoleSeatsPs5, AnnounceAJoinOnlyAfterTheConsoleAcceptsIt) {
  Couch::Binding binding(couch(), &delivery(), &active(), 0);
  EXPECT_TRUE(run(command::CouchSetAccountId{1, account("AQIDBAUGBwg=")}));
  EXPECT_TRUE(run(command::CouchJoin{1, 0}));
  EXPECT_TRUE(active().log().empty());
  EXPECT_FALSE(delivery().is_active(1));
  auto frames = drain(ctrl());
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0],
            Codec::encode_couch_user_join(1, "AQIDBAUGBwg=").value());

  auto events = notice(ctrl(), binding, 0x8068, {std::byte{1}, std::byte{0}});
  ASSERT_EQ(events.size(), 2U);
  const auto* announced = event_at<event::PadAnnounced>(events, 1);
  ASSERT_NE(announced, nullptr);
  EXPECT_EQ(announced->index, 1U);
  EXPECT_EQ(announced->led, (std::array<std::uint8_t, 3>{10, 1, 2}));
  const auto* confirmed = event_at<event::EncodedMedia>(events, 0);
  ASSERT_NE(confirmed, nullptr);
  EXPECT_EQ(confirmed->kind, remote_play::EncodedMediaKind::pad_confirmed);
  EXPECT_EQ(confirmed->bytes,
            (std::vector<std::byte>{std::byte{1}, std::byte{10}, std::byte{1},
                                    std::byte{2}}));
  EXPECT_EQ(active().log(), (Log{"admit1q", "announce1id"}));
  EXPECT_TRUE(delivery().is_active(1));
  EXPECT_EQ(couch().seat_state(1), SeatState::joined);
  events = notice(ctrl(), binding, 0x8068, {std::byte{1}, std::byte{0}});
  EXPECT_TRUE(events.empty());

  active().forget();
  EXPECT_TRUE(run(command::CouchRemove{1}));
  EXPECT_EQ(active().log(), (Log{"disconnect1", "history:1", "release1"}));
  EXPECT_EQ(frame_types(ctrl()), (Types{0x09}));
}

TEST_F(ConsoleSeatsPs5, FreeTheSeatWhenTheConsoleRefusesAJoin) {
  Couch::Binding binding(couch(), &delivery(), &active(), 0);
  EXPECT_TRUE(run(command::CouchSetAccountId{1, account("AQIDBAUGBwg=")}));
  EXPECT_TRUE(run(command::CouchJoin{1, 0}));
  const auto events =
      notice(ctrl(), binding, 0x8068, {std::byte{1}, std::byte{1}});
  const auto* failed = event_at<event::PadJoinFailed>(events, 0);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->status, 1U);
  EXPECT_EQ(couch().seat_state(1), SeatState::empty);
  EXPECT_FALSE(delivery().is_active(1));
  EXPECT_EQ(frame_types(ctrl()), (Types{0x68, 0x09}));
}

TEST_F(ConsoleSeatsPs5, FreeTheSeatWhenThePadCannotBeAnnounced) {
  Couch::Binding binding(couch(), &delivery(), &active(), 0);
  EXPECT_TRUE(run(command::CouchSetAccountId{1, account("AQIDBAUGBwg=")}));
  active().fail_announcing(true);
  EXPECT_TRUE(run(command::CouchJoin{1, 0}));
  const auto events =
      notice(ctrl(), binding, 0x8068, {std::byte{1}, std::byte{0}});
  const auto* failed = event_at<event::PadJoinFailed>(events, 0);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->status, 1U);
  EXPECT_FALSE(delivery().is_active(1));
  EXPECT_EQ(frame_types(ctrl()), (Types{0x68, 0x09}));
}

TEST_F(ConsoleSeatsPs5, IgnoreAnAnswerForASeatNotJoiningAndRefusePs4Joins) {
  Couch::Binding binding(couch(), &delivery(), &active(), 0);
  const auto events =
      notice(ctrl(), binding, 0x8068, {std::byte{3}, std::byte{0}});
  EXPECT_TRUE(events.empty());
  EXPECT_FALSE(delivery().is_active(3));
  EXPECT_TRUE(refused(run(command::CouchJoinPs4{2, 0}), Error::invalid_state));
}

TEST_F(ConsoleSeatsPs5, AnswerAPasscodeOnlyWhenAskedAndForAtMostThreeTries) {
  Couch::Binding binding(couch(), &delivery(), &active(), 0);
  EXPECT_TRUE(run(command::CouchSetAccountId{1, account("AQIDBAUGBwg=")}));
  EXPECT_TRUE(run(command::CouchJoin{1, 0}));
  auto events = notice(ctrl(), binding, 0x8068, {std::byte{1}, std::byte{3}});
  const auto* asked = event_at<event::PadPasscodeRequest>(events, 0);
  ASSERT_NE(asked, nullptr);
  EXPECT_FALSE(asked->retry);
  EXPECT_EQ(couch().seat_state(1), SeatState::entering_passcode);
  events = notice(ctrl(), binding, 0x64,
                  {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}});
  EXPECT_TRUE(events.empty());
  Couch::Binding much_later(couch(), &delivery(), &active(), 120'000'000);
  much_later.tick(events, ctrl());
  EXPECT_TRUE(events.empty());

  EXPECT_FALSE(libchiaking::play::AccessCode::parse("123"));
  EXPECT_EQ(couch().seat_state(1), SeatState::entering_passcode);
  for (const std::string_view code : {"1111", "2222", "3333"}) {
    SCOPED_TRACE(code);
    EXPECT_TRUE(run(command::CouchPasscode{1, access_code(code)}));
    EXPECT_TRUE(refused(run(command::CouchPasscode{1, access_code(code)}),
                        Error::invalid_state));
    events = notice(ctrl(), binding, 0x64,
                    {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}});
    if (code != "3333") {
      const auto* retried = event_at<event::PadPasscodeRequest>(events, 0);
      ASSERT_NE(retried, nullptr);
      EXPECT_TRUE(retried->retry);
    }
  }
  ASSERT_EQ(events.size(), 1U);
  const auto* failed = event_at<event::PadJoinFailed>(events, 0);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->status, SeatRefusal::passcode_attempts_status);
  EXPECT_EQ(couch().seat_state(1), SeatState::empty);
  EXPECT_EQ(frame_types(ctrl()), (Types{0x68, 0x8064, 0x8064, 0x8064, 0x09}));
}

TEST_F(ConsoleSeatsPs5, RefuseAPasscodeAfterTheJoinIsCancelled) {
  Couch::Binding binding(couch(), &delivery(), &active(), 0);
  EXPECT_TRUE(run(command::CouchSetAccountId{1, account("AQIDBAUGBwg=")}));
  EXPECT_TRUE(run(command::CouchJoin{1, 0}));
  const auto events =
      notice(ctrl(), binding, 0x64,
             {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}});
  const auto* asked = event_at<event::PadPasscodeRequest>(events, 0);
  ASSERT_NE(asked, nullptr);
  EXPECT_FALSE(asked->retry);
  EXPECT_TRUE(run(command::CouchRemove{1}));
  EXPECT_EQ(frame_types(ctrl()), (Types{0x68, 0x09}));
  EXPECT_TRUE(refused(run(command::CouchPasscode{1, access_code("1234")}),
                      Error::invalid_state));
}

TEST_F(ConsoleSeatsPs5, RefuseAPasscodeAfterTheConsoleDropsThePad) {
  Couch::Binding binding(couch(), &delivery(), &active(), 0);
  EXPECT_TRUE(run(command::CouchSetAccountId{1, account("AQIDBAUGBwg=")}));
  EXPECT_TRUE(run(command::CouchJoin{1, 0}));
  notice(ctrl(), binding, 0x64,
         {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}});
  const auto events = notice(ctrl(), binding, 0xa, {std::byte{1}});
  const auto* dropped = event_at<event::PadDropped>(events, 0);
  ASSERT_NE(dropped, nullptr);
  EXPECT_EQ(dropped->index, 1U);
  EXPECT_TRUE(refused(run(command::CouchPasscode{1, access_code("1234")}),
                      Error::invalid_state));
  EXPECT_EQ(frame_types(ctrl()), (Types{0x68, 0x09}));
}

TEST_F(ConsoleSeatsPs5, TimeOutAJoinTheConsoleNeverAnswers) {
  EXPECT_TRUE(run(command::CouchSetAccountId{1, account("AQIDBAUGBwg=")}));
  EXPECT_TRUE(run(command::CouchSetAccountId{2, account("AQIDBAUGBwg=")}));
  EXPECT_TRUE(run(command::CouchJoin{1, 0}));
  EXPECT_TRUE(refused(run(command::CouchJoin{2, 0}), Error::invalid_argument));
  EXPECT_TRUE(tick_at(9'000'000).empty());
  const auto timed_out = tick_at(11'000'000);
  ASSERT_EQ(timed_out.size(), 1U);
  const auto* failed = event_at<event::PadJoinFailed>(timed_out, 0);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->status, SeatRefusal::timed_out_status);
  EXPECT_TRUE(tick_at(12'000'000).empty());
  EXPECT_EQ(frame_types(ctrl()), (Types{0x68, 0x09}));
}

TEST_F(ConsoleSeatsPs5, SetAsideAnAccountTheConsoleCannotResolve) {
  EXPECT_TRUE(run(command::CouchSetAccountId{1, account("AQIDBAUGBwg=")}));
  EXPECT_TRUE(run(command::CouchSetAccountId{2, account("AQIDBAUGBwg=")}));
  EXPECT_TRUE(run(command::CouchJoin{1, 0}));
  static_cast<void>(tick_at(11'000'000));
  drain(ctrl());

  set_now(20'000'000);
  EXPECT_TRUE(run(command::CouchSetAccountId{3, account("CAcGBQQDAgE=")}));
  EXPECT_TRUE(run(command::CouchJoin{3, 0}));
  Couch::Binding binding(couch(), &delivery(), &active(), now());
  auto events = notice(ctrl(), binding, 0x61, {std::byte{3}});
  ASSERT_EQ(events.size(), 1U);
  const auto* unresolvable = event_at<event::PadAccountUnresolvable>(events, 0);
  ASSERT_NE(unresolvable, nullptr);
  EXPECT_EQ(unresolvable->index, 3U);
  EXPECT_EQ(couch().seat_state(3), SeatState::empty);
  EXPECT_EQ(frame_types(ctrl()), (Types{0x68, 0x09}));
  events = notice(ctrl(), binding, 0x61, {std::byte{3}});
  EXPECT_TRUE(events.empty());
  EXPECT_TRUE(drain(ctrl()).empty());
  EXPECT_TRUE(run(command::CouchSetAccountId{2, account("CAcGBQQDAgE=")}));
  EXPECT_TRUE(refused(run(command::CouchJoin{2, 0}), Error::invalid_argument));
  EXPECT_TRUE(run(command::CouchSetAccountId{2, account("AQIDBAUGBwg=")}));
  EXPECT_TRUE(run(command::CouchJoin{2, 0}));
}

TEST_F(ConsoleSeatsPs5, WalksEachSeatThroughTheStatesTheTableSays) {
  Couch::Binding binding(couch(), &delivery(), &active(), 0);
  std::string trace;
  const auto note = [&] {
    trace += "|";
    for (std::uint8_t pad = 0; pad < 4; ++pad)
      trace += std::to_string(static_cast<int>(couch().seat_state(pad)));
  };
  const auto ask = [&](std::uint8_t pad) {
    static_cast<void>(
        notice(ctrl(), binding, 0x64,
               {std::byte{pad}, std::byte{0}, std::byte{0}, std::byte{0}}));
  };
  const auto answer = [&](std::uint8_t pad, std::uint8_t status) {
    static_cast<void>(
        notice(ctrl(), binding, 0x8068, {std::byte{pad}, std::byte{status}}));
  };
  for (std::uint8_t pad = 1; pad < 4; ++pad)
    static_cast<void>(
        run(command::CouchSetAccountId{pad, account("AQIDBAUGBwg=")}));
  note();
  static_cast<void>(run(command::CouchJoin{1, 0}));
  static_cast<void>(run(command::CouchJoin{2, 0}));
  static_cast<void>(run(command::CouchJoin{3, 0}));
  note();
  static_cast<void>(run(command::CouchJoin{1, 0}));
  note();
  answer(1, 0);
  ask(2);
  answer(3, 1);
  note();
  static_cast<void>(run(command::CouchPasscode{2, access_code("1111")}));
  note();
  ask(2);
  note();
  static_cast<void>(run(command::CouchPasscode{2, access_code("2222")}));
  answer(2, 0);
  note();
  ask(1);
  answer(1, 0);
  static_cast<void>(run(command::CouchAcceptPad{3}));
  note();
  static_cast<void>(run(command::CouchRemove{1}));
  static_cast<void>(run(command::CouchRemove{2}));
  note();
  static_cast<void>(tick_at(120'000'000));
  note();
  static_cast<void>(run(command::CouchJoin{2, 0}));
  note();
  ask(2);
  note();
  static_cast<void>(run(command::CouchPasscode{2, access_code("1111")}));
  note();
  ask(2);
  note();
  static_cast<void>(run(command::CouchPasscode{2, access_code("2222")}));
  answer(2, 0);
  note();
  static_cast<void>(run(command::CouchJoin{1, 0}));
  note();
  ask(1);
  static_cast<void>(tick_at(240'000'000));
  note();
  static_cast<void>(run(command::CouchRemove{1}));
  note();
  EXPECT_EQ(
      trace,
      "|0000|0100|0100|0300|0300|0300|0300|0303|0003|0003|0013|0023|0013|0023"
      "|0033|0033|0033|0033")
      << trace;
}

}  // namespace
}  // namespace play_test
