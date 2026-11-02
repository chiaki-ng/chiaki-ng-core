#include "tests/unit/core/net/protocol/takion/client_rig.hpp"

#include <gtest/gtest.h>

namespace takion_test {
namespace {

class TakionClientDisconnect : public ClientRig {};

TEST_F(TakionClientDisconnect, IsSentBestEffortBeforeStoppingAndClosesOnce) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const auto sent_before = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->request_stop_with_disconnect(
      takion::Disconnect{"Client Disconnecting", std::nullopt}));
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
  const auto outbound =
      decode_outbound_control(fixture.transport->sent, sent_before, 1);
  const takion::ControlCodec codec;
  const auto decoded = codec.decode(outbound.payload);
  ASSERT_TRUE(decoded);
  const auto* disconnect = std::get_if<takion::Disconnect>(&decoded.value());
  ASSERT_TRUE(disconnect);
  EXPECT_EQ(disconnect->reason, "Client Disconnecting");
  EXPECT_EQ(fixture.transport->close_count, 1);
  const auto sent_after = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->request_stop_with_disconnect(
      takion::Disconnect{"Client Disconnecting", std::nullopt}));
  ASSERT_EQ(fixture.transport->sent.size(), sent_after);
  EXPECT_EQ(fixture.transport->close_count, 1);

  ClientHarness idle;
  EXPECT_TRUE(idle.client->request_stop_with_disconnect(
      takion::Disconnect{"Client Disconnecting", std::nullopt}));
  EXPECT_TRUE(idle.transport->sent.empty());
  EXPECT_EQ(idle.transport->close_count, 1);

  ClientHarness send_failure;
  ASSERT_NO_FATAL_FAILURE(send_failure.connect());
  const auto sent_before_failure = send_failure.transport->sent.size();
  send_failure.transport->fail_send = true;
  EXPECT_TRUE(send_failure.client->request_stop_with_disconnect(
      takion::Disconnect{"Client Disconnecting", std::nullopt}));
  EXPECT_EQ(send_failure.client->snapshot().state, takion::ClientState::closed);
  EXPECT_EQ(send_failure.events.count<takion::client_event::PacketRejected>(),
            1);
  EXPECT_EQ(send_failure.events.count<takion::client_event::TerminalFailure>(),
            0);
  EXPECT_TRUE(send_failure.client->request_stop_with_disconnect(
      takion::Disconnect{"Client Disconnecting", std::nullopt}));
  ASSERT_EQ(send_failure.transport->sent.size(), sent_before_failure);
  EXPECT_EQ(send_failure.transport->close_count, 1);
}

TEST_F(TakionClientDisconnect, IsSentBeforeCryptoIsInstalled) {
  takion::ClientConfig config;
  config.crypto_policy = takion::CryptoPolicy::required;
  config.allow_plain_control_until_crypto = true;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  EXPECT_FALSE(fixture.client->snapshot().crypto_installed);
  const auto sent_before = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->request_stop_with_disconnect(
      takion::Disconnect{"Client Disconnecting", std::nullopt}));
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
  const auto outbound =
      decode_outbound_control(fixture.transport->sent, sent_before, 1);
  const takion::ControlCodec codec;
  const auto decoded = codec.decode(outbound.payload);
  ASSERT_TRUE(decoded);
  const auto* disconnect = std::get_if<takion::Disconnect>(&decoded.value());
  ASSERT_TRUE(disconnect);
  EXPECT_EQ(disconnect->reason, "Client Disconnecting");
  EXPECT_EQ(fixture.transport->close_count, 1);
}

TEST_F(TakionClientDisconnect, DrainsUntilTheDisconnectIsAcknowledged) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  fixture.transport->waits = true;
  const auto sent_before = fixture.transport->sent.size();
  fixture.transport->on_wait = [sent_before](ClientTransport& transport) {
    EXPECT_EQ(transport.close_count, 0);
    transport.incoming.push_back(
        datagram::OwnedDatagram{std::byte{0x02}, std::byte{0}, std::byte{0}});
    if (transport.readable_waits.size() == 2)
      transport.incoming.push_back(
          acknowledgement_of(data_sequence(transport.sent[sent_before])));
  };
  const auto rejected_before =
      fixture.events.count<takion::client_event::PacketRejected>();
  EXPECT_TRUE(fixture.client->request_stop_with_disconnect(
      takion::Disconnect{"Client Disconnecting", std::nullopt}, lean_drain));
  ASSERT_EQ(fixture.transport->readable_waits.size(), 2);
  EXPECT_TRUE(fixture.transport->incoming.empty());
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(),
            rejected_before);
  EXPECT_EQ(fixture.transport->close_count, 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::Closed>(), 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::ReliableAcked>(), 1);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::closed);
  EXPECT_LT(fixture.clock.now_us, 1'000'000);
  const takion::ControlCodec codec;
  for (std::size_t index = sent_before; index < fixture.transport->sent.size();
       ++index) {
    const auto outbound =
        decode_outbound_control(fixture.transport->sent, index, 1);
    const auto decoded = codec.decode(outbound.payload);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(std::holds_alternative<takion::Disconnect>(*decoded));
  }
  EXPECT_TRUE(fixture.client->request_stop_with_disconnect(
      takion::Disconnect{"Client Disconnecting", std::nullopt}, lean_drain));
  EXPECT_EQ(fixture.transport->close_count, 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::Closed>(), 1);
}

