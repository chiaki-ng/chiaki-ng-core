#include "tests/unit/core/net/protocol/takion/client_rig.hpp"

#include <gtest/gtest.h>

namespace takion_test {
namespace {

class TakionClientProfile : public ClientRig {};

TEST_F(TakionClientProfile,
       CommitsANegotiatedVersionOnceItsAcknowledgementArrives) {
  takion::ClientConfig config;
  config.profile_selection = takion::NegotiatedTakionProfiles{{7, 20}};
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  EXPECT_FALSE(fixture.client->snapshot().profile_committed);
  auto request = decode_data(fixture.transport->sent.back());
  const auto request_sequence =
      (std::to_integer<std::uint32_t>(request.payload[0]) << 24U) |
      (std::to_integer<std::uint32_t>(request.payload[1]) << 16U) |
      (std::to_integer<std::uint32_t>(request.payload[2]) << 8U) |
      std::to_integer<std::uint32_t>(request.payload[3]);
  fixture.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{request_sequence, 0x19000, {}, {}}}));

  const takion::ControlCodec codec;
  auto selected = codec.encode(takion::ProtocolRequestAck{20});
  ASSERT_TRUE(selected);
  takion::ReliableChannel peer =
      takion::ReliableChannel::create({.initial_local_sequence = 0xa1b2c3d4,
                                       .initial_remote_sequence = 0x11223344,
                                       .maximum_message_size = 1024,
                                       .maximum_fragment_size = 1200,
                                       .maximum_fragment_count = 8,
                                       .retransmission_window_size = 8,
                                       .receive_reorder_size = 8,
                                       .retransmission_timeout_us = 100,
                                       .maximum_retries = 2})
          .value();
  auto sent =
      peer.send(1, takion::MessageDataType{0}, selected.value().bytes(), 0);
  fixture.transport->incoming.push_back(control_packet(takion::DataChunk{
      takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
      std::vector<std::byte>(sent->fragments[0].payload.begin(),
                             sent->fragments[0].payload.end())}));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_TRUE(fixture.client->snapshot().profile_committed);
  EXPECT_EQ(fixture.client->snapshot().active_version, 20);

  takion::ClientConfig invalid_config;
  invalid_config.profile_selection = takion::NegotiatedTakionProfiles{{7}};
  ClientHarness invalid{invalid_config};
  ASSERT_NO_FATAL_FAILURE(invalid.connect());
  auto invalid_selected = codec.encode(takion::ProtocolRequestAck{20});
  takion::ReliableChannel invalid_peer =
      takion::ReliableChannel::create({.initial_local_sequence = 0xa1b2c3d4,
                                       .initial_remote_sequence = 0x11223344,
                                       .maximum_message_size = 1024,
                                       .maximum_fragment_size = 1200,
                                       .maximum_fragment_count = 8,
                                       .retransmission_window_size = 8,
                                       .receive_reorder_size = 8,
                                       .retransmission_timeout_us = 100,
                                       .maximum_retries = 2})
          .value();
  auto invalid_sent = invalid_peer.send(1, takion::MessageDataType{0},
                                        invalid_selected.value().bytes(), 0);
  invalid.transport->incoming.push_back(control_packet(takion::DataChunk{
      takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
      std::vector<std::byte>(invalid_sent->fragments[0].payload.begin(),
                             invalid_sent->fragments[0].payload.end())}));
  EXPECT_TRUE(invalid.client->pump_once());
  EXPECT_NE(invalid.client->snapshot().state, takion::ClientState::failed);
  EXPECT_EQ(invalid.events.count<takion::client_event::TerminalFailure>(), 0);
}

