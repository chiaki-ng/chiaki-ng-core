#include "src/core/net/protocol/takion/handshake.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class ConnectionHandshakeTest : public ::testing::Test {
 protected:
  template <class Action>
  static auto expect_action(const takion::HandshakeAction& action) -> Action {
    EXPECT_TRUE(std::holds_alternative<Action>(action))
        << "the handshake chose action " << action.index();
    return std::get<Action>(action);
  }

  static auto cookie(std::uint8_t seed) -> takion::HandshakeCookie {
    takion::HandshakeCookie value{};
    for (std::size_t index = 0; index < value.size(); ++index)
      value[index] =
          static_cast<std::byte>(seed + static_cast<std::uint8_t>(index));
    return value;
  }

  static auto valid_ack() -> takion::InitAcknowledgementValues {
    return {
        {
            takion::VerificationTag{0x44556677},
            takion::AdvertisedReceiveWindow{0x22000},
            takion::StreamCount{80},
            takion::StreamCount{120},
            takion::InitialSequenceNumber{0xabcdef01},
        },
        cookie(3),
    };
  }

  static auto ack_event(
      takion::HandshakeIdentity identity,
      const takion::InitAcknowledgementValues& acknowledgement = valid_ack(),
      std::uint64_t now = 101) -> takion::InitAcknowledgementReceived {
    return {identity, takion::VerificationTag{0x11223344}, acknowledgement,
            now};
  }
};

TEST_F(ConnectionHandshakeTest,
       ConnectsThroughInitAndCookieUsingTheRemoteTagAsItsSequence) {
  takion::ConnectionHandshake handshake;
  const auto& init = expect_action<takion::SendInit>(
      handshake.handle(takion::Start{{0x11223344}, {0x10203040}, 100}));
  EXPECT_EQ(init.identity.generation, 1);
  EXPECT_EQ(init.identity.attempt, 1);
  EXPECT_EQ(init.values.tag.value, 0x11223344);
  EXPECT_EQ(init.values.initial_sequence_number.value, 0x10203040);
  EXPECT_EQ(init.values.advertised_receive_window.value,
            takion::default_advertised_receive_window);
  EXPECT_EQ(init.deadline.monotonic_microseconds,
            100 + takion::default_handshake_response_timeout_us);
  EXPECT_EQ(handshake.snapshot().state,
            takion::HandshakeState::await_init_acknowledgement);

  const auto& send_cookie = expect_action<takion::SendCookie>(
      handshake.handle(ack_event(init.identity)));
  EXPECT_EQ(send_cookie.identity.generation, 1);
  EXPECT_EQ(send_cookie.identity.attempt, 1);
  EXPECT_EQ(send_cookie.remote_tag.value, valid_ack().init.tag.value);
  EXPECT_EQ(send_cookie.cookie, valid_ack().cookie);
  EXPECT_EQ(send_cookie.deadline.monotonic_microseconds,
            101 + takion::default_handshake_response_timeout_us);
  EXPECT_EQ(handshake.snapshot().state,
            takion::HandshakeState::await_cookie_acknowledgement);

  const auto& connected = expect_action<takion::Connected>(
      handshake.handle(takion::CookieAcknowledgementReceived{
          send_cookie.identity, takion::VerificationTag{0x11223344}, 102}));
  EXPECT_EQ(connected.handshake.local_tag.value, 0x11223344);
  EXPECT_EQ(connected.handshake.remote_tag.value, valid_ack().init.tag.value);
  EXPECT_EQ(connected.handshake.local_initial_sequence_number.value,
            0x10203040);
  EXPECT_EQ(connected.handshake.remote_initial_sequence_number.value,
            valid_ack().init.tag.value);
  EXPECT_EQ(handshake.snapshot().state, takion::HandshakeState::connected);
  EXPECT_TRUE(handshake.snapshot().established.has_value());
}

TEST_F(ConnectionHandshakeTest,
       CanUseTheAdvertisedInitialSequenceNumberWhenAskedTo) {
  takion::ConnectionHandshake handshake(
      {}, {}, takion::StreamCount{100}, takion::StreamCount{100},
      takion::RemoteSequencePolicy::advertised_initial_sequence_number);
  const auto& init = expect_action<takion::SendInit>(
      handshake.handle(takion::Start{{7}, {8}, 0}));
  const auto& send_cookie = expect_action<takion::SendCookie>(
      handshake.handle(takion::InitAcknowledgementReceived{
          init.identity, takion::VerificationTag{7}, valid_ack(), 1}));
  const auto& connected = expect_action<takion::Connected>(handshake.handle(
      takion::CookieAcknowledgementReceived{send_cookie.identity, {7}, 2}));
  EXPECT_EQ(connected.handshake.remote_initial_sequence_number.value,
            valid_ack().init.initial_sequence_number.value);
}

