#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_session/invited_player_rig.hpp"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace kakusan_test::invited {
namespace {

class InvitedPlayerTermination : public InvitedPlayerTest {};
class InvitedPlayerPeerEnding : public InvitedPlayerTest {};
class InvitedPlayerCancel : public InvitedPlayerTest {};

TEST_F(InvitedPlayerTermination, EndsOnceAndIgnoresEverythingAfter) {
  Rig rig;
  drive(rig, Stage::media);
  const auto from = rig.log.size();
  auto effects = rig.feed(Cancel{rig.now, control::Reason::user});
  EXPECT_EQ(closing<control::End>(effects).reason, control::Reason::user);
  EXPECT_TRUE(rig.feed(Cancel{rig.now, control::Reason::user}).empty());
  EXPECT_TRUE(rig.feed(CapacityRevoked{rig.now}).empty());
  EXPECT_TRUE(rig.feed(SourceEnded{rig.now}).empty());
  EXPECT_TRUE(rig.feed(InputReceived{rig.now, input(0, 1)}).empty());
  EXPECT_TRUE(rig.feed(RumbleSampled{rig.now, 5, 5}).empty());
  EXPECT_TRUE(rig.feed(MalformedData{rig.now, Error::protocol}).empty());
  EXPECT_TRUE(rig.receive(control::Leave{control::Reason::user}).empty());
  EXPECT_TRUE(rig.receive(control::Ready{seat}).empty());
  EXPECT_TRUE(rig.receive(control::Failure{Error::protocol}).empty());
  expect_cleanup(
      rig, from,
      {.media = true, .neutral = true, .removal = true, .release = true});
  const auto* notice = terminal(rig.log);
  ASSERT_NE(notice, nullptr);
  effects = rig.ack(notice->token);
  EXPECT_EQ(count<CloseTransport>(effects), 1U);
  expect_quiet(rig);
}

TEST_F(InvitedPlayerTermination, RevokesTheSeatOnceWhenCapacityIsLost) {
  Rig rig;
  drive(rig, Stage::media);
  const auto from = rig.log.size();
  const auto effects = rig.feed(CapacityRevoked{rig.now});
  const auto& revoked = closing<control::SeatRevoked>(effects);
  EXPECT_EQ(revoked.reason, control::Reason::capacity);
  EXPECT_EQ(revoked.binding, seat);
  EXPECT_TRUE(rig.feed(CapacityRevoked{rig.now}).empty());
  EXPECT_TRUE(rig.feed(Cancel{rig.now, control::Reason::user}).empty());
  expect_cleanup(
      rig, from,
      {.media = true, .neutral = true, .removal = true, .release = true});
  EXPECT_EQ(rig.feed(TransportFailed{rig.now}).size(), 2U);
  EXPECT_EQ(rig.spoke.state(), Progress::gone);
  expect_quiet(rig);
}

TEST_F(InvitedPlayerTermination, ReportsNoCapacityBeforeTheSourceJoins) {
  Rig rig;
  drive(rig, Stage::reserved);
  const auto from = rig.log.size();
  const auto effects = rig.feed(CapacityRevoked{rig.now});
  EXPECT_EQ(failure_sent(effects), Error::no_capacity);
  expect_cleanup(rig, from, {.removal = true, .release = true});
}

TEST_F(InvitedPlayerTermination, ReleasesASeatReservedAfterLeaving) {
  Rig rig;
  drive(rig, Stage::admitting);
  EXPECT_TRUE(rig.feed(CapacityRevoked{rig.now}).empty());
  static_cast<void>(rig.feed(Cancel{rig.now, control::Reason::user}));
  const auto from = rig.log.size();
  const auto effects = rig.feed(SeatReserved{rig.now, seat});
  EXPECT_EQ(effects.size(), 1U);
  EXPECT_EQ(the<ReleaseSeat>(effects).binding, seat);
  EXPECT_EQ(rig.log.size(), from + 1);
  EXPECT_TRUE(rig.feed(SourceJoined{rig.now, vectors().setup.clone(), 4, 2,
                                    libchiaking::net::kakusan::button_mask})
                  .empty());
}

TEST_F(InvitedPlayerPeerEnding, LetsTheGuestLeave) {
  Rig rig;
  drive(rig, Stage::media);
  const auto from = rig.log.size();
  const auto effects = rig.receive(control::Leave{control::Reason::user});
  EXPECT_EQ(count<SendControl>(effects), 0U);
  EXPECT_TRUE(shown(effects).by_peer);
  EXPECT_EQ(shown(effects).reason, control::Reason::user);
  EXPECT_EQ(shown(effects).state, Progress::leaving);
  expect_cleanup(
      rig, from,
      {.media = true, .neutral = true, .removal = true, .release = true});
  expect_closed_after_grace(rig);
}

TEST_F(InvitedPlayerPeerEnding, ShowsTheGuestsFailure) {
  Rig rig;
  drive(rig, Stage::ready_wait);
  const auto from = rig.log.size();
  const auto effects = rig.receive(control::Failure{Error::codec});
  EXPECT_EQ(count<SendControl>(effects), 0U);
  EXPECT_EQ(shown(effects).error, Error::codec);
  expect_cleanup(rig, from,
                 {.neutral = true, .removal = true, .release = true});
  EXPECT_TRUE(rig.receive(control::Ready{seat}).empty());
  expect_closed_after_grace(rig);
}

TEST_F(InvitedPlayerPeerEnding, ClosesAtOnceWhenTheTransportFails) {
  Rig rig;
  drive(rig, Stage::media);
  const auto from = rig.log.size();
  const auto effects = rig.feed(TransportFailed{rig.now});
  EXPECT_EQ(count<SendControl>(effects), 0U);
  EXPECT_EQ(rig.spoke.state(), Progress::gone);
  expect_cleanup(
      rig, from,
      {.media = true, .neutral = true, .removal = true, .release = true});
}

TEST_F(InvitedPlayerPeerEnding, ReportsMalformedDataAsAProtocolFailure) {
  Rig rig;
  drive(rig, Stage::media);
  const auto from = rig.log.size();
  const auto effects = rig.feed(MalformedData{rig.now, Error::codec});
  EXPECT_EQ(failure_sent(effects), Error::protocol);
  EXPECT_TRUE(rig.feed(MalformedData{rig.now, Error::protocol}).empty());
  expect_cleanup(
      rig, from,
      {.media = true, .neutral = true, .removal = true, .release = true});
}

TEST_F(InvitedPlayerPeerEnding, EndsWithTheSourceWhilePlaying) {
  Rig rig;
  drive(rig, Stage::media);
  const auto effects = rig.feed(SourceEnded{rig.now});
  EXPECT_EQ(closing<control::End>(effects).reason, control::Reason::source);
}

TEST_F(InvitedPlayerPeerEnding, FailsWithTheSourceWhilePairing) {
  Rig rig;
  drive(rig, Stage::verifying);
  const auto effects = rig.feed(SourceEnded{rig.now});
  EXPECT_EQ(failure_sent(effects), Error::source_ended);
}

TEST_F(InvitedPlayerPeerEnding, ShowsTheEndedSourceWhileConnecting) {
  Rig rig;
  drive(rig, Stage::connecting);
  const auto effects = rig.feed(SourceEnded{rig.now});
  EXPECT_EQ(count<SendControl>(effects), 0U);
  EXPECT_EQ(shown(effects).error, Error::source_ended);
}

TEST_F(InvitedPlayerCancel, ClosesSilentlyBeforeAdmission) {
  for (const auto stage :
       {Stage::waiting, Stage::connecting, Stage::nominated, Stage::channel,
        Stage::verifying, Stage::pake_a, Stage::authenticated_sent}) {
    SCOPED_TRACE(static_cast<int>(stage));
    Rig rig;
    drive(rig, stage);
    const auto from = rig.log.size();
    const auto effects = rig.feed(Cancel{rig.now, control::Reason::user});
    EXPECT_EQ(count<SendControl>(effects), 0U);
    EXPECT_EQ(rig.spoke.state(), Progress::gone);
    EXPECT_EQ(count<CloseTransport>(effects), 1U);
    expect_cleanup(rig, from, {});
    expect_quiet(rig);
  }
}

TEST_F(InvitedPlayerCancel, EndsAndReleasesWhatWasTakenAfterAdmission) {
  const std::vector<std::pair<Stage, Cleanup>> stages{
      {Stage::admitting, {}},
      {Stage::reserved, {.removal = true, .release = true}},
      {Stage::setup_acked, {.neutral = true, .removal = true, .release = true}},
      {Stage::ready_wait, {.neutral = true, .removal = true, .release = true}},
      {Stage::active, {.neutral = true, .removal = true, .release = true}},
      {Stage::media,
       {.media = true, .neutral = true, .removal = true, .release = true}},
  };
  for (const auto& [stage, cleanup] : stages) {
    SCOPED_TRACE(static_cast<int>(stage));
    Rig rig;
    drive(rig, stage);
    const auto from = rig.log.size();
    const auto effects = rig.feed(Cancel{rig.now, control::Reason::user});
    EXPECT_NE(terminal(effects), nullptr);
    EXPECT_EQ(closing<control::End>(effects).reason, control::Reason::user);
    EXPECT_EQ(shown(effects).state, Progress::leaving);
    expect_cleanup(rig, from, cleanup);
    expect_closed_after_grace(rig);
    expect_quiet(rig);
  }
}

}  // namespace
}  // namespace kakusan_test::invited
