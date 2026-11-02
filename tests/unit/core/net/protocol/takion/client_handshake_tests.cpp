#include "tests/unit/core/net/protocol/takion/client_rig.hpp"

#include <gtest/gtest.h>

namespace takion_test {
namespace {

class TakionClientHandshake : public ClientRig {};

TEST_F(TakionClientHandshake, ResendsTheInitWhenAReceiveIsRefused) {
  ClientHarness fixture;
  EXPECT_TRUE(fixture.client->start());
  ASSERT_EQ(fixture.transport->sent.size(), 1);
  for (std::size_t attempt = 2; attempt <= 5; ++attempt) {
    fixture.transport->failed_receives = 1;
    EXPECT_TRUE(fixture.client->pump_once());
    EXPECT_EQ(fixture.client->snapshot().state,
              takion::ClientState::handshaking);
    ASSERT_EQ(fixture.transport->sent.size(), attempt);
    EXPECT_EQ(fixture.transport->sent.back(), fixture.transport->sent.front());
  }
}

TEST_F(TakionClientHandshake,
       ConnectsOnceThroughADuplicateInitAcknowledgement) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect(true));
  EXPECT_EQ(fixture.events.count<takion::client_event::Connected>(), 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::ProfileCommitted>(), 1);
  EXPECT_EQ(fixture.client->snapshot().active_version, 7);
  EXPECT_TRUE(fixture.client->request_stop());
  EXPECT_TRUE(fixture.client->request_stop());
  EXPECT_EQ(fixture.transport->close_count, 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::Closed>(), 1);
}

TEST_F(TakionClientHandshake, EndsOnAnInitAcknowledgementWithAZeroTag) {
  ClientHarness fixture;
  EXPECT_TRUE(fixture.client->start());
  const takion::InitAcknowledgementValues acknowledgement{
      {takion::VerificationTag{0},
       takion::AdvertisedReceiveWindow{
           takion::default_advertised_receive_window},
       takion::StreamCount{100}, takion::StreamCount{100},
       takion::InitialSequenceNumber{0x01020304}},
      cookie()};
  fixture.transport->incoming.push_back(
      control_packet(takion::InitAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          acknowledgement}));
  const auto pumped = fixture.client->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), takion::ClientError::handshake_failure);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::failed);
  EXPECT_EQ(fixture.events.count<takion::client_event::TerminalFailure>(), 1);
  ASSERT_EQ(fixture.transport->sent.size(), 1);
}

TEST_F(TakionClientHandshake, RetriesUntilItsAttemptsRunOutAndThenFails) {
  takion::ClientConfig config;
  config.profile_selection = takion::NegotiatedTakionProfiles{{7}};
  config.handshake_retry = takion::HandshakeRetryPolicy{2, 5};
  ClientHarness fixture{config};

  EXPECT_TRUE(fixture.client->start());
  const auto repeated_start = fixture.client->start();
  ASSERT_FALSE(repeated_start);
  EXPECT_EQ(repeated_start.error(), takion::ClientError::invalid_state);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::handshaking);
  ASSERT_EQ(fixture.transport->sent.size(), 1);

  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  const auto rejected = fixture.client->pump_once();
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), takion::ClientError::invalid_state);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::failed);
  EXPECT_EQ(fixture.transport->close_count, 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::TerminalFailure>(), 1);
  const auto post_after_failure = fixture.client->post(takion::Stop{});
  ASSERT_FALSE(post_after_failure);
  EXPECT_EQ(post_after_failure.error(), takion::ClientError::invalid_state);

  ClientHarness retry{config};
  EXPECT_TRUE(retry.client->start());
  retry.clock.now_us = 5;
  EXPECT_TRUE(retry.client->pump_once());
  ASSERT_EQ(retry.transport->sent.size(), 2);
  EXPECT_EQ(retry.transport->sent.back(), retry.transport->sent.front());
  EXPECT_EQ(retry.client->snapshot().state, takion::ClientState::handshaking);

  retry.clock.now_us = 10;
  const auto exhausted = retry.client->pump_once();
  ASSERT_FALSE(exhausted);
  EXPECT_EQ(exhausted.error(), takion::ClientError::handshake_failure);
  EXPECT_EQ(retry.client->snapshot().state, takion::ClientState::failed);
  EXPECT_EQ(retry.transport->close_count, 1);
  EXPECT_EQ(retry.events.count<takion::client_event::TerminalFailure>(), 1);
}