TEST_F(ConnectionHandshakeTest,
       RetriesInitOnTimeoutIgnoresStaleInputAndGivesUp) {
  takion::ConnectionHandshake handshake({3, 5});
  const auto& first = expect_action<takion::SendInit>(
      handshake.handle(takion::Start{{9}, {10}, 100}));

  expect_action<takion::Rejected>(
      handshake.handle(takion::Timeout{first.identity, 104}));
  EXPECT_EQ(handshake.snapshot().attempt, 1);
  EXPECT_EQ(handshake.snapshot().deadline.monotonic_microseconds, 105);

  const auto& second = expect_action<takion::SendInit>(
      handshake.handle(takion::Timeout{first.identity, 105}));
  EXPECT_EQ(second.identity.attempt, 2);
  EXPECT_EQ(second.deadline.monotonic_microseconds, 110);

  expect_action<takion::Rejected>(
      handshake.handle(ack_event(first.identity, valid_ack(), 106)));
  EXPECT_EQ(handshake.snapshot().attempt, 2);
  EXPECT_EQ(handshake.snapshot().state,
            takion::HandshakeState::await_init_acknowledgement);

  const auto& third = expect_action<takion::SendInit>(
      handshake.handle(takion::Timeout{second.identity, 110}));
  EXPECT_EQ(third.identity.attempt, 3);
  const auto& failed = expect_action<takion::Failed>(
      handshake.handle(takion::Timeout{third.identity, 115}));
  EXPECT_EQ(failed.error, takion::HandshakeError::attempts_exhausted);
  EXPECT_EQ(handshake.snapshot().state, takion::HandshakeState::failed);
}

TEST_F(ConnectionHandshakeTest, RetriesTheCookieWithTheValuesItFirstReceived) {
  takion::ConnectionHandshake handshake({3, 5});
  const auto& init = expect_action<takion::SendInit>(
      handshake.handle(takion::Start{{4}, {5}, 10}));
  auto acknowledgement = valid_ack();
  const auto& first = expect_action<takion::SendCookie>(
      handshake.handle(takion::InitAcknowledgementReceived{
          init.identity, {4}, acknowledgement, 11}));
  const auto& second = expect_action<takion::SendCookie>(
      handshake.handle(takion::Timeout{first.identity, 16}));
  EXPECT_EQ(second.identity.attempt, 2);
  EXPECT_EQ(second.remote_tag.value, acknowledgement.init.tag.value);
  EXPECT_EQ(second.cookie, acknowledgement.cookie);
  EXPECT_EQ(second.deadline.monotonic_microseconds, 21);

  const auto& connected = expect_action<takion::Connected>(handshake.handle(
      takion::CookieAcknowledgementReceived{second.identity, {4}, 17}));
  EXPECT_EQ(connected.identity.attempt, 2);
}

TEST_F(ConnectionHandshakeTest,
       CookieAttemptsRunOutAndTheCookiePhaseCanBeCancelled) {
  takion::ConnectionHandshake exhausted_handshake({2, 5});
  const auto& init = expect_action<takion::SendInit>(
      exhausted_handshake.handle(takion::Start{{14}, {15}, 0}));
  const auto& first_cookie = expect_action<takion::SendCookie>(
      exhausted_handshake.handle(takion::InitAcknowledgementReceived{
          init.identity, {14}, valid_ack(), 1}));
  const auto& second_cookie = expect_action<takion::SendCookie>(
      exhausted_handshake.handle(takion::Timeout{first_cookie.identity, 6}));
  const auto& failure = expect_action<takion::Failed>(
      exhausted_handshake.handle(takion::Timeout{second_cookie.identity, 11}));
  EXPECT_EQ(failure.error, takion::HandshakeError::attempts_exhausted);
  EXPECT_EQ(exhausted_handshake.snapshot().state,
            takion::HandshakeState::failed);
  EXPECT_FALSE(exhausted_handshake.snapshot().established.has_value());

  takion::ConnectionHandshake cancelled_handshake;
  const auto& other_init = expect_action<takion::SendInit>(
      cancelled_handshake.handle(takion::Start{{24}, {25}, 0}));
  expect_action<takion::SendCookie>(
      cancelled_handshake.handle(takion::InitAcknowledgementReceived{
          other_init.identity, {24}, valid_ack(), 1}));
  const auto& cancelled = expect_action<takion::Failed>(
      cancelled_handshake.handle(takion::Cancel{1}));
  EXPECT_EQ(cancelled.error, takion::HandshakeError::cancelled);
  EXPECT_EQ(cancelled_handshake.snapshot().state,
            takion::HandshakeState::failed);
}

