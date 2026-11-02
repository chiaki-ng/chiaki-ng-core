#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/guest_visit/visiting_player_rig.hpp"

#include <gtest/gtest.h>

namespace kakusan_test::visiting {
namespace {

class VisitingPlayerEnding : public VisitingPlayerTest {};
class VisitingPlayerCancel : public VisitingPlayerTest {};

TEST_F(VisitingPlayerEnding, StopsEverythingWhenTheSeatIsRevoked) {
  Rig rig;
  drive(rig, Stage::active);
  rig.now += 1'000;
  static_cast<void>(rig.feed(FeedbackReceived{rig.now, feedback(0, 900)}));
  const auto from = rig.log.size();
  const auto effects =
      rig.receive(control::SeatRevoked{seat, control::Reason::capacity});
  EXPECT_EQ(count<SendControl>(effects), 0U);
  EXPECT_TRUE(shown(effects).by_peer);
  EXPECT_EQ(shown(effects).reason, control::Reason::capacity);
  expect_cleanup(rig, from, {.receiver = true, .rumble = true});
  EXPECT_TRUE(rig.feed(FeedbackReceived{rig.now, feedback(1, 5)}).empty());
  EXPECT_TRUE(rig.feed(ControllerSampled{rig.now, pressed(south)}).empty());
  expect_closed_after_grace(rig);
  expect_quiet(rig);
}

TEST_F(VisitingPlayerEnding, RefusesARevocationOfAnotherSeat) {
  for (const auto wrong : {SeatBinding{2, 7}, SeatBinding{3, 6}}) {
    SCOPED_TRACE(::testing::Message() << "seat " << +wrong.seat
                                      << " generation " << wrong.generation);
    Rig rig;
    drive(rig, Stage::active);
    const auto effects =
        rig.receive(control::SeatRevoked{wrong, control::Reason::user});
    EXPECT_EQ(failure_sent(effects), Error::protocol);
  }
}

TEST_F(VisitingPlayerEnding, LeavesWhenTheHostEndsTheSession) {
  for (const auto stage :
       {Stage::admitting, Stage::setup, Stage::granted, Stage::active}) {
    SCOPED_TRACE(static_cast<int>(stage));
    Rig rig;
    drive(rig, stage);
    const auto from = rig.log.size();
    const auto effects = rig.receive(control::End{control::Reason::source});
    EXPECT_EQ(count<SendControl>(effects), 0U);
    EXPECT_EQ(shown(effects).reason, control::Reason::source);
    EXPECT_TRUE(shown(effects).by_peer);
    expect_cleanup(
        rig, from,
        {.receiver = stage >= Stage::setup, .rumble = stage == Stage::active});
    expect_closed_after_grace(rig);
  }
}

TEST_F(VisitingPlayerEnding, ShowsTheHostsFailure) {
  Rig rig;
  drive(rig, Stage::active);
  const auto from = rig.log.size();
  const auto effects = rig.receive(control::Failure{Error::congestion});
  EXPECT_EQ(count<SendControl>(effects), 0U);
  EXPECT_EQ(shown(effects).error, Error::congestion);
  expect_cleanup(rig, from, {.receiver = true, .rumble = true});
}

TEST_F(VisitingPlayerEnding, ClosesOnceTheFailureNoticeIsAcknowledged) {
  Rig rig;
  drive(rig, Stage::active);
  const auto from = rig.log.size();
  const auto effects = rig.feed(MalformedData{rig.now, Error::protocol});
  EXPECT_EQ(failure_sent(effects), Error::protocol);
  EXPECT_TRUE(rig.feed(MalformedData{rig.now, Error::protocol}).empty());
  expect_cleanup(rig, from, {.receiver = true, .rumble = true});
  const auto* notice = terminal(effects);
  ASSERT_NE(notice, nullptr);
  const auto close = rig.feed(ControlAcked{rig.now, notice->token});
  EXPECT_EQ(count<CloseTransport>(close), 1U);
  expect_quiet(rig);
}

TEST_F(VisitingPlayerEnding, ClosesAtOnceWhenTheTransportFails) {
  Rig rig;
  drive(rig, Stage::active);
  const auto from = rig.log.size();
  const auto effects = rig.feed(TransportFailed{rig.now});
  EXPECT_EQ(count<SendControl>(effects), 0U);
  EXPECT_EQ(count<CloseTransport>(effects), 1U);
  expect_cleanup(rig, from, {.receiver = true, .rumble = true});
}

TEST_F(VisitingPlayerCancel, ClosesSilentlyBeforeAdmission) {
  for (const auto stage :
       {Stage::waiting, Stage::connecting, Stage::channel, Stage::verifying,
        Stage::entered, Stage::pake_b, Stage::confirm_b}) {
    SCOPED_TRACE(static_cast<int>(stage));
    Rig rig;
    drive(rig, stage);
    const auto from = rig.log.size();
    const auto effects = rig.feed(Cancel{rig.now, control::Reason::user});
    EXPECT_EQ(count<SendControl>(effects), 0U);
    EXPECT_EQ(count<CloseTransport>(effects), 1U);
    expect_cleanup(rig, from, {});
    expect_quiet(rig);
  }
}

TEST_F(VisitingPlayerCancel, SaysGoodbyeOnceAfterAdmission) {
  for (const auto stage :
       {Stage::admitting, Stage::pending, Stage::setup, Stage::source_ready,
        Stage::granted, Stage::ready, Stage::active}) {
    SCOPED_TRACE(static_cast<int>(stage));
    Rig rig;
    drive(rig, stage);
    const auto from = rig.log.size();
    const auto effects = rig.feed(Cancel{rig.now, control::Reason::user});
    EXPECT_NE(terminal(effects), nullptr);
    EXPECT_EQ(closing<control::Leave>(effects).reason, control::Reason::user);
    EXPECT_TRUE(rig.feed(Cancel{rig.now, control::Reason::user}).empty());
    expect_cleanup(
        rig, from,
        {.receiver = stage >= Stage::setup, .rumble = stage == Stage::active});
    expect_closed_after_grace(rig);
    expect_quiet(rig);
  }
}

}  // namespace
}  // namespace kakusan_test::visiting
