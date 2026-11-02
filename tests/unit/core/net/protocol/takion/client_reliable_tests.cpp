#include "tests/unit/core/net/protocol/takion/client_rig.hpp"

#include <gtest/gtest.h>

namespace takion_test {
namespace {

class TakionClientReliable : public ClientRig {};

TEST_F(TakionClientReliable, AcknowledgesAndRetransmitsAHeartbeat) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const auto sent_before = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
  auto outbound = decode_data(fixture.transport->sent.back());
  const auto sequence =
      (std::to_integer<std::uint32_t>(outbound.payload[0]) << 24U) |
      (std::to_integer<std::uint32_t>(outbound.payload[1]) << 16U) |
      (std::to_integer<std::uint32_t>(outbound.payload[2]) << 8U) |
      std::to_integer<std::uint32_t>(outbound.payload[3]);

  fixture.clock.now_us = 199'999;
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
  fixture.clock.now_us = 299'999;
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
  fixture.clock.now_us = 300'000;
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 2);
  EXPECT_EQ(fixture.transport->sent.back(),
            fixture.transport->sent[sent_before]);

  fixture.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{sequence, 0x19000, {}, {}}}));

  const takion::ControlCodec codec;
  auto heartbeat = codec.encode(takion::Heartbeat{});
  ASSERT_TRUE(heartbeat);
  takion::ReliableChannel peer =
      takion::ReliableChannel::create({
                                          .initial_local_sequence = 0xa1b2c3d4,
                                          .initial_remote_sequence = 0x11223344,
                                          .maximum_message_size = 1024 * 1024,
                                          .maximum_fragment_size = 1200,
                                          .maximum_fragment_count = 1024,
                                          .retransmission_window_size = 256,
                                          .receive_reorder_size = 256,
                                          .retransmission_timeout_us = 100'000,
                                          .maximum_retries = 5,
                                      })
          .value();
  auto peer_send =
      peer.send(1, takion::MessageDataType{0}, heartbeat.value().bytes(), 0);
  ASSERT_EQ(peer_send->fragments.size(), 1);
  fixture.transport->incoming.push_back(control_packet(takion::DataChunk{
      takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
      std::vector<std::byte>(peer_send->fragments[0].payload.begin(),
                             peer_send->fragments[0].payload.end())}));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.events.count<takion::client_event::ReliableAcked>(), 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::ControlReceived>(), 1);
  auto ack_plain = takion::PacketCodec::decode_plain_packet(
      fixture.transport->sent.back(), profile(7));
  ASSERT_TRUE(ack_plain);
  auto ack =
      takion::ControlChunkCodec::decode_packet(ack_plain.value().payload);
  ASSERT_TRUE(ack);
  EXPECT_TRUE(
      std::holds_alternative<takion::DataAcknowledgementChunk>(ack.value()));
}

