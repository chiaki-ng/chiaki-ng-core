#include "tests/unit/core/net/protocol/takion/client_rig.hpp"

#include <gtest/gtest.h>

namespace takion_test {
namespace {

class TakionClientLifecycle : public ClientRig {};

TEST_F(TakionClientLifecycle, ReleasingTheTransportStopsWithoutClosingIt) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  auto released = fixture.client->release_transport();
  ASSERT_TRUE(released);
  EXPECT_EQ(released.value().get(), fixture.transport);
  EXPECT_EQ(fixture.transport->close_count, 0);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::closed);
  EXPECT_EQ(fixture.events.count<takion::client_event::Closed>(), 1);

  const auto posted =
      fixture.client->post(takion::SendControl{takion::Heartbeat{}});
  ASSERT_FALSE(posted);
  EXPECT_EQ(posted.error(), takion::ClientError::invalid_state);
  const auto second = fixture.client->release_transport();
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error(), takion::ClientError::invalid_state);

  const std::array payload{std::byte{0x42}};
  const auto sent = released.value()->send(payload);
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value(), payload.size());
  fixture.client.reset();
  EXPECT_EQ(fixture.events.count<takion::client_event::Closed>(), 1);
  EXPECT_EQ(fixture.transport->close_count, 0);
}

TEST_F(TakionClientLifecycle, RejectsCommandsWhileIdleWithoutQueueingThem) {
  ClientHarness fixture;
  const auto stop_result = fixture.client->post(takion::Stop{});
  ASSERT_FALSE(stop_result);
  EXPECT_EQ(stop_result.error(), takion::ClientError::invalid_state);
  const auto control_result =
      fixture.client->post(takion::SendControl{takion::Heartbeat{}});
  ASSERT_FALSE(control_result);
  EXPECT_EQ(control_result.error(), takion::ClientError::invalid_state);
  const auto probe_result = fixture.client->post(
      takion::SendProbe{takion::SenkushaProbeMedia::video, {}, 9});
  ASSERT_FALSE(probe_result);
  EXPECT_EQ(probe_result.error(), takion::ClientError::invalid_state);
  const auto congestion_result = fixture.client->post(
      takion::SendCongestion{takion::CongestionValues{0x1122, 0x3344, 0x5566}});
  ASSERT_FALSE(congestion_result);
  EXPECT_EQ(congestion_result.error(), takion::ClientError::invalid_state);
  const auto crypto_result = fixture.client->post(takion::InstallCrypto{
      nullptr,
      takion::CryptoMaterial{takion::SecretBytes{}},
      takion::CryptoMaterial{takion::SecretBytes{}},
      0,
      0,
  });
  ASSERT_FALSE(crypto_result);
  EXPECT_EQ(crypto_result.error(), takion::ClientError::invalid_state);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::idle);
  EXPECT_EQ(fixture.client->snapshot().queued_commands, 0);
}

TEST_F(TakionClientLifecycle, StoppingDuringTheHandshakeHaltsAllProgress) {
  ClientHarness fixture;
  EXPECT_TRUE(fixture.client->start());
  const auto sent_before = fixture.transport->sent.size();
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
  EXPECT_TRUE(fixture.client->request_stop());
  fixture.clock.now_us = 100'000;
  EXPECT_TRUE(fixture.client->pump_once());

  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::closed);
  EXPECT_EQ(fixture.client->snapshot().queued_commands, 0);
  ASSERT_EQ(fixture.transport->sent.size(), sent_before);
  ASSERT_EQ(fixture.transport->incoming.size(), 1);
  EXPECT_EQ(fixture.transport->close_count, 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::Connected>(), 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::ProfileCommitted>(), 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 0);
}

TEST_F(TakionClientLifecycle, StoppingHaltsRetransmissionAndMediaDispatch) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto sent_before = fixture.transport->sent.size();
  const std::vector<std::byte> media_bytes{std::byte{1}, std::byte{2}};
  const std::vector<datagram::OwnedDatagram> media{
      packet(takion::PacketFamily::video, media_bytes)};
  fixture.transport->incoming =
      std::deque<datagram::OwnedDatagram>(media.begin(), media.end());

  EXPECT_TRUE(fixture.client->request_stop());
  fixture.clock.now_us = 100'000;
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::closed);
  ASSERT_EQ(fixture.transport->sent.size(), sent_before);
  ASSERT_EQ(fixture.transport->incoming.size(), 1);
  EXPECT_EQ(fixture.transport->close_count, 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::EncodedVideo>(), 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 0);
}