TEST_F(ConnectionHandshakeTest,
       RejectsAnAcknowledgementWithTheWrongTagWithoutChangingState) {
  takion::ConnectionHandshake handshake;
  const auto& init = expect_action<takion::SendInit>(
      handshake.handle(takion::Start{{20}, {21}, 30}));
  const auto before = handshake.snapshot();

  const auto& tag_rejection = expect_action<takion::Rejected>(
      handshake.handle(takion::InitAcknowledgementReceived{
          init.identity, {999}, valid_ack(), 34}));
  EXPECT_EQ(tag_rejection.error, takion::HandshakeError::tag_mismatch);
  EXPECT_EQ(handshake.snapshot().state, before.state);

  expect_action<takion::SendCookie>(
      handshake.handle(takion::InitAcknowledgementReceived{
          init.identity, {20}, valid_ack(), 35}));
}

TEST_F(ConnectionHandshakeTest,
       FailsOnAZeroTagOrBadStreamCountsInTheInitAcknowledgement) {
  auto zero_tag = valid_ack();
  zero_tag.init.tag.value = 0;
  auto too_many_remote_outbound = valid_ack();
  too_many_remote_outbound.init.outbound_streams.value = 101;
  auto too_few_remote_inbound = valid_ack();
  too_few_remote_inbound.init.inbound_streams.value = 99;
  auto zero_outbound = valid_ack();
  zero_outbound.init.outbound_streams.value = 0;
  const std::pair<takion::InitAcknowledgementValues, takion::HandshakeError>
      cases[]{
          {zero_tag, takion::HandshakeError::zero_remote_tag},
          {too_many_remote_outbound,
           takion::HandshakeError::invalid_stream_counts},
          {too_few_remote_inbound,
           takion::HandshakeError::invalid_stream_counts},
          {zero_outbound, takion::HandshakeError::invalid_stream_counts},
      };
  for (const auto& [acknowledgement, error] : cases) {
    takion::ConnectionHandshake handshake;
    const auto& init = expect_action<takion::SendInit>(
        handshake.handle(takion::Start{{20}, {21}, 30}));
    const auto& failure = expect_action<takion::Failed>(
        handshake.handle(takion::InitAcknowledgementReceived{
            init.identity, {20}, acknowledgement, 31}));
    EXPECT_EQ(failure.error, error);
    EXPECT_EQ(handshake.snapshot().state, takion::HandshakeState::failed);
    EXPECT_FALSE(handshake.snapshot().established.has_value());
  }
}

TEST_F(ConnectionHandshakeTest,
       IgnoresADuplicateInitAcknowledgementButRejectsAChangedOne) {
  takion::ConnectionHandshake handshake;
  const auto& init = expect_action<takion::SendInit>(
      handshake.handle(takion::Start{{30}, {31}, 40}));
  const auto acknowledgement = valid_ack();
  const auto& send_cookie = expect_action<takion::SendCookie>(
      handshake.handle(takion::InitAcknowledgementReceived{
          init.identity, {30}, acknowledgement, 41}));
  const auto before = handshake.snapshot();

  expect_action<takion::NoAction>(
      handshake.handle(takion::InitAcknowledgementReceived{
          send_cookie.identity, {30}, acknowledgement, 42}));
  EXPECT_EQ(handshake.snapshot().state, before.state);
  EXPECT_EQ(handshake.snapshot().attempt, before.attempt);
  EXPECT_EQ(handshake.snapshot().deadline.monotonic_microseconds,
            before.deadline.monotonic_microseconds);

  auto changed = acknowledgement;
  changed.cookie[0] = std::byte{0xff};
  expect_action<takion::Rejected>(
      handshake.handle(takion::InitAcknowledgementReceived{
          send_cookie.identity, {30}, changed, 43}));
  EXPECT_EQ(handshake.snapshot().state, before.state);

  expect_action<takion::Connected>(handshake.handle(
      takion::CookieAcknowledgementReceived{send_cookie.identity, {30}, 44}));
}