TEST_F(TakionClientProfile, CanKeepTheBootstrapWireFramingAfterNegotiation) {
  takion::ClientConfig config;
  config.profile_selection = takion::NegotiatedTakionProfiles{{7, 20}};
  config.negotiation_wire_policy =
      takion::NegotiationWirePolicy::retain_bootstrap_profile;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  auto request = decode_data(fixture.transport->sent.back());
  const auto request_sequence =
      (std::to_integer<std::uint32_t>(request.payload[0]) << 24U) |
      (std::to_integer<std::uint32_t>(request.payload[1]) << 16U) |
      (std::to_integer<std::uint32_t>(request.payload[2]) << 8U) |
      std::to_integer<std::uint32_t>(request.payload[3]);
  fixture.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{request_sequence, 0x19000, {}, {}}}));

  const takion::ControlCodec codec;
  auto selected = codec.encode(takion::ProtocolRequestAck{20});
  ASSERT_TRUE(selected);
  takion::ReliableChannel peer =
      takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4))
          .value();
  auto sent =
      peer.send(1, takion::MessageDataType{0}, selected.value().bytes(), 0);
  ASSERT_EQ(sent->fragments.size(), 1);
  fixture.transport->incoming.push_back(control_packet(takion::DataChunk{
      takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
      std::vector<std::byte>(sent->fragments[0].payload.begin(),
                             sent->fragments[0].payload.end())}));
  EXPECT_TRUE(fixture.client->pump_once());

  const auto snapshot = fixture.client->snapshot();
  ASSERT_TRUE(snapshot.profile_committed);
  EXPECT_EQ(snapshot.active_version, 7);
  const auto committed =
      fixture.events.values<takion::client_event::ProfileCommitted>();
  ASSERT_EQ(committed.size(), 1);
  EXPECT_EQ(committed.front().version, 20);
  EXPECT_TRUE(committed.front().features.contains(
      takion::Feature::extended_packet_headers));

  const auto boundary = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->pump_once());
  auto outbound =
      decode_outbound_control(fixture.transport->sent, boundary, 1, 7,
                              takion::PlainEnvelope{}, 1, request_sequence + 1);
  auto decoded = codec.decode(outbound.payload);
  ASSERT_TRUE(decoded);
  EXPECT_TRUE(std::holds_alternative<takion::Heartbeat>(decoded.value()));
}

TEST_F(TakionClientProfile, RoutesNegotiationMessagesDeliberately) {
  takion::ClientConfig config;
  config.profile_selection = takion::NegotiatedTakionProfiles{{7, 12, 20}};
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const takion::ControlCodec codec;
  auto request = codec.encode(takion::ProtocolRequest{{7, 12, 20}});
  ASSERT_TRUE(request);
  auto outbound = decode_outbound_control(fixture.transport->sent, 2, 1);
  EXPECT_TRUE(std::ranges::equal(outbound.payload, request.value().bytes()));
  EXPECT_EQ(outbound.channel, 1);
  EXPECT_EQ(outbound.data_type.known(), takion::MessageDataKind::protobuf);
  EXPECT_FALSE(fixture.client->snapshot().profile_committed);

  takion::ReliableChannel peer =
      takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4))
          .value();
  auto incoming_request =
      peer.send(1, takion::MessageDataType{0}, request.value().bytes(), 0);
  ASSERT_EQ(incoming_request->fragments.size(), 1);
  fixture.transport->incoming.push_back(control_packet(takion::DataChunk{
      takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
      std::vector<std::byte>(incoming_request->fragments[0].payload.begin(),
                             incoming_request->fragments[0].payload.end())}));
  auto selected_ack = codec.encode(takion::ProtocolRequestAck{20});
  ASSERT_TRUE(selected_ack);
  auto incoming_ack =
      peer.send(1, takion::MessageDataType{0}, selected_ack.value().bytes(), 0);
  ASSERT_EQ(incoming_ack->fragments.size(), 1);
  fixture.transport->incoming.push_back(control_packet(takion::DataChunk{
      takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
      std::vector<std::byte>(incoming_ack->fragments[0].payload.begin(),
                             incoming_ack->fragments[0].payload.end())}));

  const auto automatic_ack_at = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->pump_once());
  auto automatic =
      decode_outbound_control(fixture.transport->sent, automatic_ack_at + 1, 1,
                              7, takion::PlainEnvelope{}, 1, 0x11223345);
  EXPECT_TRUE(std::ranges::equal(
      automatic.payload,
      codec.encode(takion::ProtocolRequestAck{20}).value().bytes()));
  EXPECT_EQ(fixture.events.count<takion::client_event::ControlReceived>(), 2);
  EXPECT_FALSE(fixture.client->snapshot().profile_committed);

  fixture.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{0x11223345, 0x19000, {}, {}}}));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_TRUE(fixture.client->snapshot().profile_committed);
  EXPECT_EQ(fixture.client->snapshot().active_version, 20);
  EXPECT_EQ(fixture.events.count<takion::client_event::ProfileCommitted>(), 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::TerminalFailure>(), 0);

  const auto rejected_selection = [&](std::optional<std::uint32_t> selected) {
    takion::ClientConfig failure_config;
    failure_config.profile_selection = takion::NegotiatedTakionProfiles{{7}};
    ClientHarness failure{failure_config};
    ASSERT_NO_FATAL_FAILURE(failure.connect());
    const auto outgoing = decode_data(failure.transport->sent.back());
    const auto outgoing_sequence =
        (std::to_integer<std::uint32_t>(outgoing.payload[0]) << 24U) |
        (std::to_integer<std::uint32_t>(outgoing.payload[1]) << 16U) |
        (std::to_integer<std::uint32_t>(outgoing.payload[2]) << 8U) |
        std::to_integer<std::uint32_t>(outgoing.payload[3]);
    failure.transport->incoming.push_back(
        control_packet(takion::DataAcknowledgementChunk{
            takion::ControlHeader{
                takion::VerificationTag{0x11223344}, {}, 0, 0},
            takion::DataAcknowledgement{outgoing_sequence, 0x19000, {}, {}}}));
    auto encoded = codec.encode(takion::ProtocolRequestAck{selected});
    ASSERT_TRUE(encoded);
    takion::ReliableChannel failure_peer =
        takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4))
            .value();
    auto sent = failure_peer.send(1, takion::MessageDataType{0},
                                  encoded.value().bytes(), 0);
    ASSERT_EQ(sent->fragments.size(), 1);
    failure.transport->incoming.push_back(control_packet(takion::DataChunk{
        takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
        std::vector<std::byte>(sent->fragments[0].payload.begin(),
                               sent->fragments[0].payload.end())}));
    EXPECT_TRUE(failure.client->pump_once());
    EXPECT_NE(failure.client->snapshot().state, takion::ClientState::failed);
    EXPECT_EQ(failure.events.count<takion::client_event::TerminalFailure>(), 0);
    EXPECT_EQ(failure.events.count<takion::client_event::ControlReceived>(), 1);
    EXPECT_TRUE(failure.client->snapshot().profile_committed);
    EXPECT_EQ(failure.client->snapshot().active_version,
              failure_config.bootstrap_version);
    EXPECT_EQ(failure.transport->close_count, 0);
  };
  rejected_selection(std::nullopt);
  rejected_selection(8);
  rejected_selection(20);
}