TEST_F(TakionClientLifecycle,
       StaysIdleWhenRandomFailsAndClosesOnceWhenStopped) {
  ClientHarness fixture;
  fixture.random.fail = true;
  const auto random_failure = fixture.client->start();
  ASSERT_FALSE(random_failure);
  EXPECT_EQ(random_failure.error(), takion::ClientError::random_failure);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::idle);
  EXPECT_EQ(fixture.transport->close_count, 0);

  fixture.random.fail = false;
  EXPECT_TRUE(fixture.client->start());
  ASSERT_EQ(fixture.transport->sent.size(), 1);

  ClientHarness stopped;
  EXPECT_TRUE(stopped.client->request_stop());
  EXPECT_EQ(stopped.client->snapshot().state, takion::ClientState::closed);
  EXPECT_EQ(stopped.transport->close_count, 1);
  EXPECT_EQ(stopped.events.count<takion::client_event::Closed>(), 1);
  EXPECT_TRUE(stopped.client->request_stop());
  EXPECT_EQ(stopped.transport->close_count, 1);

  ClientHarness close_failure;
  close_failure.transport->fail_close = true;
  const auto close_result = close_failure.client->request_stop();
  ASSERT_FALSE(close_result);
  EXPECT_EQ(close_result.error(), takion::ClientError::transport_failure);
  EXPECT_EQ(close_failure.client->snapshot().state,
            takion::ClientState::failed);
  EXPECT_EQ(close_failure.transport->close_count, 1);
  EXPECT_EQ(close_failure.events.count<takion::client_event::TerminalFailure>(),
            1);
  EXPECT_TRUE(close_failure.client->request_stop());
  EXPECT_EQ(close_failure.transport->close_count, 1);
  EXPECT_EQ(close_failure.events.count<takion::client_event::Closed>(), 0);
}

TEST_F(TakionClientLifecycle, RefusesToStartWithAnInvalidConfiguration) {
  test_support::FakeClock clock{0};
  ClientRandom random;
  ClientEvents events;
  takion::Client client(nullptr, clock, threading, random, events,
                        takion::ClientConfig{});
  const auto result = client.start();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), takion::ClientError::invalid_configuration);
  EXPECT_EQ(client.snapshot().state, takion::ClientState::idle);

  takion::ClientConfig zero_command_pump;
  zero_command_pump.limits.maximum_commands_per_pump = 0;
  ClientHarness zero_command{zero_command_pump};
  const auto zero_command_result = zero_command.client->start();
  ASSERT_FALSE(zero_command_result);
  EXPECT_EQ(zero_command_result.error(),
            takion::ClientError::invalid_configuration);
  EXPECT_EQ(zero_command.client->snapshot().state, takion::ClientState::idle);

  takion::ClientConfig equal_channels;
  equal_channels.senkusha_control_channel = equal_channels.control_channel;
  ClientHarness equal{equal_channels};
  const auto equal_result = equal.client->start();
  ASSERT_FALSE(equal_result);
  EXPECT_EQ(equal_result.error(), takion::ClientError::invalid_configuration);
  EXPECT_EQ(equal.client->snapshot().state, takion::ClientState::idle);

  takion::ClientConfig ack_on_control;
  ack_on_control.stream_info_ack_channel = ack_on_control.control_channel;
  ClientHarness control_collision{ack_on_control};
  const auto control_collision_result = control_collision.client->start();
  ASSERT_FALSE(control_collision_result);
  EXPECT_EQ(control_collision_result.error(),
            takion::ClientError::invalid_configuration);

  takion::ClientConfig ack_on_senkusha;
  ack_on_senkusha.stream_info_ack_channel =
      ack_on_senkusha.senkusha_control_channel;
  ClientHarness senkusha_collision{ack_on_senkusha};
  const auto senkusha_collision_result = senkusha_collision.client->start();
  ASSERT_FALSE(senkusha_collision_result);
  EXPECT_EQ(senkusha_collision_result.error(),
            takion::ClientError::invalid_configuration);
}

