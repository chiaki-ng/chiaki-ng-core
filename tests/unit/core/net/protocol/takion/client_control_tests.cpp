#include "tests/unit/core/net/protocol/takion/client_rig.hpp"

#include <gtest/gtest.h>

namespace takion_test {
namespace {

class TakionClientControl : public ClientRig {};

TEST_F(TakionClientControl, SplitsServerDataAtTheFinalFlag) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const auto sent_before = fixture.transport->sent.size();
  const auto wire = [](std::uint8_t sequence, std::uint8_t flags,
                       std::vector<std::byte> fragment) {
    std::vector<std::byte> payload{
        std::byte{0xa1}, std::byte{0xb2}, std::byte{0xc3}, std::byte{sequence},
        std::byte{0},    std::byte{1},    std::byte{0x12}, std::byte{0x34}};
    payload.insert(payload.end(), fragment.begin(), fragment.end());
    return control_packet(takion::DataChunk{
        takion::ControlHeader{
            takion::VerificationTag{0x11223344}, {}, 0, flags},
        payload});
  };
  const std::vector<std::byte> heartbeat{std::byte{0}, std::byte{0x08},
                                         std::byte{0x03}};
  fixture.transport->incoming.push_back(wire(0xd5, 2, {std::byte{0x08}}));
  fixture.transport->incoming.push_back(wire(0xd4, 0, {std::byte{0}}));
  fixture.transport->incoming.push_back(wire(0xd5, 2, {std::byte{0x08}}));
  fixture.transport->incoming.push_back(wire(0xd6, 3, {std::byte{0x03}}));
  fixture.transport->incoming.push_back(wire(0xd7, 0xff, heartbeat));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto messages =
      fixture.events.values<takion::client_event::ControlReceived>();
  ASSERT_EQ(messages.size(), 2);
  for (const auto& message : messages)
    EXPECT_TRUE(std::holds_alternative<takion::Heartbeat>(message.message));
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 0);
  EXPECT_FALSE(fixture.client->snapshot().incomplete_control_message);
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 5);
  const auto early = decode_ack(fixture.transport->sent[sent_before]).values;
  EXPECT_EQ(early.cumulative_sequence, 0xa1b2c3d3);
  EXPECT_EQ(early.gap_blocks,
            (std::vector<takion::GapAcknowledgementBlock>{{2, 2}}));
  const auto partial =
      decode_ack(fixture.transport->sent[sent_before + 1]).values;
  EXPECT_EQ(partial.cumulative_sequence, 0xa1b2c3d5);
  EXPECT_TRUE(partial.gap_blocks.empty());
  EXPECT_EQ(decode_ack(fixture.transport->sent[sent_before + 2])
                .values.cumulative_sequence,
            0xa1b2c3d5);
  EXPECT_EQ(
      decode_ack(fixture.transport->sent.back()).values.cumulative_sequence,
      0xa1b2c3d7);
}