TEST_F(TakionClientReliable,
       CouplesAcknowledgementsRetransmissionAndDuplicates) {
  takion::ClientConfig single_config;
  single_config.limits.retransmission_timeout_us = 100;
  single_config.limits.maximum_retries = 2;
  ClientHarness single{single_config};
  ASSERT_NO_FATAL_FAILURE(single.connect());
  EXPECT_TRUE(single.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(single.client->pump_once());
  const auto original_single = single.transport->sent.back();
  const auto single_sequence =
      (std::to_integer<std::uint32_t>(decode_data(original_single).payload[0])
       << 24U) |
      (std::to_integer<std::uint32_t>(decode_data(original_single).payload[1])
       << 16U) |
      (std::to_integer<std::uint32_t>(decode_data(original_single).payload[2])
       << 8U) |
      std::to_integer<std::uint32_t>(decode_data(original_single).payload[3]);

  single.clock.now_us = 101;
  EXPECT_TRUE(single.client->pump_once());
  EXPECT_EQ(single.transport->sent.back(), original_single);
  single.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{single_sequence, 0x19000, {}, {}}}));
  EXPECT_TRUE(single.client->pump_once());
  EXPECT_EQ(single.events.count<takion::client_event::ReliableAcked>(), 1);
  EXPECT_EQ(single.events.values<takion::client_event::ReliableAcked>()
                .front()
                .sequence,
            single_sequence);
  EXPECT_EQ(single.client->snapshot().outstanding_fragments, 0);
  const auto cleared_at = single.transport->sent.size();
  single.clock.now_us = 200;
  EXPECT_TRUE(single.client->pump_once());
  ASSERT_EQ(single.transport->sent.size(), cleared_at);

  takion::ClientConfig selective_config;
  selective_config.limits.retransmission_timeout_us = 100;
  selective_config.limits.minimum_retransmission_timeout_us = 100;
  ClientHarness selective{selective_config};
  ASSERT_NO_FATAL_FAILURE(selective.connect());
  std::vector<datagram::OwnedDatagram> selective_originals;
  std::vector<std::uint32_t> selective_sequences;
  for (std::size_t index = 0; index < 3; ++index) {
    EXPECT_TRUE(
        selective.client->post(takion::SendControl{takion::Heartbeat{}}));
    EXPECT_TRUE(selective.client->pump_once());
    selective_originals.push_back(selective.transport->sent.back());
    const auto& payload = decode_data(selective_originals.back()).payload;
    const auto sequence = (std::to_integer<std::uint32_t>(payload[0]) << 24U) |
                          (std::to_integer<std::uint32_t>(payload[1]) << 16U) |
                          (std::to_integer<std::uint32_t>(payload[2]) << 8U) |
                          std::to_integer<std::uint32_t>(payload[3]);
    selective_sequences.push_back(sequence);
  }
  selective.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{selective_sequences[0] - 1,
                                      0x19000,
                                      {{2, 2}},
                                      {selective_sequences[0]}}}));
  const auto before_selective_ack = selective.transport->sent.size();
  EXPECT_TRUE(selective.client->pump_once());
  EXPECT_EQ(selective.events.count<takion::client_event::ReliableAcked>(), 1);
  EXPECT_EQ(selective.events.values<takion::client_event::ReliableAcked>()
                .front()
                .sequence,
            selective_sequences[1]);
  EXPECT_EQ(selective.client->snapshot().outstanding_fragments, 2);
  ASSERT_EQ(selective.transport->sent.size(), before_selective_ack + 1);
  EXPECT_EQ(selective.transport->sent.back(), selective_originals[0]);

  const auto selective_retransmission_start = selective.transport->sent.size();
  selective.clock.now_us = 101;
  EXPECT_TRUE(selective.client->pump_once());
  ASSERT_EQ(selective.transport->sent.size(),
            selective_retransmission_start + 2);
  EXPECT_EQ(selective.transport->sent[selective_retransmission_start],
            selective_originals[0]);
  EXPECT_EQ(selective.transport->sent[selective_retransmission_start + 1],
            selective_originals[2]);

  selective.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{
              selective_sequences[2], 0x19000, {}, {}}}));
  EXPECT_TRUE(selective.client->pump_once());
  const auto selective_ack_events =
      selective.events.values<takion::client_event::ReliableAcked>();
  ASSERT_EQ(selective_ack_events.size(), 3);
  EXPECT_EQ(selective_ack_events[0].sequence, selective_sequences[1]);
  EXPECT_EQ(selective_ack_events[1].sequence, selective_sequences[0]);
  EXPECT_EQ(selective_ack_events[2].sequence, selective_sequences[2]);
  EXPECT_EQ(selective.client->snapshot().outstanding_fragments, 0);

  takion::ClientConfig fragmented_config;
  fragmented_config.limits.maximum_fragment_size = 80;
  fragmented_config.limits.retransmission_timeout_us = 100;
  fragmented_config.limits.maximum_retries = 2;
  ClientHarness fragmented{fragmented_config};
  ASSERT_NO_FATAL_FAILURE(fragmented.connect());
  const std::vector<std::byte> big_header{std::byte{1}};
  EXPECT_TRUE(fragmented.client->post(takion::SendControl{
      takion::Big{7, takion::SharedSecretText{"session"},
                  takion::SharedSecretText{std::string(54, 'x')}, big_header,
                  big_header, big_header}}));
  const auto fragment_start = fragmented.transport->sent.size();
  EXPECT_TRUE(fragmented.client->pump_once());
  const auto fragment_count =
      fragmented.transport->sent.size() - fragment_start;
  EXPECT_GT(fragment_count, 1);
  std::vector<datagram::OwnedDatagram> originals(
      fragmented.transport->sent.begin() +
          static_cast<std::ptrdiff_t>(fragment_start),
      fragmented.transport->sent.end());
  for (std::size_t index = 0; index < fragment_count; ++index) {
    EXPECT_EQ(decode_data(originals[index]).header.verification_tag.value,
              0xa1b2c3d4);
  }

  fragmented.clock.now_us = 101;
  EXPECT_TRUE(fragmented.client->pump_once());
  ASSERT_EQ(fragmented.transport->sent.size(),
            fragment_start + 2 * fragment_count);
  for (std::size_t index = 0; index < fragment_count; ++index) {
    EXPECT_EQ(
        fragmented.transport->sent[fragment_start + fragment_count + index],
        originals[index]);
  }
  const auto last_payload = decode_data(originals.back()).payload;
  const auto last_sequence =
      (std::to_integer<std::uint32_t>(last_payload[0]) << 24U) |
      (std::to_integer<std::uint32_t>(last_payload[1]) << 16U) |
      (std::to_integer<std::uint32_t>(last_payload[2]) << 8U) |
      std::to_integer<std::uint32_t>(last_payload[3]);
  fragmented.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{last_sequence, 0x19000, {}, {}}}));
  EXPECT_TRUE(fragmented.client->pump_once());
  EXPECT_EQ(fragmented.events.count<takion::client_event::ReliableAcked>(),
            fragment_count);
  EXPECT_EQ(fragmented.client->snapshot().outstanding_fragments, 0);
  const auto fragmented_cleared_at = fragmented.transport->sent.size();
  fragmented.clock.now_us = 200;
  EXPECT_TRUE(fragmented.client->pump_once());
  ASSERT_EQ(fragmented.transport->sent.size(), fragmented_cleared_at);

  takion::ClientConfig bounded_config;
  bounded_config.limits.retransmission_timeout_us = 100;
  bounded_config.limits.maximum_retries = 1;
  ClientHarness bounded{bounded_config};
  ASSERT_NO_FATAL_FAILURE(bounded.connect());
  EXPECT_TRUE(bounded.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(bounded.client->pump_once());
  const auto bounded_initial = bounded.transport->sent.size();
  bounded.clock.now_us = 101;
  EXPECT_TRUE(bounded.client->pump_once());
  ASSERT_EQ(bounded.transport->sent.size(), bounded_initial + 1);
  bounded.clock.now_us = 202;
  EXPECT_TRUE(bounded.client->pump_once());
  ASSERT_EQ(bounded.transport->sent.size(), bounded_initial + 1);
  EXPECT_EQ(bounded.transport->close_count, 0);
  EXPECT_EQ(bounded.client->snapshot().state, takion::ClientState::connected);
  EXPECT_EQ(bounded.client->snapshot().outstanding_fragments, 0);
  EXPECT_EQ(bounded.events.count<takion::client_event::TerminalFailure>(), 0);
  EXPECT_EQ(bounded.events.count<takion::client_event::PacketRejected>(), 1);
  EXPECT_TRUE(bounded.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(bounded.client->pump_once());
  ASSERT_EQ(bounded.transport->sent.size(), bounded_initial + 2);

  takion::ClientConfig reference_limits;
  EXPECT_EQ(reference_limits.limits.retransmission_timeout_us, 300'000);
  EXPECT_EQ(reference_limits.limits.minimum_retransmission_timeout_us, 200'000);
  EXPECT_EQ(reference_limits.limits.maximum_retransmission_timeout_us,
            2'000'000);
  EXPECT_EQ(reference_limits.limits.maximum_retries, 24);

  ClientHarness duplicate;
  ASSERT_NO_FATAL_FAILURE(duplicate.connect());
  const takion::ControlCodec codec;
  auto heartbeat = codec.encode(takion::Heartbeat{});
  ASSERT_TRUE(heartbeat);
  takion::ReliableChannel peer =
      takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4))
          .value();
  auto sent =
      peer.send(1, takion::MessageDataType{0}, heartbeat.value().bytes(), 0);
  ASSERT_EQ(sent->fragments.size(), 1);
  const takion::DataChunk data{
      takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
      std::vector<std::byte>(sent->fragments[0].payload.begin(),
                             sent->fragments[0].payload.end())};
  duplicate.transport->incoming.push_back(control_packet(data));
  duplicate.transport->incoming.push_back(control_packet(data));
  EXPECT_TRUE(duplicate.client->pump_once());
  EXPECT_EQ(duplicate.events.count<takion::client_event::ControlReceived>(), 1);
  EXPECT_EQ(duplicate.events.values<takion::client_event::ControlReceived>()
                .front()
                .message,
            takion::ControlMessage{takion::Heartbeat{}});
  ASSERT_EQ(duplicate.transport->sent.size(), 4);
  EXPECT_EQ(decode_ack(duplicate.transport->sent[2]).values.cumulative_sequence,
            sent->fragments[0].sequence);
  EXPECT_EQ(decode_ack(duplicate.transport->sent[3]).values.cumulative_sequence,
            sent->fragments[0].sequence);
  EXPECT_EQ(takion::ClientConfig{}.limits.receive_reorder_size,
            takion::default_reorder_size);
  EXPECT_FALSE(duplicate.client->snapshot().incomplete_control_message);
}

TEST_F(TakionClientReliable, RetransmitsAnEncryptedShortSend) {
  takion::ClientConfig config;
  config.bootstrap_version = 7;
  config.profile_selection = takion::FixedTakionProfile{7};
  config.envelope = takion::PlainEnvelope{};
  config.crypto_policy = takion::CryptoPolicy::required;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());

  auto local_state = std::make_shared<ClientCryptoState>();
  auto remote_state = std::make_shared<ClientCryptoState>();
  ClientCryptoProvider provider{local_state, remote_state};
  EXPECT_TRUE(fixture.client->post(takion::InstallCrypto{
      &provider,
      takion::CryptoMaterial{crypto_secret()},
      takion::CryptoMaterial{crypto_secret()},
      0,
      0,
  }));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  const auto sent_before = fixture.transport->sent.size();

  fixture.transport->forced_send_size = 0;
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  EXPECT_EQ(fixture.client->snapshot().outstanding_fragments, 1);
  EXPECT_EQ(fixture.transport->close_count, 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::TerminalFailure>(), 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 1);

  fixture.transport->forced_send_size.reset();
  fixture.clock.now_us += 300'000;
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 2);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
}