TEST_F(TakionClientProfile, AdvertisesOnlyRegisteredVersions) {
  const std::vector<std::uint32_t> advertised{7,  9,  10, 11, 12, 13, 14, 15,
                                              16, 17, 18, 19, 20, 21, 23, 25};
  takion::ClientConfig config;
  config.profile_selection = takion::NegotiatedTakionProfiles{advertised};
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());

  const takion::ControlCodec codec;
  auto outbound = decode_outbound_control(fixture.transport->sent, 2, 1);
  auto decoded = codec.decode(outbound.payload);
  ASSERT_TRUE(decoded);
  const auto* request = std::get_if<takion::ProtocolRequest>(&decoded.value());
  ASSERT_TRUE(request);
  EXPECT_EQ(request->supported_versions, advertised);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  EXPECT_EQ(fixture.client->snapshot().active_version, 7);
  EXPECT_FALSE(fixture.client->snapshot().profile_committed);

  const auto rejected = [&](const std::vector<std::uint32_t>& versions) {
    takion::ClientConfig invalid;
    invalid.profile_selection = takion::NegotiatedTakionProfiles{versions};
    ClientHarness invalid_fixture{invalid};
    const auto result = invalid_fixture.client->start();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), takion::ClientError::invalid_configuration);
    EXPECT_EQ(invalid_fixture.client->snapshot().state,
              takion::ClientState::idle);
    EXPECT_TRUE(invalid_fixture.transport->sent.empty());
    EXPECT_EQ(invalid_fixture.events.count<takion::client_event::Connected>(),
              0);
    EXPECT_EQ(
        invalid_fixture.events.count<takion::client_event::ProfileCommitted>(),
        0);
    EXPECT_EQ(
        invalid_fixture.events.count<takion::client_event::TerminalFailure>(),
        0);
  };
  rejected({});
  rejected({8});
  rejected({22});
  rejected({24});
  rejected({7, 8});
}