TEST_F(TakionClientControl, FragmentsABigLikeTheReferenceWireLayout) {
  const takion::ControlCodec codec;
  for (const std::size_t length : {std::size_t{32}, std::size_t{3000}}) {
    ClientHarness fixture;
    ASSERT_NO_FATAL_FAILURE(fixture.connect());
    const std::vector<std::byte> header{std::byte{1}};
    const takion::Big message{
        7,
        takion::SharedSecretText{"synthetic-session"},
        takion::SharedSecretText{std::string(length, 'x')},
        header,
        header,
        header};
    const auto encoded = codec.encode(takion::ControlMessage{message.clone()});
    ASSERT_TRUE(encoded);
    const auto sent_before = fixture.transport->sent.size();
    EXPECT_TRUE(fixture.client->post(takion::SendControl{message.clone()}));
    EXPECT_TRUE(fixture.client->pump_once());
    const auto fragment_count = fixture.transport->sent.size() - sent_before;
    if (length == 32)
      EXPECT_EQ(fragment_count, 1);
    else
      EXPECT_GE(fragment_count, 3);

    std::vector<std::byte> reassembled;
    for (std::size_t i = 0; i < fragment_count; ++i) {
      const auto chunk = decode_data(fixture.transport->sent[sent_before + i]);
      const bool first = i == 0;
      const bool last = i + 1 == fragment_count;
      const std::size_t prefix_size = first ? 9 : 8;
      EXPECT_GT(chunk.payload.size(), prefix_size);
      EXPECT_EQ(chunk.header.flags, (last ? 1 : 0));
      EXPECT_EQ(chunk.header.verification_tag.value, 0xa1b2c3d4);
      const auto sequence =
          (std::to_integer<std::uint32_t>(chunk.payload[0]) << 24U) |
          (std::to_integer<std::uint32_t>(chunk.payload[1]) << 16U) |
          (std::to_integer<std::uint32_t>(chunk.payload[2]) << 8U) |
          std::to_integer<std::uint32_t>(chunk.payload[3]);
      EXPECT_EQ(sequence, 0x11223344U + static_cast<std::uint32_t>(i));
      EXPECT_EQ(chunk.payload[4], std::byte{0});
      EXPECT_EQ(chunk.payload[5], std::byte{1});
      EXPECT_EQ(chunk.payload[6], std::byte{0});
      EXPECT_EQ(chunk.payload[7], std::byte{0});
      if (first) {
        EXPECT_EQ(chunk.payload[8], std::byte{0});
      }
      reassembled.insert(
          reassembled.end(),
          chunk.payload.begin() + static_cast<std::ptrdiff_t>(prefix_size),
          chunk.payload.end());
    }
    EXPECT_TRUE(std::ranges::equal(reassembled, encoded.value().bytes()));
  }
}

TEST_F(TakionClientControl, SendsAnOversizedNonBigMessageAsOneDatagram) {
  takion::ClientConfig config;
  config.limits.maximum_fragment_size = 80;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const takion::ControlCodec codec;
  const takion::Disconnect message{std::string(400, 'r'), std::nullopt};
  const auto encoded = codec.encode(message);
  ASSERT_TRUE(encoded);
  EXPECT_GT(encoded.value().size(), 80);
  const auto sent_before = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->post(takion::SendControl{message}));
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
  const auto chunk = decode_data(fixture.transport->sent.back());
  EXPECT_EQ(chunk.header.flags, 1);
  ASSERT_EQ(chunk.payload.size(), 9 + encoded.value().size());
  EXPECT_TRUE(std::equal(chunk.payload.begin() + 9, chunk.payload.end(),
                         encoded.value().begin()));
  EXPECT_EQ(fixture.client->snapshot().outstanding_fragments, 1);
}

TEST_F(TakionClientControl, RejectsANonBigMessageOverOneDataChunk) {
  takion::ClientConfig config;
  config.limits.maximum_message_size = 2 * 1024 * 1024;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const auto sent_before = fixture.transport->sent.size();
  const auto outstanding_before =
      fixture.client->snapshot().outstanding_fragments;
  EXPECT_TRUE(fixture.client->post(takion::SendControl{
      takion::Disconnect{std::string(70000, 'r'), std::nullopt}}));
  const auto rejected = fixture.client->pump_once();
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), takion::ClientError::control_codec_failure);
  ASSERT_EQ(fixture.transport->sent.size(), sent_before);
  EXPECT_EQ(fixture.client->snapshot().outstanding_fragments,
            outstanding_before);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
}