TEST_F(TakionClientHandshake, IgnoresStaleTimeoutsAndInputOnceFailed) {
  takion::ClientConfig config;
  config.profile_selection = takion::FixedTakionProfile{7};
  config.handshake_retry = takion::HandshakeRetryPolicy{2, 5};
  ClientHarness fixture{config};

  EXPECT_TRUE(fixture.client->start());
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::handshaking);
  ASSERT_EQ(fixture.transport->sent.size(), 1);

  fixture.clock.now_us = 5;
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), 2);
  const auto sent_after_retry = fixture.transport->sent.size();

  fixture.clock.now_us = 10;
  const auto failed = fixture.client->pump_once();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), takion::ClientError::handshake_failure);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::failed);
  EXPECT_EQ(fixture.transport->close_count, 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::TerminalFailure>(), 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::Connected>(), 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::ProfileCommitted>(), 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::Closed>(), 0);

  fixture.clock.now_us = 1000;
  for (int index = 0; index < 2; ++index) {
    const auto stale_timeout = fixture.client->pump_once();
    ASSERT_FALSE(stale_timeout);
    EXPECT_EQ(stale_timeout.error(), takion::ClientError::handshake_failure);
  }
  ASSERT_EQ(fixture.transport->sent.size(), sent_after_retry);
  ASSERT_EQ(fixture.events.events.size(), 1);
  EXPECT_EQ(fixture.transport->close_count, 1);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::failed);

  fixture.transport->incoming.push_back(
      control_packet(takion::InitAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::InitAcknowledgementValues{
              {takion::VerificationTag{0xa1b2c3d4},
               takion::AdvertisedReceiveWindow{
                   takion::default_advertised_receive_window},
               takion::StreamCount{100}, takion::StreamCount{100},
               takion::InitialSequenceNumber{0x01020304}},
              cookie()}}));
  const auto stale_input = fixture.client->pump_once();
  ASSERT_FALSE(stale_input);
  EXPECT_EQ(stale_input.error(), takion::ClientError::handshake_failure);
  ASSERT_EQ(fixture.transport->incoming.size(), 1);
  ASSERT_EQ(fixture.transport->sent.size(), sent_after_retry);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::failed);
  EXPECT_EQ(fixture.transport->close_count, 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::TerminalFailure>(), 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::Connected>(), 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::ProfileCommitted>(), 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::Closed>(), 0);
}

TEST_F(TakionClientHandshake, FailsOnceWhenTheFirstInitCannotBeSent) {
  ClientHarness fixture;
  fixture.transport->fail_send = true;
  EXPECT_TRUE(fixture.client->start());
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::failed);
  EXPECT_TRUE(fixture.transport->sent.empty());
  EXPECT_EQ(fixture.transport->close_count, 1);
  const auto terminal =
      fixture.events.values<takion::client_event::TerminalFailure>();
  ASSERT_EQ(terminal.size(), 1);
  EXPECT_EQ(terminal.front().error, takion::ClientError::send_dropped);
  EXPECT_EQ(fixture.events.count<takion::client_event::Closed>(), 0);
  const auto pumped = fixture.client->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), takion::ClientError::send_dropped);
  EXPECT_EQ(fixture.events.count<takion::client_event::TerminalFailure>(), 1);
  EXPECT_EQ(fixture.transport->close_count, 1);

  const auto rejected = fixture.client->post(takion::Stop{});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), takion::ClientError::invalid_state);
}

}  // namespace
}  // namespace takion_test