TEST_F(TakionClientReliable, ResendsTheOriginalEncryptedDatagram) {
  takion::ClientConfig config;
  config.bootstrap_version = 12;
  config.profile_selection = takion::FixedTakionProfile{12};
  config.envelope = takion::PlainEnvelope{};
  config.crypto_policy = takion::CryptoPolicy::required;
  config.limits.retransmission_timeout_us = 100;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect(false, 12, config.envelope));

  auto local_state = std::make_shared<ClientCryptoState>();
  auto remote_state = std::make_shared<ClientCryptoState>();
  ClientCryptoProvider provider{local_state, remote_state};
  EXPECT_TRUE(fixture.client->post(takion::InstallCrypto{
      &provider,
      takion::CryptoMaterial{crypto_secret()},
      takion::CryptoMaterial{crypto_secret()},
      0,
      0,
  }));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto original = fixture.transport->sent.back();
  const auto sent_before = fixture.transport->sent.size();

  fixture.clock.now_us += 101;
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
  EXPECT_EQ(fixture.transport->sent.back(), original);
  fixture.clock.now_us += 101;
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 2);
  EXPECT_EQ(fixture.transport->sent.back(), original);

  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto first = decode_data(original);
  const auto position = [](const datagram::OwnedDatagram& datagram) {
    return (std::to_integer<std::uint32_t>(datagram[9]) << 24U) |
           (std::to_integer<std::uint32_t>(datagram[10]) << 16U) |
           (std::to_integer<std::uint32_t>(datagram[11]) << 8U) |
           std::to_integer<std::uint32_t>(datagram[12]);
  };
  const auto message_size = first.payload.size() - 9;
  EXPECT_EQ(position(fixture.transport->sent.back()),
            position(original) + message_size + message_size % 16);
}