TEST_F(TakionClientControl, SendsEveryRouteThroughTheReliableChannel) {
  takion::ClientConfig config;
  config.control_channel = 2;
  config.senkusha_control_channel = 9;
  config.stream_info_ack_channel = 11;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const takion::ControlCodec codec;
  auto routes = supported_control_routes();
  std::size_t reserved_sequences{};

  for (auto& route : routes) {
    const auto expected_channel =
        std::holds_alternative<takion::Senkusha>(route)
            ? config.senkusha_control_channel
        : std::holds_alternative<takion::StreamInfoAcknowledgement>(route)
            ? config.stream_info_ack_channel
        : std::holds_alternative<takion::CorruptFrame>(route)
            ? std::uint16_t{2}
            : config.control_channel;
    auto encoded = codec.encode(route);
    ASSERT_TRUE(encoded);
    const auto sent_before = fixture.transport->sent.size();
    EXPECT_TRUE(fixture.client->post(takion::SendControl{std::move(route)}));
    const auto pumped = fixture.client->pump_once();
    ASSERT_TRUE(pumped);
    const auto fragment_count = fixture.transport->sent.size() - sent_before;
    EXPECT_NE(fragment_count, 0);
    auto decoded = decode_outbound_control(
        fixture.transport->sent, sent_before, fragment_count, 7,
        takion::PlainEnvelope{}, expected_channel,
        static_cast<std::uint32_t>(0x11223344U + reserved_sequences));
    EXPECT_TRUE(std::ranges::equal(decoded.payload, encoded.value().bytes()));
    EXPECT_EQ(decoded.channel, expected_channel);
    EXPECT_EQ(decoded.data_type.known(), takion::MessageDataKind::protobuf);
    reserved_sequences += fragment_count;
  }

  const auto unsupported_before = fixture.transport->sent.size();
  const auto outstanding_before =
      fixture.client->snapshot().outstanding_fragments;
  EXPECT_TRUE(fixture.client->post(takion::SendControl{
      takion::UnsupportedControlMessage{takion::ControlPayloadType::info}}));
  const auto rejected = fixture.client->pump_once();
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), takion::ClientError::control_codec_failure);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  ASSERT_EQ(fixture.transport->sent.size(), unsupported_before);
  EXPECT_EQ(fixture.client->snapshot().outstanding_fragments,
            outstanding_before);
  EXPECT_EQ(fixture.events.count<takion::client_event::ControlReceived>(), 0);

  auto heartbeat = codec.encode(takion::Heartbeat{});
  ASSERT_TRUE(heartbeat);
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->pump_once());
  auto decoded = decode_outbound_control(
      fixture.transport->sent, unsupported_before, 1, 7,
      takion::PlainEnvelope{}, 2,
      static_cast<std::uint32_t>(0x11223344U + reserved_sequences));
  EXPECT_TRUE(std::ranges::equal(decoded.payload, heartbeat.value().bytes()));
  EXPECT_EQ(decoded.channel, 2);
  EXPECT_EQ(decoded.data_type.known(), takion::MessageDataKind::protobuf);
}