TEST_F(TakionClientDisconnect, DrainResendsAndGivesUpAtItsBound) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  fixture.transport->waits = true;
  const auto started = fixture.clock.now_us;
  const auto sent_before = fixture.transport->sent.size();
  fixture.transport->on_wait = [](ClientTransport& transport) {
    EXPECT_EQ(transport.close_count, 0);
  };
  EXPECT_TRUE(fixture.client->request_stop_with_disconnect(
      takion::Disconnect{"Client Disconnecting", std::nullopt},
      std::chrono::milliseconds{900}));
  EXPECT_EQ(fixture.clock.now_us, started + 900'000);
  EXPECT_GT(fixture.transport->sent.size(), sent_before + 1);
  for (std::size_t index = sent_before + 1;
       index < fixture.transport->sent.size(); ++index)
    EXPECT_EQ(fixture.transport->sent[index],
              fixture.transport->sent[sent_before]);
  EXPECT_EQ(fixture.transport->close_count, 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::Closed>(), 1);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::closed);

  ClientHarness undrained;
  ASSERT_NO_FATAL_FAILURE(undrained.connect());
  undrained.transport->waits = true;
  EXPECT_TRUE(undrained.client->request_stop_with_disconnect(
      takion::Disconnect{"Client Disconnecting", std::nullopt}));
  EXPECT_TRUE(undrained.transport->readable_waits.empty());
  EXPECT_EQ(undrained.transport->close_count, 1);
}

TEST_F(TakionClientDisconnect, DrainIsItsOwnLifecycleState) {
  namespace detail = takion::client_detail;
  detail::LifecycleContext context{true};
  boost::sml::sm<detail::LifecycleFsm> machine{context};
  machine.process_event(detail::StartRequested{});
  machine.process_event(detail::StartMaterialReady{});
  machine.process_event(detail::HandshakeConnected{});
  EXPECT_TRUE(machine.is(boost::sml::state<detail::ConnectedState>));
  context.effects.clear();

  machine.process_event(detail::DisconnectSent{500});
  EXPECT_TRUE(machine.is(boost::sml::state<detail::DisconnectingState>));
  ASSERT_TRUE(context.drain);
  EXPECT_EQ(context.drain->deadline_us, 500);
  const auto drain = context.drain->id;
  EXPECT_TRUE(context.effects.empty());

  machine.process_event(detail::DisconnectSettled{drain + 1});
  EXPECT_TRUE(machine.is(boost::sml::state<detail::DisconnectingState>));
  EXPECT_TRUE(context.effects.empty());

  machine.process_event(detail::StopRequested{false});
  EXPECT_TRUE(machine.is(boost::sml::state<detail::StoppingState>));
  ASSERT_FALSE(context.drain);
  ASSERT_TRUE(context.close_attempted);
  ASSERT_EQ(context.effects.size(), 1);
  const auto* close = std::get_if<detail::CloseTransport>(&context.effects[0]);
  ASSERT_TRUE(close);
  EXPECT_FALSE(close->emit_closed);
  context.effects.clear();

  machine.process_event(detail::DisconnectSettled{drain});
  EXPECT_TRUE(machine.is(boost::sml::state<detail::StoppingState>));
  EXPECT_TRUE(context.effects.empty());
  machine.process_event(detail::CloseCompleted{false});
  EXPECT_TRUE(machine.is(boost::sml::state<detail::ClosedState>));
  machine.process_event(detail::DisconnectSettled{drain});
  EXPECT_TRUE(machine.is(boost::sml::state<detail::ClosedState>));

  detail::LifecycleContext failing{true};
  boost::sml::sm<detail::LifecycleFsm> failed{failing};
  failed.process_event(detail::StartRequested{});
  failed.process_event(detail::StartMaterialReady{});
  failed.process_event(detail::HandshakeConnected{});
  failed.process_event(detail::DisconnectSent{500});
  failed.process_event(
      detail::TerminalFailure{takion::ClientError::transport_failure});
  EXPECT_TRUE(failed.is(boost::sml::state<detail::FailedState>));
  EXPECT_FALSE(failing.drain);
  EXPECT_EQ(failing.terminal_error, takion::ClientError::transport_failure);
  failed.process_event(detail::DisconnectSettled{1});
  EXPECT_TRUE(failed.is(boost::sml::state<detail::FailedState>));
}

}  // namespace
}  // namespace takion_test