TEST_F(TakionClientReliable,
       KeepsEntriesOnTransientSendErrorsAndFailsOnAClosedTransport) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const auto sent_before = fixture.transport->sent.size();
  fixture.transport->fail_send = true;
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  EXPECT_EQ(fixture.client->snapshot().outstanding_fragments, 1);
  for (int tick = 0; tick < 3; ++tick) {
    fixture.clock.now_us += 200'000;
    EXPECT_TRUE(fixture.client->pump_once());
  }
  ASSERT_EQ(fixture.transport->sent.size(), sent_before);
  EXPECT_EQ(fixture.transport->close_count, 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::TerminalFailure>(), 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 2);

  fixture.transport->fail_send = false;
  fixture.clock.now_us += 200'000;
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 2);
  EXPECT_EQ(fixture.client->snapshot().outstanding_fragments, 2);

  ClientHarness closed;
  ASSERT_NO_FATAL_FAILURE(closed.connect());
  closed.transport->fail_send = true;
  closed.transport->send_error = datagram::TransportError::closed;
  EXPECT_TRUE(closed.client->post(takion::SendControl{takion::Heartbeat{}}));
  const auto result = closed.client->pump_once();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), takion::ClientError::transport_failure);
  EXPECT_EQ(closed.client->snapshot().state, takion::ClientState::failed);
  EXPECT_EQ(closed.transport->close_count, 1);
}