TEST_F(TakionClientControl, ReceivesEveryRouteAsAClientEvent) {
  takion::ClientConfig config;
  config.control_channel = 2;
  config.profile_selection = takion::NegotiatedTakionProfiles{{7}};
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const takion::ControlCodec codec;
  const auto routes = supported_control_routes();
  takion::ReliableChannel peer =
      takion::ReliableChannel::create(peer_channel_config(2, 0xa1b2c3d4))
          .value();

  const auto inject = [&](const takion::ControlMessage& expected,
                          std::vector<std::byte> payload) {
    auto sent = peer.send(2, takion::MessageDataType{0}, payload, 0);
    ASSERT_TRUE(sent.has_value());
    ASSERT_EQ(sent->fragments.size(), 1);
    const auto flags = static_cast<std::uint8_t>(
        (sent->fragments[0].end_of_message ? 1 : 0) |
        (sent->fragments[0].layout == takion::FragmentLayout::continuation
             ? 2
             : 0));
    fixture.transport->incoming.push_back(control_packet(takion::DataChunk{
        takion::ControlHeader{
            takion::VerificationTag{0x11223344}, {}, 0, flags},
        std::vector<std::byte>(sent->fragments[0].payload.begin(),
                               sent->fragments[0].payload.end())}));
    const auto events_before =
        fixture.events.count<takion::client_event::ControlReceived>();
    const auto sent_before = fixture.transport->sent.size();
    const auto pumped = fixture.client->pump_once();
    ASSERT_TRUE(pumped);
    EXPECT_EQ(fixture.events.count<takion::client_event::ControlReceived>(),
              events_before + 1);
    EXPECT_EQ(fixture.events.values<takion::client_event::ControlReceived>()
                  .back()
                  .message,
              expected);
    const bool internal_reply =
        std::holds_alternative<takion::ProtocolRequest>(expected);
    ASSERT_EQ(fixture.transport->sent.size(),
              sent_before + (internal_reply ? 2U : 1U));
    const auto ack = decode_ack(fixture.transport->sent[sent_before]);
    EXPECT_EQ(ack.values.cumulative_sequence, sent->fragments[0].sequence);
    EXPECT_FALSE(fixture.client->snapshot().incomplete_control_message);
    EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  };

  for (const auto& route : routes) {
    auto encoded = codec.encode(route);
    ASSERT_TRUE(encoded);
    inject(route, std::vector<std::byte>(encoded.value().begin(),
                                         encoded.value().end()));
  }

  const std::vector<std::byte> unsupported_wire{std::byte{0x08},
                                                std::byte{0x02}};
  const auto unsupported_decode = codec.decode(unsupported_wire);
  ASSERT_TRUE(unsupported_decode);
  inject(
      std::get<takion::UnsupportedControlMessage>(unsupported_decode.value()),
      unsupported_wire);
  EXPECT_EQ(fixture.events.values<takion::client_event::ControlReceived>()
                .back()
                .unsupported_payload,
            unsupported_wire);
  EXPECT_TRUE(fixture.events.values<takion::client_event::ControlReceived>()
                  .front()
                  .unsupported_payload.empty());
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 0);
  EXPECT_TRUE(fixture.client->request_stop());
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::closed);
}

TEST_F(TakionClientControl, RejectsInvalidRoutingWithoutAnEvent) {
  const takion::ControlCodec codec;
  auto heartbeat = codec.encode(takion::Heartbeat{});
  ASSERT_TRUE(heartbeat);

  const auto run_invalid = [&](datagram::OwnedDatagram bad,
                               std::optional<takion::ClientError> rejected,
                               bool invalid_advances_remote_sequence) {
    ClientHarness fixture;
    ASSERT_NO_FATAL_FAILURE(fixture.connect());
    ASSERT_EQ(fixture.transport->sent.size(), 2);
    fixture.transport->incoming.push_back(std::move(bad));
    EXPECT_TRUE(fixture.client->pump_once());
    EXPECT_EQ(fixture.events.count<takion::client_event::ControlReceived>(), 0);
    EXPECT_EQ(fixture.events.count<takion::client_event::Rumble>(), 0);
    EXPECT_EQ(fixture.events.count<takion::client_event::Haptics>(), 0);
    if (rejected) {
      const auto values =
          fixture.events.values<takion::client_event::PacketRejected>();
      ASSERT_EQ(values.size(), 1);
      EXPECT_EQ(values.front().error, *rejected);
    } else {
      EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(),
                0);
    }
    if (!invalid_advances_remote_sequence) {
      ASSERT_EQ(fixture.transport->sent.size(), 2);
    }

    takion::ReliableChannel peer =
        takion::ReliableChannel::create(
            peer_channel_config(1, invalid_advances_remote_sequence
                                       ? 0xa1b2c3d5U
                                       : 0xa1b2c3d4U))
            .value();
    auto valid =
        peer.send(1, takion::MessageDataType{0}, heartbeat.value().bytes(), 0);
    ASSERT_EQ(valid->fragments.size(), 1);
    fixture.transport->incoming.push_back(control_packet(takion::DataChunk{
        takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
        std::vector<std::byte>(valid->fragments[0].payload.begin(),
                               valid->fragments[0].payload.end())}));
    EXPECT_TRUE(fixture.client->pump_once());
    EXPECT_EQ(fixture.events.count<takion::client_event::ControlReceived>(), 1);
    const auto ack = decode_ack(fixture.transport->sent.back());
    EXPECT_EQ(ack.values.cumulative_sequence,
              (invalid_advances_remote_sequence ? 0xa1b2c3d5U : 0xa1b2c3d4U));
    EXPECT_FALSE(fixture.client->snapshot().incomplete_control_message);
  };

  takion::ReliableChannel peer =
      takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4))
          .value();
  auto sent =
      peer.send(1, takion::MessageDataType{0}, heartbeat.value().bytes(), 0);
  ASSERT_EQ(sent->fragments.size(), 1);
  std::vector<std::byte> payload(sent->fragments[0].payload.begin(),
                                 sent->fragments[0].payload.end());

  run_invalid(
      control_packet(takion::DataChunk{
          takion::ControlHeader{takion::VerificationTag{0xdeadbeef}, {}, 0, 1},
          payload}),
      takion::ClientError::invalid_state, false);

  auto truncated = payload;
  truncated.resize(7);
  run_invalid(
      control_packet(takion::DataChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
          truncated}),
      takion::ClientError::reliable_channel_failure, false);

  auto empty_message = payload;
  empty_message.resize(8);
  run_invalid(
      control_packet(takion::DataChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
          empty_message}),
      takion::ClientError::reliable_channel_failure, true);

  auto non_protobuf = payload;
  non_protobuf[8] = std::byte{7};
  run_invalid(
      control_packet(takion::DataChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
          non_protobuf}),
      takion::ClientError::packet_decode_failure, true);

  auto malformed = payload;
  malformed.resize(9);
  malformed.push_back(std::byte{0x08});
  run_invalid(
      control_packet(takion::DataChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
          malformed}),
      takion::ClientError::control_codec_failure, true);
}

