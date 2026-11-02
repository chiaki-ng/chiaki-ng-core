#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_session/invited_player_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>

namespace kakusan_test::invited {
namespace {

class InvitedPlayerWaiting : public InvitedPlayerTest {};
class InvitedPlayerTransport : public InvitedPlayerTest {};
class InvitedPlayerIdentity : public InvitedPlayerTest {};

TEST_F(InvitedPlayerWaiting, IgnoresEarlyEventsUntilTheInvitationExpires) {
  Rig rig;
  EXPECT_EQ(rig.spoke.state(), Progress::waiting);
  EXPECT_FALSE(rig.spoke.next_deadline_us());
  EXPECT_FALSE(rig.spoke.binding());
  EXPECT_TRUE(rig.feed(IceNominated{rig.now}).empty());
  EXPECT_TRUE(
      rig
          .feed(LocalCode{
              rig.now, libchiaking::net::kakusan::Code::from_value(1).value()})
          .empty());
  EXPECT_TRUE(rig.feed(DtlsEstablished{rig.now, vectors().exporter.clone(),
                                       vectors().own_proof})
                  .empty());
  const auto effects = rig.feed(InvitationExpired{rig.now});
  EXPECT_EQ(shown(effects).state, Progress::gone);
  EXPECT_EQ(shown(effects).error, Error::expired);
  EXPECT_EQ(count<SendControl>(effects), 0U);
  expect_cleanup(rig, 0, {});
  EXPECT_EQ(count<CloseTransport>(rig.log), 1U);
  expect_quiet(rig);
}

TEST_F(InvitedPlayerWaiting, EndsQuietlyWhenCancelled) {
  Rig rig;
  const auto effects = rig.feed(Cancel{rig.now, control::Reason::user});
  EXPECT_EQ(shown(effects).state, Progress::gone);
  EXPECT_EQ(shown(effects).reason, control::Reason::user);
  EXPECT_EQ(count<SendControl>(effects), 0U);
  expect_cleanup(rig, 0, {});
  expect_quiet(rig);
}

TEST_F(InvitedPlayerWaiting, IgnoresExpiryAndASecondReplyOnceConnecting) {
  Rig rig;
  drive(rig, Stage::connecting);
  EXPECT_TRUE(rig.feed(InvitationExpired{rig.now}).empty());
  EXPECT_EQ(rig.spoke.state(), Progress::connecting);
  EXPECT_TRUE(rig.feed(ReplyImported{rig.now, vectors().context}).empty());
}

TEST_F(InvitedPlayerTransport, ReportsNoPathWithoutConnectivity) {
  Rig rig;
  drive(rig, Stage::connecting);
  EXPECT_EQ(rig.spoke.next_deadline_us(), start_us + connectivity_timeout_us);
  EXPECT_TRUE(rig.at(start_us + connectivity_timeout_us - 1).empty());
  const auto effects = rig.at(start_us + connectivity_timeout_us);
  EXPECT_EQ(shown(effects).state, Progress::gone);
  EXPECT_EQ(shown(effects).error, Error::no_path);
  EXPECT_EQ(count<SendControl>(rig.log), 0U);
  expect_cleanup(rig, 0, {});
  expect_quiet(rig);
}

TEST_F(InvitedPlayerTransport, GivesTheChannelItsOwnDeadlineOnceNominated) {
  Rig rig;
  drive(rig, Stage::connecting);
  rig.now = start_us + 20'000'000;
  static_cast<void>(rig.feed(IceNominated{rig.now}));
  const auto nominated = rig.now;
  EXPECT_EQ(rig.spoke.next_deadline_us(), nominated + dtls_timeout_us);
  EXPECT_TRUE(rig.at(start_us + connectivity_timeout_us).empty());
  EXPECT_TRUE(rig.at(nominated + dtls_timeout_us - 1).empty());
  const auto effects = rig.at(nominated + dtls_timeout_us);
  EXPECT_EQ(shown(effects).error, Error::timeout);
  EXPECT_EQ(shown(effects).state, Progress::gone);
  EXPECT_EQ(count<SendControl>(rig.log), 0U);
  expect_cleanup(rig, 0, {});
}

TEST_F(InvitedPlayerTransport, ClosesWhenTheTimeoutNoticeIsAcknowledged) {
  Rig rig;
  drive(rig, Stage::channel);
  EXPECT_EQ(rig.spoke.next_deadline_us(), start_us + hello_timeout_us);
  EXPECT_TRUE(rig.at(start_us + hello_timeout_us - 1).empty());
  auto effects = rig.at(start_us + hello_timeout_us);
  EXPECT_EQ(failure_sent(effects), Error::timeout);
  EXPECT_EQ(rig.spoke.state(), Progress::leaving);
  expect_cleanup(rig, 0, {});
  EXPECT_TRUE(rig.ack(rig.hello_token).empty());
  EXPECT_TRUE(rig.ack(0).empty());
  const auto* notice = terminal(effects);
  ASSERT_NE(notice, nullptr);
  effects = rig.ack(notice->token);
  EXPECT_EQ(count<CloseTransport>(effects), 1U);
  EXPECT_EQ(rig.spoke.state(), Progress::gone);
  expect_quiet(rig);
}

TEST_F(InvitedPlayerTransport, ClosesAfterTheGraceWithoutTheHello) {
  Rig rig;
  drive(rig, Stage::channel);
  const auto effects = rig.at(start_us + hello_timeout_us);
  EXPECT_EQ(failure_sent(effects), Error::timeout);
  expect_closed_after_grace(rig);
}

TEST_F(InvitedPlayerTransport, ClosesAtOnceWhenTheTransportFailsWhileLeaving) {
  Rig rig;
  drive(rig, Stage::channel);
  static_cast<void>(rig.at(start_us + hello_timeout_us));
  const auto effects = rig.feed(TransportFailed{rig.now});
  EXPECT_EQ(count<CloseTransport>(effects), 1U);
  EXPECT_EQ(rig.spoke.state(), Progress::gone);
}

TEST_F(InvitedPlayerTransport, ReportsNoPathWhenConnectingFails) {
  Rig rig;
  drive(rig, Stage::connecting);
  const auto effects = rig.feed(TransportFailed{rig.now});
  EXPECT_EQ(shown(effects).error, Error::no_path);
  EXPECT_EQ(rig.spoke.state(), Progress::gone);
  expect_cleanup(rig, 0, {});
}

TEST_F(InvitedPlayerTransport, BlamesTheNetworkWhenAnOpenChannelFails) {
  Rig rig;
  drive(rig, Stage::channel);
  const auto effects = rig.feed(TransportFailed{rig.now});
  EXPECT_EQ(rig.spoke.state(), Progress::gone);
  EXPECT_EQ(shown(effects).reason, control::Reason::network);
  EXPECT_EQ(count<SendControl>(effects), 0U);
  expect_cleanup(rig, 0, {});
}

TEST_F(InvitedPlayerIdentity, RefusesAnIdentityBeforeTheHello) {
  Rig rig;
  drive(rig, Stage::channel);
  const auto effects = rig.receive(vectors().peer_proof);
  EXPECT_EQ(failure_sent(effects), Error::protocol);
  EXPECT_EQ(count<PeerIdentified>(rig.log), 0U);
}

TEST_F(InvitedPlayerIdentity, RefusesAForgedSignature) {
  Rig rig;
  drive(rig, Stage::channel);
  static_cast<void>(rig.receive(control::Hello{vectors().context.transcript}));
  auto forged = vectors().peer_proof;
  forged.signature[0] ^= std::byte{1};
  const auto effects = rig.receive(forged);
  EXPECT_EQ(failure_sent(effects), Error::identity_changed);
  EXPECT_EQ(shown(effects).error, Error::identity_changed);
  EXPECT_EQ(count<PeerIdentified>(rig.log), 0U);
}

TEST_F(InvitedPlayerIdentity, RefusesAStrangerOtherThanTheReplyingGuest) {
  Rig rig;
  drive(rig, Stage::channel);
  static_cast<void>(rig.receive(control::Hello{vectors().context.transcript}));
  libchiaking::net::kakusan::IdentitySeed seed{};
  seed.fill(std::byte{0x77});
  const auto stranger = libchiaking::net::kakusan::IdentityProof::sign(
      crypto(), libchiaking::net::kakusan::Role::guest,
      vectors().context.transcript, vectors().exporter, seed, std::nullopt);
  ASSERT_TRUE(stranger);
  const auto effects = rig.receive(stranger.value());
  EXPECT_EQ(failure_sent(effects), Error::identity_changed);
  EXPECT_EQ(count<PeerIdentified>(rig.log), 0U);
}

TEST_F(InvitedPlayerIdentity, RefusesItsOwnIdentityEchoedBack) {
  Rig rig;
  drive(rig, Stage::channel);
  static_cast<void>(rig.receive(control::Hello{vectors().context.transcript}));
  const auto effects = rig.receive(vectors().own_proof);
  EXPECT_EQ(failure_sent(effects), Error::protocol);
  EXPECT_EQ(count<PeerIdentified>(rig.log), 0U);
}

TEST_F(InvitedPlayerIdentity, RefusesPairingBeforeTheGuestIsIdentified) {
  Rig rig;
  drive(rig, Stage::channel);
  static_cast<void>(rig.receive(control::Hello{vectors().context.transcript}));
  const auto effects = rig.receive(control::PakeB{vectors().pb});
  EXPECT_TRUE(failure_sent(effects).has_value());
  EXPECT_NE(rig.spoke.state(), Progress::verifying);
  EXPECT_EQ(count<PeerIdentified>(rig.log), 0U);
}

TEST_F(InvitedPlayerIdentity, RefusesASecondIdentity) {
  Rig rig;
  drive(rig, Stage::verifying);
  const auto effects = rig.receive(vectors().peer_proof);
  EXPECT_EQ(failure_sent(effects), Error::protocol);
}

}  // namespace
}  // namespace kakusan_test::invited