TEST_F(TakionClientReliable, GatesSendsOnTheInitAcknowledgementWindow) {
  ClientHarness fixture;
  fixture.peer_receive_window = 0;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const auto sent_before = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before);
  EXPECT_EQ(fixture.client->snapshot().outstanding_fragments, 1);

  fixture.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{0x11223343, 0x19000, {}, {}}}));
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before);
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
  const auto data = decode_data(fixture.transport->sent.back());
  EXPECT_EQ(std::to_integer<std::uint8_t>(data.payload[3]), 0x44);
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 0);
}

TEST_F(TakionClientReliable,
       SendsOnceIntoAFullRetransmissionWindowAndKeepsTheSession) {
  EXPECT_EQ(takion::TakionClientLimits{}.retransmission_window_size,
            takion::default_retransmission_window);
  takion::ClientConfig config;
  config.limits.retransmission_window_size = 16;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const auto sent_before = fixture.transport->sent.size();
  for (int index = 0; index < 17; ++index)
    EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 17);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  EXPECT_EQ(fixture.client->snapshot().outstanding_fragments, 16);
  EXPECT_EQ(fixture.client->snapshot().untracked_fragments, 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::TerminalFailure>(), 0);
  const auto overflow = decode_data(fixture.transport->sent.back());
  const auto sent_after = fixture.transport->sent.size();
  fixture.clock.now_us += 300'000;
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_after + 16);
  for (auto index = sent_after; index < fixture.transport->sent.size(); ++index)
    EXPECT_NE(decode_data(fixture.transport->sent[index]).payload,
              overflow.payload);
}

TEST_F(TakionClientReliable,
       CorrelatesASenkushaOperationWithItsAcknowledgement) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());

  const takion::ControlOperationId operation{0xfeedf00d};
  EXPECT_TRUE(fixture.client->post(takion::SendControl{
      takion::Senkusha{takion::SenkushaEchoCommand{true}}, operation}));
  const auto sent_before = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->pump_once());
  auto senkusha = decode_outbound_control(fixture.transport->sent, sent_before,
                                          1, 7, takion::PlainEnvelope{}, 8);

  fixture.clock.now_us = 25;
  fixture.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{
              senkusha.sequences.front(), 0x19000, {}, {}}}));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto acknowledgements =
      fixture.events.values<takion::client_event::ReliableAcked>();
  ASSERT_EQ(acknowledgements.size(), 1);
  EXPECT_EQ(acknowledgements[0].operation_id, operation);
  ASSERT_TRUE(acknowledgements[0].operation_complete);
  EXPECT_EQ(acknowledgements[0].round_trip_time_us, 25);

  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  const auto heartbeat_before = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->pump_once());
  auto heartbeat = decode_outbound_control(
      fixture.transport->sent, heartbeat_before, 1, 7, takion::PlainEnvelope{},
      1, senkusha.sequences.front() + 1);
  fixture.clock.now_us = 50;
  fixture.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{
              heartbeat.sequences.front(), 0x19000, {}, {}}}));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto all_acknowledgements =
      fixture.events.values<takion::client_event::ReliableAcked>();
  ASSERT_EQ(all_acknowledgements.size(), 2);
  EXPECT_FALSE(all_acknowledgements.back().operation_id);
  EXPECT_FALSE(all_acknowledgements.back().operation_complete);
}

TEST_F(TakionClientReliable,
       MeasuresRoundTripsFromFirstTransmissionAcknowledgements) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  EXPECT_FALSE(fixture.client->snapshot().link.round_trip);
  EXPECT_EQ(fixture.client->snapshot().link.features, 0);
  fixture.clock.now_us = 1'000'000;
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->pump_once());
  auto outbound = decode_data(fixture.transport->sent.back());
  const auto sequence =
      (std::to_integer<std::uint32_t>(outbound.payload[0]) << 24U) |
      (std::to_integer<std::uint32_t>(outbound.payload[1]) << 16U) |
      (std::to_integer<std::uint32_t>(outbound.payload[2]) << 8U) |
      std::to_integer<std::uint32_t>(outbound.payload[3]);
  fixture.clock.now_us = 1'040'999;
  fixture.transport->incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{sequence, 0x19000, {}, {}}}));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto round_trip = fixture.client->snapshot().link.round_trip;
  ASSERT_TRUE(round_trip);
  ASSERT_EQ(round_trip->smoothed_us, 40'000);
  EXPECT_EQ(round_trip->variation_us, 20'000);
  EXPECT_EQ(
      round_trip->spread,
      (takion::RoundTripSpread{1, 40'999, 40'999, 40'999, 40'999, 40'999}));
  EXPECT_FALSE(fixture.client->snapshot().link.extension);
  EXPECT_FALSE(fixture.client->snapshot().link.video);
}

}  // namespace
}  // namespace takion_test