TEST_F(TakionClientLifecycle, FailsAndClosesOnACommandItsStateForbids) {
  ClientHarness missing_provider;
  EXPECT_TRUE(missing_provider.client->start());
  EXPECT_TRUE(missing_provider.client->post(takion::InstallCrypto{
      nullptr,
      takion::CryptoMaterial{takion::SecretBytes{}},
      takion::CryptoMaterial{takion::SecretBytes{}},
      0,
      0,
  }));
  const auto crypto_result = missing_provider.client->pump_once();
  ASSERT_FALSE(crypto_result);
  EXPECT_EQ(crypto_result.error(), takion::ClientError::crypto_failure);
  EXPECT_EQ(missing_provider.client->snapshot().state,
            takion::ClientState::failed);
  EXPECT_EQ(missing_provider.transport->close_count, 1);

  takion::ClientConfig negotiated;
  negotiated.profile_selection = takion::NegotiatedTakionProfiles{{7}};
  ClientHarness uncommitted{negotiated};
  ASSERT_NO_FATAL_FAILURE(uncommitted.connect());
  EXPECT_FALSE(uncommitted.client->snapshot().profile_committed);
  EXPECT_TRUE(
      uncommitted.client->post(takion::SendControl{takion::Heartbeat{}}));
  const auto control_result = uncommitted.client->pump_once();
  ASSERT_FALSE(control_result);
  EXPECT_EQ(control_result.error(), takion::ClientError::invalid_state);
  EXPECT_EQ(uncommitted.client->snapshot().state, takion::ClientState::failed);
  EXPECT_EQ(uncommitted.transport->close_count, 1);
}

TEST_F(TakionClientLifecycle, FailsWhenCryptoIsInstalledWithoutAProvider) {
  ClientHarness handshaking;
  EXPECT_TRUE(handshaking.client->start());
  EXPECT_TRUE(handshaking.client->post(takion::InstallCrypto{
      nullptr,
      takion::CryptoMaterial{takion::SecretBytes{}},
      takion::CryptoMaterial{takion::SecretBytes{}},
      0,
      0,
  }));
  const auto handshake_result = handshaking.client->pump_once();
  ASSERT_FALSE(handshake_result);
  EXPECT_EQ(handshake_result.error(), takion::ClientError::crypto_failure);
  EXPECT_EQ(handshaking.client->snapshot().state, takion::ClientState::failed);
  EXPECT_EQ(handshaking.transport->close_count, 1);
  EXPECT_EQ(handshaking.events.count<takion::client_event::TerminalFailure>(),
            1);

  ClientHarness connected;
  ASSERT_NO_FATAL_FAILURE(connected.connect());
  EXPECT_TRUE(connected.client->post(takion::InstallCrypto{
      nullptr,
      takion::CryptoMaterial{takion::SecretBytes{}},
      takion::CryptoMaterial{takion::SecretBytes{}},
      0,
      0,
  }));
  const auto connected_result = connected.client->pump_once();
  ASSERT_FALSE(connected_result);
  EXPECT_EQ(connected_result.error(), takion::ClientError::crypto_failure);
  EXPECT_EQ(connected.client->snapshot().state, takion::ClientState::failed);
  EXPECT_EQ(connected.transport->close_count, 1);
  EXPECT_EQ(connected.events.count<takion::client_event::TerminalFailure>(), 1);
}

TEST_F(TakionClientLifecycle,
       ClosesOnceForAQueuedStopAndKeepsTheLaterCommandQueued) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  EXPECT_TRUE(fixture.client->post(takion::Stop{}));
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::closed);
  EXPECT_EQ(fixture.transport->close_count, 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::TerminalFailure>(), 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::Closed>(), 1);
  EXPECT_EQ(fixture.client->snapshot().queued_commands, 1);
}

}  // namespace
}  // namespace takion_test
