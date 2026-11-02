#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/guest_visit/visiting_player_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>

namespace kakusan_test::visiting {
namespace {

class VisitingPlayerWaiting : public VisitingPlayerTest {};
class VisitingPlayerTransport : public VisitingPlayerTest {};
class VisitingPlayerIdentity : public VisitingPlayerTest {};

TEST_F(VisitingPlayerWaiting, IgnoresEarlyEventsUntilTheInvitationExpires) {
  Rig rig;
  EXPECT_EQ(rig.spoke.state(), Progress::waiting);
  EXPECT_FALSE(rig.spoke.next_deadline_us());
  EXPECT_FALSE(rig.spoke.binding());
  EXPECT_TRUE(
      rig
          .feed(LocalCode{
              rig.now, libchiaking::net::kakusan::Code::from_value(1).value()})
          .empty());
  EXPECT_TRUE(rig.feed(ReceiverReady{rig.now}).empty());
  const auto effects = rig.feed(InvitationExpired{rig.now});
  EXPECT_EQ(shown(effects).state, Progress::gone);
  EXPECT_EQ(shown(effects).error, Error::expired);
  EXPECT_EQ(count<SendControl>(effects), 0U);
  expect_cleanup(rig, 0, {});
  expect_quiet(rig);
}

TEST_F(VisitingPlayerWaiting, EndsQuietlyWhenCancelled) {
  Rig rig;
  const auto effects = rig.feed(Cancel{rig.now, control::Reason::user});
  EXPECT_EQ(shown(effects).state, Progress::gone);
  EXPECT_EQ(count<SendControl>(effects), 0U);
  expect_cleanup(rig, 0, {});
}

TEST_F(VisitingPlayerWaiting, IgnoresExpiryAndRenominationOnceConnecting) {
  Rig rig;
  drive(rig, Stage::connecting);
  EXPECT_TRUE(rig.feed(InvitationExpired{rig.now}).empty());
  EXPECT_TRUE(rig.feed(IceNominated{rig.now}).empty());
  EXPECT_EQ(rig.spoke.state(), Progress::connecting);
}

TEST_F(VisitingPlayerWaiting, SaysHelloWhenTheChannelOpensBeforeNomination) {
  Rig rig;
  const auto effects = rig.feed(DtlsEstablished{
      rig.now, vectors().exporter.clone(), vectors().own_proof});
  EXPECT_EQ(rig.spoke.state(), Progress::connecting);
  EXPECT_NE(sent<control::Hello>(effects), nullptr);
}

TEST_F(VisitingPlayerWaiting, ReportsNoPathWhenTheTransportFails) {
  Rig rig;
  const auto effects = rig.feed(TransportFailed{rig.now});
  EXPECT_EQ(shown(effects).error, Error::no_path);
  EXPECT_EQ(rig.spoke.state(), Progress::gone);
}

TEST_F(VisitingPlayerTransport, TimesOutWithoutAChannel) {
  Rig rig;
  drive(rig, Stage::connecting);
  EXPECT_EQ(rig.spoke.next_deadline_us(), start_us + dtls_timeout_us);
  EXPECT_TRUE(rig.at(start_us + dtls_timeout_us - 1).empty());
  const auto effects = rig.at(start_us + dtls_timeout_us);
  EXPECT_EQ(shown(effects).error, Error::timeout);
  EXPECT_EQ(rig.spoke.state(), Progress::gone);
  EXPECT_EQ(count<SendControl>(rig.log), 0U);
  expect_cleanup(rig, 0, {});
}

TEST_F(VisitingPlayerTransport, FailsWithATimeoutWithoutTheHostsHello) {
  Rig rig;
  drive(rig, Stage::channel);
  EXPECT_EQ(rig.spoke.next_deadline_us(), start_us + hello_timeout_us);
  EXPECT_TRUE(rig.at(start_us + hello_timeout_us - 1).empty());
  const auto effects = rig.at(start_us + hello_timeout_us);
  EXPECT_EQ(failure_sent(effects), Error::timeout);
  expect_cleanup(rig, 0, {});
  expect_closed_after_grace(rig);
}

TEST_F(VisitingPlayerTransport, ReportsNoPathWhenConnectingFails) {
  Rig rig;
  drive(rig, Stage::connecting);
  const auto effects = rig.feed(TransportFailed{rig.now});
  EXPECT_EQ(shown(effects).error, Error::no_path);
  EXPECT_EQ(count<SendControl>(effects), 0U);
}

TEST_F(VisitingPlayerTransport, BlamesTheNetworkWhenAnOpenChannelFails) {
  Rig rig;
  drive(rig, Stage::verifying);
  const auto effects = rig.feed(TransportFailed{rig.now});
  EXPECT_EQ(shown(effects).reason, control::Reason::network);
  EXPECT_EQ(rig.spoke.state(), Progress::gone);
  EXPECT_EQ(count<SendControl>(effects), 0U);
  expect_cleanup(rig, 0, {});
}

TEST_F(VisitingPlayerIdentity, RefusesAnIdentityBeforeTheHello) {
  Rig rig;
  drive(rig, Stage::channel);
  const auto effects = rig.receive(vectors().peer_proof);
  EXPECT_EQ(failure_sent(effects), Error::protocol);
  EXPECT_EQ(count<PeerIdentified>(rig.log), 0U);
}

TEST_F(VisitingPlayerIdentity, RefusesAForgedSignature) {
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

TEST_F(VisitingPlayerIdentity, RefusesAStrangerOtherThanTheInvitingHost) {
  Rig rig;
  drive(rig, Stage::channel);
  static_cast<void>(rig.receive(control::Hello{vectors().context.transcript}));
  libchiaking::net::kakusan::IdentitySeed seed{};
  seed.fill(std::byte{0x77});
  const auto stranger = libchiaking::net::kakusan::IdentityProof::sign(
      crypto(), libchiaking::net::kakusan::Role::host,
      vectors().context.transcript, vectors().exporter, seed,
      vectors().introductions.hosting());
  ASSERT_TRUE(stranger);
  const auto effects = rig.receive(stranger.value());
  EXPECT_EQ(failure_sent(effects), Error::identity_changed);
  EXPECT_EQ(count<PeerIdentified>(rig.log), 0U);
}

TEST_F(VisitingPlayerIdentity, RefusesItsOwnIdentityEchoedBack) {
  Rig rig;
  drive(rig, Stage::channel);
  static_cast<void>(rig.receive(control::Hello{vectors().context.transcript}));
  const auto effects = rig.receive(vectors().own_proof);
  EXPECT_EQ(failure_sent(effects), Error::protocol);
  EXPECT_EQ(count<PeerIdentified>(rig.log), 0U);
}

TEST_F(VisitingPlayerIdentity, RefusesPairingBeforeTheHostIsIdentified) {
  Rig rig;
  drive(rig, Stage::channel);
  static_cast<void>(rig.receive(control::Hello{vectors().context.transcript}));
  const auto effects = rig.receive(control::PakeA{vectors().pa});
  EXPECT_TRUE(failure_sent(effects).has_value());
  EXPECT_NE(rig.spoke.state(), Progress::verifying);
  EXPECT_EQ(count<PeerIdentified>(rig.log), 0U);
}

TEST_F(VisitingPlayerIdentity, RefusesASecondIdentity) {
  Rig rig;
  drive(rig, Stage::verifying);
  const auto effects = rig.receive(vectors().peer_proof);
  EXPECT_EQ(failure_sent(effects), Error::protocol);
}

}  // namespace
}  // namespace kakusan_test::visiting