TEST_F(ConnectionHandshakeTest,
       RejectsEventsInTheWrongStateAndStartsANewGenerationAfterCancel) {
  takion::ConnectionHandshake handshake;
  expect_action<takion::Rejected>(
      handshake.handle(takion::CookieAcknowledgementReceived{{1, 1}, {1}, 0}));
  expect_action<takion::Rejected>(handshake.handle(takion::Cancel{0}));
  expect_action<takion::Rejected>(handshake.handle(takion::Start{{0}, {1}, 0}));
  EXPECT_EQ(handshake.snapshot().state, takion::HandshakeState::idle);

  const auto& first = expect_action<takion::SendInit>(
      handshake.handle(takion::Start{{1}, {2}, 0}));
  expect_action<takion::Rejected>(handshake.handle(takion::Cancel{99}));
  EXPECT_EQ(handshake.snapshot().state,
            takion::HandshakeState::await_init_acknowledgement);
  const auto& cancelled =
      expect_action<takion::Failed>(handshake.handle(takion::Cancel{1}));
  EXPECT_EQ(cancelled.error, takion::HandshakeError::cancelled);

  const auto& second = expect_action<takion::SendInit>(
      handshake.handle(takion::Start{{3}, {4}, 10}));
  EXPECT_EQ(second.identity.generation, first.identity.generation + 1);
  expect_action<takion::Rejected>(handshake.handle(ack_event(first.identity)));
  EXPECT_EQ(handshake.snapshot().generation, second.identity.generation);
  EXPECT_EQ(handshake.snapshot().state,
            takion::HandshakeState::await_init_acknowledgement);
}

TEST_F(ConnectionHandshakeTest,
       RejectsARepeatedStartAndAWrongCookieTagWithoutConnecting) {
  takion::ConnectionHandshake handshake;
  const auto& init = expect_action<takion::SendInit>(
      handshake.handle(takion::Start{{50}, {51}, 0}));
  expect_action<takion::Rejected>(
      handshake.handle(takion::Start{{50}, {51}, 1}));
  const auto& send_cookie = expect_action<takion::SendCookie>(
      handshake.handle(takion::InitAcknowledgementReceived{
          init.identity, {50}, valid_ack(), 2}));
  expect_action<takion::Rejected>(handshake.handle(
      takion::CookieAcknowledgementReceived{send_cookie.identity, {999}, 3}));
  EXPECT_FALSE(handshake.snapshot().established.has_value());
  EXPECT_EQ(handshake.snapshot().state,
            takion::HandshakeState::await_cookie_acknowledgement);
}

TEST_F(ConnectionHandshakeTest,
       RejectsLateEventsOnceConnectedWithoutChangingState) {
  takion::ConnectionHandshake handshake;
  const auto& init = expect_action<takion::SendInit>(
      handshake.handle(takion::Start{{0x11223344}, {61}, 0}));
  const auto& send_cookie = expect_action<takion::SendCookie>(
      handshake.handle(ack_event(init.identity)));
  expect_action<takion::Connected>(
      handshake.handle(takion::CookieAcknowledgementReceived{
          send_cookie.identity, {0x11223344}, 1}));
  const auto before = handshake.snapshot();

  const auto& repeated_start = expect_action<takion::Rejected>(
      handshake.handle(takion::Start{{61}, {62}, 2}));
  EXPECT_EQ(repeated_start.error, takion::HandshakeError::wrong_state);
  const auto& late_ack = expect_action<takion::Rejected>(handshake.handle(
      ack_event(takion::HandshakeIdentity{before.generation, before.attempt})));
  EXPECT_EQ(late_ack.error, takion::HandshakeError::wrong_state);
  const auto& late_timeout =
      expect_action<takion::Rejected>(handshake.handle(takion::Timeout{
          takion::HandshakeIdentity{before.generation, before.attempt}, 3}));
  EXPECT_EQ(late_timeout.error, takion::HandshakeError::wrong_state);
  const auto& late_cancel = expect_action<takion::Rejected>(
      handshake.handle(takion::Cancel{before.generation}));
  EXPECT_EQ(late_cancel.error, takion::HandshakeError::wrong_state);

  EXPECT_EQ(handshake.snapshot().state, before.state);
  EXPECT_EQ(handshake.snapshot().generation, before.generation);
  EXPECT_EQ(handshake.snapshot().attempt, before.attempt);
  EXPECT_EQ(handshake.snapshot().deadline.monotonic_microseconds,
            before.deadline.monotonic_microseconds);
  EXPECT_TRUE(handshake.snapshot().established.has_value());
}

TEST_F(ConnectionHandshakeTest, SaturatesADeadlinePastTheEndOfTime) {
  takion::ConnectionHandshake handshake({3, 10});
  const auto maximum = UINT64_MAX;
  const auto& init = expect_action<takion::SendInit>(
      handshake.handle(takion::Start{{1}, {2}, maximum - 2}));
  EXPECT_EQ(init.deadline.monotonic_microseconds, maximum);
}

}  // namespace
}  // namespace takion_test