TEST_F(TakionClientProfile, CommitWaitsForTheDatagramBudgetBoundary) {
  takion::ClientConfig config;
  config.profile_selection = takion::NegotiatedTakionProfiles{{7, 20}};
  config.limits.maximum_datagrams_per_pump = 2;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  EXPECT_FALSE(fixture.client->snapshot().profile_committed);

  auto request = decode_data(fixture.transport->sent.back());
  const auto request_sequence =
      (std::to_integer<std::uint32_t>(request.payload[0]) << 24U) |
      (std::to_integer<std::uint32_t>(request.payload[1]) << 16U) |
      (std::to_integer<std::uint32_t>(request.payload[2]) << 8U) |
      std::to_integer<std::uint32_t>(request.payload[3]);

  const takion::ControlCodec codec;
  auto selected = codec.encode(takion::ProtocolRequestAck{20});
  ASSERT_TRUE(selected);
  auto heartbeat = codec.encode(takion::Heartbeat{});
  ASSERT_TRUE(heartbeat);
  takion::ReliableChannel peer =
      takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4))
          .value();

  auto selection =
      peer.send(1, takion::MessageDataType{0}, selected.value().bytes(), 0);
  ASSERT_EQ(selection->fragments.size(), 1);
  fixture.transport->incoming.push_back(control_packet(takion::DataChunk{
      takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
      std::vector<std::byte>(selection->fragments[0].payload.begin(),
                             selection->fragments[0].payload.end())}));
  fixture.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{request_sequence, 0x19000, {}, {}}}));
  auto later_control =
      peer.send(1, takion::MessageDataType{0}, heartbeat.value().bytes(), 0);
  ASSERT_EQ(later_control->fragments.size(), 1);
  fixture.transport->incoming.push_back(control_packet(takion::DataChunk{
      takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
      std::vector<std::byte>(later_control->fragments[0].payload.begin(),
                             later_control->fragments[0].payload.end())}));
  ASSERT_EQ(fixture.transport->incoming.size(), 3);

  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->incoming.size(), 1);
  EXPECT_FALSE(fixture.client->snapshot().profile_committed);
  EXPECT_EQ(fixture.client->snapshot().active_version, 7);
  EXPECT_EQ(fixture.client->snapshot().outstanding_fragments, 0);
  EXPECT_FALSE(fixture.client->snapshot().incomplete_control_message);
  EXPECT_EQ(fixture.events.count<takion::client_event::ControlReceived>(), 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::ReliableAcked>(), 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::ProfileCommitted>(), 0);

  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_TRUE(fixture.transport->incoming.empty());
  EXPECT_TRUE(fixture.client->snapshot().profile_committed);
  EXPECT_EQ(fixture.client->snapshot().active_version, 20);
  EXPECT_EQ(fixture.events.count<takion::client_event::ControlReceived>(), 2);
  EXPECT_EQ(fixture.events.count<takion::client_event::ProfileCommitted>(), 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 0);
}

TEST_F(TakionClientProfile, CommitWaitsForTheFinalFragment) {
  takion::ClientConfig config;
  config.profile_selection = takion::NegotiatedTakionProfiles{{7, 20}};
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  auto request = decode_data(fixture.transport->sent.back());
  const auto request_sequence =
      (std::to_integer<std::uint32_t>(request.payload[0]) << 24U) |
      (std::to_integer<std::uint32_t>(request.payload[1]) << 16U) |
      (std::to_integer<std::uint32_t>(request.payload[2]) << 8U) |
      std::to_integer<std::uint32_t>(request.payload[3]);
  takion::ControlCodec codec;
  auto selected = codec.encode(takion::ProtocolRequestAck{20});
  ASSERT_TRUE(selected);
  takion::ReliableChannel peer =
      takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4, 11))
          .value();
  auto selection = peer.send(
      1, takion::MessageDataType{0}, selected.value().bytes(), 0, std::nullopt,
      takion::MessageFragmentation::split_to_fragment_size);
  EXPECT_GT(selection->fragments.size(), 1);
  fixture.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{request_sequence, 0x19000, {}, {}}}));
  for (const auto& fragment : selection->fragments) {
    EXPECT_FALSE(fixture.client->snapshot().profile_committed);
    fixture.transport->incoming.push_back(control_packet(takion::DataChunk{
        takion::ControlHeader{
            takion::VerificationTag{0x11223344},
            {},
            0,
            static_cast<std::uint8_t>(fragment.end_of_message ? 1 : 0)},
        std::vector<std::byte>(fragment.payload.begin(),
                               fragment.payload.end())}));
    EXPECT_TRUE(fixture.client->pump_once());
    EXPECT_EQ(fixture.client->snapshot().incomplete_control_message,
              !fragment.end_of_message);
  }
  EXPECT_TRUE(fixture.client->snapshot().profile_committed);
  EXPECT_EQ(fixture.client->snapshot().active_version, 20);
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 0);
}