TEST_F(TakionClientControl,
       LimitIsTheSmallestOfConfigProfileAndReliableBounds) {
  const std::size_t backend_limit =
      std::min(takion::default_control_message_size_limit,
               static_cast<std::size_t>(std::numeric_limits<int>::max()));

  const auto active_capacity = [&](const takion::TakionClientLimits& limits,
                                   const takion::PacketEnvelope& envelope) {
    auto result = limits.maximum_fragment_size - 1;
    if (std::holds_alternative<takion::PsnEnvelope>(envelope))
      result -= takion::psn_envelope_size;
    result -= takion::control_header_size;
    return std::min(result, static_cast<std::size_t>(
                                std::numeric_limits<std::uint16_t>::max() -
                                takion::control_length_bias));
  };
  const auto reliable_capacity = [&](const takion::TakionClientLimits& limits,
                                     std::size_t active) {
    const auto aggregate =
        (active - takion::first_fragment_overhead) +
        (limits.maximum_fragment_count - 1) *
            (active - takion::continuation_fragment_overhead);
    return std::min(limits.maximum_message_size, aggregate);
  };
  const auto expected_limit = [&](const takion::TakionClientLimits& limits,
                                  const takion::PacketEnvelope& envelope) {
    const auto active = active_capacity(limits, envelope);
    return std::min(limits.maximum_message_size,
                    std::min(reliable_capacity(limits, active), backend_limit));
  };

  const auto message_at_limit =
      [](std::size_t target) -> takion::ControlMessage {
    const bool single_datagram =
        target + takion::first_fragment_overhead <=
        std::numeric_limits<std::uint16_t>::max() - takion::control_length_bias;
    const auto first = target > 64 ? target - 64 : std::size_t{0};
    const std::vector<std::byte> header{std::byte{1}};
    for (std::size_t length = first; length <= target; ++length) {
      takion::ControlMessage message =
          single_datagram
              ? takion::ControlMessage{takion::Disconnect{
                    std::string(length, 'x'), std::nullopt}}
              : takion::ControlMessage{takion::Big{
                    7, takion::SharedSecretText{"s"},
                    takion::SharedSecretText{std::string(length, 'x')}, header,
                    header, header}};
      const takion::ControlCodec codec{target};
      auto encoded = codec.encode(message);
      if (encoded && encoded.value().size() == target) return message;
    }
    ADD_FAILURE();
    return takion::ControlMessage{takion::Heartbeat{}};
  };

  const auto exercise_bound = [&](takion::ClientConfig config,
                                  std::uint32_t version,
                                  takion::PacketEnvelope envelope,
                                  std::size_t expected) {
    EXPECT_EQ(expected_limit(config.limits, envelope), expected);
    ClientHarness fixture{config};
    fixture.peer_receive_window = std::numeric_limits<std::uint32_t>::max();
    ASSERT_NO_FATAL_FAILURE(fixture.connect(false, version, envelope));
    const takion::ControlCodec default_codec;
    auto encoded = default_codec.encode(message_at_limit(expected));
    ASSERT_TRUE(encoded);
    ASSERT_EQ(encoded.value().size(), expected);
    auto accepted = message_at_limit(expected);
    const auto sent_before = fixture.transport->sent.size();
    EXPECT_TRUE(fixture.client->post(takion::SendControl{std::move(accepted)}));
    EXPECT_TRUE(fixture.client->pump_once());
    const auto fragment_count = fixture.transport->sent.size() - sent_before;
    EXPECT_NE(fragment_count, 0);
    auto decoded = decode_outbound_control(fixture.transport->sent, sent_before,
                                           fragment_count, version, envelope,
                                           config.control_channel);
    EXPECT_TRUE(std::ranges::equal(decoded.payload, encoded.value().bytes()));
    EXPECT_EQ(decoded.channel, config.control_channel);
    EXPECT_EQ(decoded.data_type.known(), takion::MessageDataKind::protobuf);
    if (expected ==
        static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max() -
                                 takion::control_length_bias -
                                 takion::first_fragment_overhead)) {
      EXPECT_EQ(fragment_count, 1);
      const auto data =
          decode_data(fixture.transport->sent[sent_before], version, envelope);
      ASSERT_EQ(data.payload.size(),
                expected + takion::first_fragment_overhead);
      EXPECT_LE(data.payload.size(), std::numeric_limits<std::uint16_t>::max() -
                                         takion::control_length_bias);
    }

    const auto rejected_sent = fixture.transport->sent.size();
    const auto rejected_outstanding =
        fixture.client->snapshot().outstanding_fragments;
    auto oversized = takion::ControlCodec{expected + 1}.encode(
        message_at_limit(expected + 1));
    ASSERT_TRUE(oversized);
    ASSERT_EQ(oversized.value().size(), expected + 1);
    EXPECT_TRUE(fixture.client->post(
        takion::SendControl{message_at_limit(expected + 1)}));
    const auto rejected = fixture.client->pump_once();
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error(), takion::ClientError::control_codec_failure);
    EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
    ASSERT_EQ(fixture.transport->sent.size(), rejected_sent);
    EXPECT_EQ(fixture.client->snapshot().outstanding_fragments,
              rejected_outstanding);

    auto heartbeat = default_codec.encode(takion::Heartbeat{});
    ASSERT_TRUE(heartbeat);
    EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
    EXPECT_TRUE(fixture.client->pump_once());
    const auto next_fragment_count =
        fixture.transport->sent.size() - rejected_sent;
    EXPECT_NE(next_fragment_count, 0);
    auto next = decode_outbound_control(
        fixture.transport->sent, rejected_sent, next_fragment_count, version,
        envelope, config.control_channel,
        static_cast<std::uint32_t>(0x11223344U + fragment_count));
    EXPECT_TRUE(std::ranges::equal(next.payload, heartbeat.value().bytes()));
  };

  takion::ClientConfig configured;
  configured.limits.maximum_message_size = 8;
  exercise_bound(configured, 7, takion::PlainEnvelope{}, 8);

  takion::ClientConfig active;
  active.limits.maximum_message_size = 2 * 1024 * 1024;
  active.limits.maximum_fragment_size = 36;
  active.limits.maximum_fragment_count = 1;
  exercise_bound(active, 7, takion::PlainEnvelope{}, 10);

  takion::ClientConfig reliable;
  reliable.limits.maximum_message_size = 2 * 1024 * 1024;
  reliable.limits.maximum_fragment_count = 1;
  exercise_bound(reliable, 7, takion::PlainEnvelope{}, 1174);

  takion::ClientConfig backend;
  backend.limits.maximum_message_size = 2 * 1024 * 1024;
  backend.limits.maximum_fragment_size =
      takion::default_control_message_size_limit + takion::control_header_size +
      2;
  exercise_bound(backend, 7, takion::PlainEnvelope{},
                 takion::default_control_message_size_limit);

  takion::ClientConfig wire_length_clamp;
  wire_length_clamp.limits.maximum_message_size = 2 * 1024 * 1024;
  wire_length_clamp.limits.maximum_fragment_size =
      std::numeric_limits<std::uint16_t>::max() + takion::control_header_size +
      1;
  wire_length_clamp.limits.maximum_fragment_count = 1;
  exercise_bound(wire_length_clamp, 7, takion::PlainEnvelope{},
                 std::numeric_limits<std::uint16_t>::max() -
                     takion::control_length_bias -
                     takion::first_fragment_overhead);

  takion::ClientConfig final_minimum;
  final_minimum.limits.maximum_message_size = 20;
  final_minimum.limits.maximum_fragment_size = 33;
  final_minimum.limits.maximum_fragment_count = 1;
  exercise_bound(final_minimum, 7, takion::PlainEnvelope{}, 7);

  takion::ClientConfig version7;
  version7.limits.maximum_message_size = 2 * 1024 * 1024;
  version7.limits.maximum_fragment_count = 1;
  exercise_bound(version7, 7, takion::PlainEnvelope{}, 1174);

  takion::ClientConfig version20;
  version20.bootstrap_version = 20;
  version20.profile_selection = takion::FixedTakionProfile{20};
  version20.envelope = takion::PsnEnvelope{0x42};
  version20.limits.maximum_message_size = 2 * 1024 * 1024;
  version20.limits.maximum_fragment_count = 1;
  exercise_bound(version20, 20, version20.envelope, 1170);

  takion::ClientConfig zero_plain;
  zero_plain.limits.maximum_fragment_size = takion::control_header_size + 1;
  ClientHarness zero{zero_plain};
  const auto zero_result = zero.client->start();
  ASSERT_FALSE(zero_result);
  EXPECT_EQ(zero_result.error(), takion::ClientError::invalid_configuration);
  EXPECT_TRUE(zero.transport->sent.empty());

  takion::ClientConfig zero_psn;
  zero_psn.envelope = takion::PsnEnvelope{0x42};
  zero_psn.limits.maximum_fragment_size =
      takion::control_header_size + takion::psn_envelope_size + 1;
  ClientHarness enveloped_zero{zero_psn};
  const auto enveloped_zero_result = enveloped_zero.client->start();
  ASSERT_FALSE(enveloped_zero_result);
  EXPECT_EQ(enveloped_zero_result.error(),
            takion::ClientError::invalid_configuration);

  takion::ClientConfig overflow;
  overflow.limits.maximum_message_size = 1;
  overflow.limits.maximum_fragment_count =
      std::numeric_limits<std::size_t>::max();
  ClientHarness overflowing{overflow};
  const auto overflow_result = overflowing.client->start();
  ASSERT_FALSE(overflow_result);
  EXPECT_EQ(overflow_result.error(),
            takion::ClientError::invalid_configuration);
  EXPECT_TRUE(overflowing.transport->sent.empty());
}

}  // namespace
}  // namespace takion_test