TEST_F(TakionClientProfile, NegotiatesWithAScriptedCloudPeer) {
  const auto offer =
      takion::takion_negotiation_offer(takion::SessionScope::cloud_stream);
  enum class Reply { accepted, silent, unoffered, absent };
  for (const auto reply :
       {Reply::accepted, Reply::silent, Reply::unoffered, Reply::absent}) {
    takion::ClientConfig config;
    config.bootstrap_version = 23;
    config.profile_selection = takion::NegotiatedTakionProfiles{offer};
    config.negotiation_timeout_us = 1'000'000;
    ClientHarness fixture{config};
    ASSERT_NO_FATAL_FAILURE(fixture.connect(false, 23));
    auto request = decode_data(fixture.transport->sent.back(), 23);
    const auto request_sequence =
        (std::to_integer<std::uint32_t>(request.payload[0]) << 24U) |
        (std::to_integer<std::uint32_t>(request.payload[1]) << 16U) |
        (std::to_integer<std::uint32_t>(request.payload[2]) << 8U) |
        std::to_integer<std::uint32_t>(request.payload[3]);
    const takion::ControlCodec codec;
    const auto sent_request = decode_outbound_control(
        fixture.transport->sent, fixture.transport->sent.size() - 1, 1, 23);
    const auto decoded_request = codec.decode(sent_request.payload);
    ASSERT_TRUE(decoded_request);
    EXPECT_EQ(std::get<takion::ProtocolRequest>(decoded_request.value())
                  .supported_versions,
              offer);
    fixture.transport->incoming.push_back(control_packet(
        takion::DataAcknowledgementChunk{
            takion::ControlHeader{
                takion::VerificationTag{0x11223344}, {}, 0, 0},
            takion::DataAcknowledgement{request_sequence, 0x19000, {}, {}}},
        23));
    if (reply != Reply::silent) {
      const auto ack = codec.encode(
          reply == Reply::accepted    ? takion::ProtocolRequestAck{20}
          : reply == Reply::unoffered ? takion::ProtocolRequestAck{12}
                                      : takion::ProtocolRequestAck{});
      ASSERT_TRUE(ack);
      takion::ReliableChannel peer =
          takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4))
              .value();
      auto selection = peer.send(1, takion::MessageDataType{0}, ack.value(), 0);
      fixture.transport->incoming.push_back(control_packet(
          takion::DataChunk{
              takion::ControlHeader{
                  takion::VerificationTag{0x11223344}, {}, 0, 1},
              std::vector<std::byte>(selection->fragments[0].payload.begin(),
                                     selection->fragments[0].payload.end())},
          23));
    }
    EXPECT_TRUE(fixture.client->pump_once());
    if (reply == Reply::silent) {
      EXPECT_FALSE(fixture.client->snapshot().profile_committed);
      fixture.clock.now_us += 1'000'000;
      EXPECT_TRUE(fixture.client->pump_once());
      EXPECT_FALSE(fixture.client->snapshot().profile_committed);
      fixture.clock.now_us += 1;
      EXPECT_TRUE(fixture.client->pump_once());
    }
    EXPECT_TRUE(fixture.client->snapshot().profile_committed);
    const auto committed =
        fixture.events.values<takion::client_event::ProfileCommitted>();
    ASSERT_EQ(committed.size(), 1);
    EXPECT_EQ(committed.front().version,
              (reply == Reply::accepted ? 20U : 23U));
    EXPECT_EQ(fixture.client->snapshot().active_version,
              committed.front().version);
    EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  }
}

}  // namespace
}  // namespace takion_test
