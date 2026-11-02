#include "tests/unit/core/net/protocol/takion/client_rig.hpp"

#include <gtest/gtest.h>

namespace takion_test {
namespace {

class TakionClientTypedPackets : public ClientRig {};

TEST_F(TakionClientTypedPackets, ProbesEnforceFamilyProfileAndDatagramBudget) {
  const std::vector<std::byte> payload{std::byte{1}, std::byte{2},
                                       std::byte{3}};

  ClientHarness plain;
  ASSERT_NO_FATAL_FAILURE(plain.connect());
  EXPECT_TRUE(plain.client->post(
      takion::SendProbe{takion::SenkushaProbeMedia::audio, payload, 4}));
  EXPECT_TRUE(plain.client->pump_once());
  ASSERT_EQ(plain.transport->sent.size(), 3);
  const auto& audio_datagram = plain.transport->sent.back();
  ASSERT_EQ(audio_datagram.size(), 4);
  auto audio_plain = takion::PacketCodec::decode_plain_packet(
      takion::EnvelopeCodec::decode(audio_datagram, takion::PlainEnvelope{})
          .value(),
      profile(7));
  ASSERT_TRUE(audio_plain);
  EXPECT_EQ(audio_plain.value().type.family(),
            takion::PacketFamily::audio_and_haptics);
  EXPECT_FALSE(audio_plain.value().extended_header.has_value());
  EXPECT_TRUE(std::ranges::equal(audio_plain.value().payload, payload));

  takion::ClientConfig version20_config;
  version20_config.bootstrap_version = 20;
  version20_config.profile_selection = takion::FixedTakionProfile{20};
  version20_config.envelope = takion::PsnEnvelope{0x42};
  ClientHarness version20{version20_config};
  ASSERT_NO_FATAL_FAILURE(
      version20.connect(false, 20, takion::PsnEnvelope{0x42}));
  EXPECT_TRUE(version20.client->post(
      takion::SendProbe{takion::SenkushaProbeMedia::video, payload, 16}));
  EXPECT_TRUE(version20.client->pump_once());
  const auto& video_datagram = version20.transport->sent.back();
  ASSERT_EQ(video_datagram.size(), 16);
  auto video_plain = takion::PacketCodec::decode_plain_packet(
      takion::EnvelopeCodec::decode(video_datagram, takion::PsnEnvelope{0x42})
          .value(),
      profile(20));
  ASSERT_TRUE(video_plain);
  EXPECT_EQ(video_plain.value().type.family(), takion::PacketFamily::video);
  ASSERT_TRUE(video_plain.value().extended_header.has_value());
  EXPECT_TRUE(std::ranges::equal(video_plain.value().payload, payload));

  const auto outstanding = plain.client->snapshot().outstanding_fragments;
  const auto sent_count = plain.transport->sent.size();
  for (const auto& request : {
           takion::SendProbe{takion::SenkushaProbeMedia::audio, payload, 5},
           takion::SendProbe{takion::SenkushaProbeMedia::audio, payload, 0},
       }) {
    EXPECT_TRUE(plain.client->post(request));
    const auto rejected = plain.client->pump_once();
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error(), takion::ClientError::packet_encode_failure);
    EXPECT_EQ(plain.client->snapshot().state, takion::ClientState::connected);
    EXPECT_EQ(plain.events.count<takion::client_event::TerminalFailure>(), 0);
    EXPECT_EQ(plain.transport->close_count, 0);
    ASSERT_EQ(plain.transport->sent.size(), sent_count);
    EXPECT_EQ(plain.client->snapshot().outstanding_fragments, outstanding);
  }

  takion::ClientConfig overflow_config;
  overflow_config.limits.maximum_fragment_size = 32;
  ClientHarness overflow{overflow_config};
  ASSERT_NO_FATAL_FAILURE(overflow.connect());
  const std::vector<std::byte> overflow_payload(32, std::byte{7});
  EXPECT_TRUE(overflow.client->post(takion::SendProbe{
      takion::SenkushaProbeMedia::video, overflow_payload, 33}));
  const auto overflow_rejected = overflow.client->pump_once();
  ASSERT_FALSE(overflow_rejected);
  EXPECT_EQ(overflow_rejected.error(),
            takion::ClientError::packet_encode_failure);
  EXPECT_EQ(overflow.client->snapshot().state, takion::ClientState::connected);
  EXPECT_EQ(overflow.events.count<takion::client_event::TerminalFailure>(), 0);
  EXPECT_EQ(overflow.transport->close_count, 0);
  ASSERT_EQ(overflow.transport->sent.size(), 2);

  EXPECT_TRUE(plain.client->post(
      takion::SendProbe{takion::SenkushaProbeMedia::audio, payload, 4}));
  EXPECT_TRUE(plain.client->pump_once());
  ASSERT_EQ(plain.transport->sent.size(), sent_count + 1);
}

TEST_F(TakionClientTypedPackets, ReportsAProbeOperationOnlyAfterItIsSent) {
  const std::vector<std::byte> payload{std::byte{0xaa}, std::byte{0xbb}};
  const auto expected_size =
      profile(7).packet_header_size(takion::PacketType{
          static_cast<std::uint8_t>(takion::PacketFamily::audio_and_haptics)}) +
      payload.size();

  ClientHarness correlated;
  ASSERT_NO_FATAL_FAILURE(correlated.connect());
  correlated.clock.now_us = 4'242;
  EXPECT_TRUE(correlated.client->post(takion::SendProbe{
      takion::SenkushaProbeMedia::audio,
      payload,
      expected_size,
      takion::ProbeOperationId{0xfeedface},
  }));
  EXPECT_TRUE(correlated.client->pump_once());
  const auto sent = correlated.events.values<takion::client_event::ProbeSent>();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0].operation_id, takion::ProbeOperationId{0xfeedface});
  EXPECT_EQ(sent[0].now_microseconds, 4'242);

  ClientHarness uncorrelated;
  ASSERT_NO_FATAL_FAILURE(uncorrelated.connect());
  EXPECT_TRUE(uncorrelated.client->post(takion::SendProbe{
      takion::SenkushaProbeMedia::audio, payload, expected_size}));
  EXPECT_TRUE(uncorrelated.client->pump_once());
  EXPECT_EQ(uncorrelated.events.count<takion::client_event::ProbeSent>(), 0);

  ClientHarness short_send;
  ASSERT_NO_FATAL_FAILURE(short_send.connect());
  short_send.transport->forced_send_size = 1;
  EXPECT_TRUE(short_send.client->post(takion::SendProbe{
      takion::SenkushaProbeMedia::audio,
      payload,
      expected_size,
      takion::ProbeOperationId{1},
  }));
  EXPECT_TRUE(short_send.client->pump_once());
  EXPECT_EQ(short_send.events.count<takion::client_event::ProbeSent>(), 1);
  EXPECT_EQ(short_send.events.count<takion::client_event::PacketRejected>(), 1);
  EXPECT_EQ(short_send.client->snapshot().state,
            takion::ClientState::connected);

  ClientHarness failed;
  ASSERT_NO_FATAL_FAILURE(failed.connect());
  failed.transport->fail_send = true;
  EXPECT_TRUE(failed.client->post(takion::SendProbe{
      takion::SenkushaProbeMedia::audio,
      payload,
      expected_size,
      takion::ProbeOperationId{2},
  }));
  EXPECT_TRUE(failed.client->pump_once());
  EXPECT_EQ(failed.events.count<takion::client_event::ProbeSent>(), 1);
  EXPECT_EQ(failed.client->snapshot().state, takion::ClientState::connected);

  ClientHarness closed;
  ASSERT_NO_FATAL_FAILURE(closed.connect());
  closed.transport->fail_send = true;
  closed.transport->send_error = datagram::TransportError::closed;
  EXPECT_TRUE(closed.client->post(takion::SendProbe{
      takion::SenkushaProbeMedia::audio,
      payload,
      expected_size,
      takion::ProbeOperationId{3},
  }));
  const auto closed_result = closed.client->pump_once();
  ASSERT_FALSE(closed_result);
  EXPECT_EQ(closed_result.error(), takion::ClientError::transport_failure);
  EXPECT_EQ(closed.events.count<takion::client_event::ProbeSent>(), 0);
}

TEST_F(TakionClientTypedPackets,
       CongestionGoesThroughProfileCryptoAndEnvelope) {
  const takion::CongestionValues values{0x1122, 0x3344, 0x5566};

  ClientHarness plain;
  ASSERT_NO_FATAL_FAILURE(plain.connect());
  EXPECT_TRUE(plain.client->post(takion::SendCongestion{values}));
  EXPECT_TRUE(plain.client->pump_once());
  const auto& plain_datagram = plain.transport->sent.back();
  EXPECT_EQ(
      plain_datagram,
      (datagram::OwnedDatagram{
          std::byte{0x05}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
          std::byte{0x44}, std::byte{0x55}, std::byte{0x66}, std::byte{0},
          std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
          std::byte{0}, std::byte{0}}));

  takion::ClientConfig version20_config;
  version20_config.bootstrap_version = 20;
  version20_config.profile_selection = takion::FixedTakionProfile{20};
  version20_config.envelope = takion::PsnEnvelope{0x42};
  ClientHarness version20{version20_config};
  ASSERT_NO_FATAL_FAILURE(
      version20.connect(false, 20, takion::PsnEnvelope{0x42}));
  EXPECT_TRUE(version20.client->post(takion::SendCongestion{values}));
  EXPECT_TRUE(version20.client->pump_once());
  const auto& datagram = version20.transport->sent.back();
  ASSERT_EQ(datagram.size(), 27);
  auto plain_packet = takion::PacketCodec::decode_plain_packet(
      takion::EnvelopeCodec::decode(datagram, takion::PsnEnvelope{0x42})
          .value(),
      profile(20));
  ASSERT_TRUE(plain_packet);
  EXPECT_EQ(plain_packet.value().type.family(),
            takion::PacketFamily::congestion);
  ASSERT_TRUE(plain_packet.value().extended_header.has_value());
  ASSERT_EQ(plain_packet.value().payload.size(), 14);
}

TEST_F(TakionClientTypedPackets,
       ExtendedHeaderCountsFromConnectAndNeverResets) {
  const takion::CongestionValues values{0, 1, 2};
  takion::ClientConfig config;
  config.bootstrap_version = 20;
  config.profile_selection = takion::FixedTakionProfile{20};
  config.envelope = takion::PsnEnvelope{0x42};
  ClientHarness fixture{config};
  fixture.clock.now_us = 7'000'000;
  ASSERT_NO_FATAL_FAILURE(
      fixture.connect(false, 20, takion::PsnEnvelope{0x42}));
  fixture.clock.now_us = 7'000'700;
  EXPECT_TRUE(fixture.client->post(takion::SendCongestion{values}));
  EXPECT_TRUE(fixture.client->post(takion::SendCongestion{values}));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto& sent = fixture.transport->sent;
  EXPECT_GE(sent.size(), 2);
  for (std::size_t index = 0; index < 2; ++index) {
    auto plain = takion::PacketCodec::decode_plain_packet(
        takion::EnvelopeCodec::decode(sent[sent.size() - 2 + index],
                                      takion::PsnEnvelope{0x42})
            .value(),
        profile(20));
    ASSERT_TRUE(plain);
    ASSERT_TRUE(plain.value().extended_header.has_value());
    EXPECT_EQ(plain.value().extended_header->timestamp, 700);
    EXPECT_EQ(plain.value().extended_header->counter, index);
  }
}

TEST_F(TakionClientTypedPackets,
       FeedbackStateGoesThroughPacketCryptoAndLifecycle) {
  const takion::FeedbackStateValues values{.controller_id = 2};
  ClientHarness plain;
  ASSERT_NO_FATAL_FAILURE(plain.connect());
  EXPECT_TRUE(plain.client->post(takion::SendFeedbackState{values, 0x1234}));
  EXPECT_TRUE(plain.client->pump_once());
  const auto& plain_datagram = plain.transport->sent.back();
  ASSERT_EQ(plain_datagram.size(), 37);
  EXPECT_EQ(plain_datagram[0], std::byte{0x06});
  EXPECT_EQ(plain_datagram[1], std::byte{0x12});
  EXPECT_EQ(plain_datagram[2], std::byte{0x34});
  EXPECT_EQ(plain_datagram[3], std::byte{0x00});
  for (std::size_t index = 4; index < 12; ++index)
    EXPECT_EQ(plain_datagram[index], std::byte{0});
  EXPECT_EQ(plain.client->snapshot().state, takion::ClientState::connected);

  takion::ClientConfig version20_config;
  version20_config.bootstrap_version = 20;
  version20_config.profile_selection = takion::FixedTakionProfile{20};
  version20_config.envelope = takion::PsnEnvelope{0x42};
  ClientHarness version20{version20_config};
  ASSERT_NO_FATAL_FAILURE(
      version20.connect(false, 20, takion::PsnEnvelope{0x42}));
  EXPECT_TRUE(
      version20.client->post(takion::SendFeedbackState{values, 0x1234}));
  EXPECT_TRUE(version20.client->pump_once());
  const auto& wrapped = version20.transport->sent.back();
  ASSERT_EQ(wrapped.size(), 52);
  auto unwrapped =
      takion::EnvelopeCodec::decode(wrapped, takion::PsnEnvelope{0x42});
  ASSERT_TRUE(unwrapped);
  EXPECT_EQ(unwrapped.value()[0], std::byte{0x06});
  for (std::size_t index = 1; index < 9; ++index)
    EXPECT_EQ(unwrapped.value()[index], std::byte{0});
  EXPECT_EQ(unwrapped.value()[9], std::byte{0x12});
  EXPECT_EQ(unwrapped.value()[10], std::byte{0x34});
  EXPECT_EQ(unwrapped.value()[11], std::byte{0x00});
  for (std::size_t index = 12; index < 20; ++index)
    EXPECT_EQ(unwrapped.value()[index], std::byte{0});

  takion::ClientConfig encrypted_config;
  encrypted_config.crypto_policy = takion::CryptoPolicy::required;
  ClientHarness encrypted{encrypted_config};
  ASSERT_NO_FATAL_FAILURE(encrypted.connect());
  auto local_state = std::make_shared<ClientCryptoState>();
  auto remote_state = std::make_shared<ClientCryptoState>();
  ClientCryptoProvider provider{local_state, remote_state};
  EXPECT_TRUE(encrypted.client->post(takion::InstallCrypto{
      &provider,
      takion::CryptoMaterial{crypto_secret()},
      takion::CryptoMaterial{crypto_secret()},
      0,
      0,
  }));
  EXPECT_TRUE(encrypted.client->pump_once());
  EXPECT_TRUE(
      encrypted.client->post(takion::SendFeedbackState{values, 0x1234}));
  EXPECT_TRUE(encrypted.client->pump_once());
  const auto& protected_datagram = encrypted.transport->sent.back();
  ASSERT_EQ(protected_datagram.size(), 37);
  auto expected_state = std::make_shared<ClientCryptoState>();
  auto expected_remote = std::make_shared<ClientCryptoState>();
  ClientCryptoProvider expected_provider{expected_state, expected_remote};
  takion::CryptoContext expected_crypto{takion::CryptoPolicy::required};
  EXPECT_TRUE(expected_crypto.install_local(
      expected_provider, takion::CryptoMaterial{crypto_secret()}));
  EXPECT_TRUE(expected_crypto.set_local_key_position(0));
  auto expected_datagram = plain_datagram;
  auto expected_reservation = expected_crypto.protect_local(
      expected_datagram, takion::PacketFamily::feedback_state, profile(7),
      takion::PacketCryptoCoverage{0, expected_datagram.size(), 12, 25}, 41);
  ASSERT_TRUE(expected_reservation);
  EXPECT_TRUE(expected_reservation.value().commit());
  EXPECT_EQ(protected_datagram, expected_datagram);
  EXPECT_EQ(local_state->last_position, 0);
  EXPECT_EQ(remote_state->last_position, 0);

  ClientHarness short_fixture{encrypted_config};
  ASSERT_NO_FATAL_FAILURE(short_fixture.connect());
  auto short_local = std::make_shared<ClientCryptoState>();
  auto short_remote = std::make_shared<ClientCryptoState>();
  ClientCryptoProvider short_provider{short_local, short_remote};
  EXPECT_TRUE(short_fixture.client->post(takion::InstallCrypto{
      &short_provider,
      takion::CryptoMaterial{crypto_secret()},
      takion::CryptoMaterial{crypto_secret()},
      50,
      0,
  }));
  EXPECT_TRUE(short_fixture.client->pump_once());
  short_fixture.transport->forced_send_size = 0;
  EXPECT_TRUE(
      short_fixture.client->post(takion::SendFeedbackState{values, 0x1234}));
  EXPECT_TRUE(short_fixture.client->pump_once());
  ASSERT_EQ(short_fixture.transport->sent.back().size(), 37);
  EXPECT_EQ(short_fixture.client->snapshot().state,
            takion::ClientState::connected);
  EXPECT_EQ(short_fixture.transport->close_count, 0);
  EXPECT_EQ(short_fixture.events.count<takion::client_event::PacketRejected>(),
            1);
  EXPECT_EQ(short_local->last_position, 50);

  ClientHarness unconnected;
  const auto invalid =
      unconnected.client->post(takion::SendFeedbackState{values, 0x1234});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), takion::ClientError::invalid_state);
  EXPECT_TRUE(unconnected.transport->sent.empty());
}

TEST_F(TakionClientTypedPackets, FeedbackHistoryCarriesTheControllerId) {
  ClientTransport transport;
  test_support::FakeClock clock{0};
  takion::CryptoContext crypto{takion::CryptoPolicy::disabled};
  const takion::ClientConfig config;
  auto profile = takion::resolve_takion_wire_profile(7);
  ASSERT_TRUE(profile);
  takion::client_detail::ClientOutbound outbound{&transport, clock, crypto,
                                                 config};
  outbound.set_profile(&profile.value());
  const takion::SendFeedbackHistory effect{
      0x1234,
      {takion::FeedbackDigitalButtonEvent{takion::FeedbackDigitalButton::cross,
                                          true}},
      2};
  EXPECT_TRUE(outbound.send_feedback_history(effect));
  ASSERT_EQ(transport.sent.size(), 1);
  EXPECT_EQ(transport.sent[0][12], std::byte{0x82});
  auto invalid = effect;
  invalid.controller_id = 4;
  const auto rejected = outbound.send_feedback_history(invalid);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), takion::ClientError::packet_encode_failure);
  ASSERT_EQ(transport.sent.size(), 1);
}

TEST_F(TakionClientTypedPackets, FeedbackHistoryCarriesKind0AndKind1Records) {
  const auto fixed = [](std::uint32_t version) {
    takion::ClientConfig config;
    config.bootstrap_version = version;
    config.profile_selection = takion::FixedTakionProfile{version};
    return config;
  };

  ClientHarness legacy{fixed(16)};
  ASSERT_NO_FATAL_FAILURE(legacy.connect(false, 16));
  EXPECT_TRUE(legacy.client->post(takion::SendFeedbackHistory{
      0x0203,
      {
          takion::FeedbackKind0Record{
              .raw_1 = 0x11, .raw_2 = 0x22, .entries = {{0x01, 0x02}}},
          takion::FeedbackKind1LegacyRecord{.flag = true,
                                            .payload = {1, 2, 3, 4, 5, 6, 7}},
      },
      3}));
  EXPECT_TRUE(legacy.client->pump_once());
  EXPECT_EQ(
      legacy.transport->sent.back(),
      (datagram::OwnedDatagram{
          std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x01},
          std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0},
          std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0},
          std::byte{0x03}, std::byte{0x11}, std::byte{0x22}, std::byte{0x01},
          std::byte{0x01}, std::byte{0x02}, std::byte{0x33}, std::byte{0x01},
          std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05},
          std::byte{0x06}, std::byte{0x07}}));

  takion::FeedbackKind1Record mode_2{.mode = 2};
  mode_2.mode_2_fields[13] = {0xb1, 0xb2, 0xb3, 0xb4};
  ClientHarness modern{fixed(17)};
  ASSERT_NO_FATAL_FAILURE(modern.connect(false, 17));
  EXPECT_TRUE(
      modern.client->post(takion::SendFeedbackHistory{0x0001, {mode_2}}));
  EXPECT_TRUE(modern.client->pump_once());
  const auto& sent = modern.transport->sent.back();
  ASSERT_EQ(sent.size(), 12 + 18);
  EXPECT_EQ(std::vector<std::byte>(sent.begin() + 12, sent.end()),
            (std::vector<std::byte>{
                std::byte{0x20}, std::byte{0}, std::byte{0}, std::byte{0x02},
                std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                std::byte{0x00}, std::byte{0x20}, std::byte{0xb1},
                std::byte{0xb2}, std::byte{0xb3}, std::byte{0xb4}}));

  ClientHarness mismatched{fixed(17)};
  ASSERT_NO_FATAL_FAILURE(mismatched.connect(false, 17));
  EXPECT_TRUE(mismatched.client->post(takion::SendFeedbackHistory{
      0x0001, {takion::FeedbackKind1LegacyRecord{}}}));
  const auto before = mismatched.transport->sent.size();
  (void)mismatched.client->pump_once();
  ASSERT_EQ(mismatched.transport->sent.size(), before);
}

TEST_F(TakionClientTypedPackets,
       FeedbackHistoryGoesThroughPacketCryptoAndLifecycle) {
  const auto history_events = [] {
    return std::vector<takion::FeedbackHistoryEvent>{
        takion::FeedbackDigitalButtonEvent{takion::FeedbackDigitalButton::cross,
                                           true},
        takion::FeedbackDigitalButtonEvent{takion::FeedbackDigitalButton::ps,
                                           false},
        takion::FeedbackTouchEvent{true, 3, 1920, 942},
    };
  };

  ClientHarness plain;
  ASSERT_NO_FATAL_FAILURE(plain.connect());
  EXPECT_TRUE(plain.client->post(
      takion::SendFeedbackHistory{0x1234, history_events()}));
  EXPECT_TRUE(plain.client->pump_once());
  const auto& plain_datagram = plain.transport->sent.back();
  EXPECT_EQ(
      plain_datagram,
      (datagram::OwnedDatagram{
          std::byte{0x01}, std::byte{0x12}, std::byte{0x34}, std::byte{0x02},
          std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0},
          std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0},
          std::byte{0x80}, std::byte{0x88}, std::byte{0xff}, std::byte{0x80},
          std::byte{0x8e}, std::byte{0xd0}, std::byte{0x03}, std::byte{0x78},
          std::byte{0x03}, std::byte{0xae}}));
  EXPECT_EQ(plain.client->snapshot().state, takion::ClientState::connected);

  ClientHarness second_controller;
  ASSERT_NO_FATAL_FAILURE(second_controller.connect());
  EXPECT_TRUE(second_controller.client->post(
      takion::SendFeedbackHistory{0x1234, history_events(), 2}));
  EXPECT_TRUE(second_controller.client->pump_once());
  const auto& controller_wire = second_controller.transport->sent.back();
  EXPECT_EQ(controller_wire[12], std::byte{0x82});
  EXPECT_EQ(controller_wire[15], std::byte{0x82});
  EXPECT_EQ(controller_wire[17], std::byte{0xd2});

  takion::ClientConfig version20_config;
  version20_config.bootstrap_version = 20;
  version20_config.profile_selection = takion::FixedTakionProfile{20};
  version20_config.envelope = takion::PsnEnvelope{0x42};
  ClientHarness version20{version20_config};
  ASSERT_NO_FATAL_FAILURE(
      version20.connect(false, 20, takion::PsnEnvelope{0x42}));
  EXPECT_TRUE(version20.client->post(takion::SendFeedbackHistory{
      0x1234,
      {
          takion::FeedbackDigitalButtonEvent{takion::FeedbackDigitalButton::ps,
                                             true},
      }}));
  EXPECT_TRUE(version20.client->pump_once());
  const auto& wrapped = version20.transport->sent.back();
  ASSERT_EQ(wrapped.size(), 27);
  auto unwrapped =
      takion::EnvelopeCodec::decode(wrapped, takion::PsnEnvelope{0x42});
  ASSERT_TRUE(unwrapped);
  EXPECT_EQ(unwrapped.value()[0], std::byte{0x01});
  for (std::size_t index = 1; index < 9; ++index)
    EXPECT_EQ(unwrapped.value()[index], std::byte{0});
  EXPECT_EQ(unwrapped.value()[9], std::byte{0x12});
  EXPECT_EQ(unwrapped.value()[10], std::byte{0x34});
  EXPECT_EQ(unwrapped.value()[11], std::byte{0x00});
  for (std::size_t index = 12; index < 20; ++index)
    EXPECT_EQ(unwrapped.value()[index], std::byte{0});
  EXPECT_EQ(unwrapped.value()[20], std::byte{0x80});
  EXPECT_EQ(unwrapped.value()[21], std::byte{0xae});
  EXPECT_EQ(unwrapped.value()[22], std::byte{0x00});

  takion::ClientConfig encrypted_config;
  encrypted_config.crypto_policy = takion::CryptoPolicy::required;
  ClientHarness encrypted{encrypted_config};
  ASSERT_NO_FATAL_FAILURE(encrypted.connect());
  auto local_state = std::make_shared<ClientCryptoState>();
  auto remote_state = std::make_shared<ClientCryptoState>();
  ClientCryptoProvider provider{local_state, remote_state};
  EXPECT_TRUE(encrypted.client->post(takion::InstallCrypto{
      &provider,
      takion::CryptoMaterial{crypto_secret()},
      takion::CryptoMaterial{crypto_secret()},
      0,
      0,
  }));
  EXPECT_TRUE(encrypted.client->pump_once());
  EXPECT_TRUE(encrypted.client->post(
      takion::SendFeedbackHistory{0x1234, history_events()}));
  EXPECT_TRUE(encrypted.client->pump_once());
  const auto& protected_datagram = encrypted.transport->sent.back();
  auto expected_state = std::make_shared<ClientCryptoState>();
  auto expected_remote = std::make_shared<ClientCryptoState>();
  ClientCryptoProvider expected_provider{expected_state, expected_remote};
  takion::CryptoContext expected_crypto{takion::CryptoPolicy::required};
  EXPECT_TRUE(expected_crypto.install_local(
      expected_provider, takion::CryptoMaterial{crypto_secret()}));
  EXPECT_TRUE(expected_crypto.set_local_key_position(0));
  auto expected_datagram = plain_datagram;
  auto expected_reservation = expected_crypto.protect_local(
      expected_datagram, takion::PacketFamily::feedback_history, profile(7),
      takion::PacketCryptoCoverage{0, expected_datagram.size(), 12, 10}, 26);
  ASSERT_TRUE(expected_reservation);
  EXPECT_TRUE(expected_reservation.value().commit());
  EXPECT_EQ(protected_datagram, expected_datagram);
  EXPECT_EQ(local_state->last_position, 0);
  EXPECT_EQ(remote_state->last_position, 0);

  ClientHarness short_fixture{encrypted_config};
  ASSERT_NO_FATAL_FAILURE(short_fixture.connect());
  auto short_local = std::make_shared<ClientCryptoState>();
  auto short_remote = std::make_shared<ClientCryptoState>();
  ClientCryptoProvider short_provider{short_local, short_remote};
  EXPECT_TRUE(short_fixture.client->post(takion::InstallCrypto{
      &short_provider,
      takion::CryptoMaterial{crypto_secret()},
      takion::CryptoMaterial{crypto_secret()},
      50,
      0,
  }));
  EXPECT_TRUE(short_fixture.client->pump_once());
  short_fixture.transport->forced_send_size = 0;
  EXPECT_TRUE(short_fixture.client->post(
      takion::SendFeedbackHistory{0x1234, history_events()}));
  EXPECT_TRUE(short_fixture.client->pump_once());
  ASSERT_EQ(short_fixture.transport->sent.back().size(), 22);
  EXPECT_EQ(short_fixture.client->snapshot().state,
            takion::ClientState::connected);
  EXPECT_EQ(short_fixture.transport->close_count, 0);
  EXPECT_EQ(short_fixture.events.count<takion::client_event::PacketRejected>(),
            1);
  EXPECT_EQ(short_local->last_position, 50);

  const auto sent_count = plain.transport->sent.size();
  EXPECT_TRUE(plain.client->post(takion::SendFeedbackHistory{
      0x1235,
      {
          takion::FeedbackTouchEvent{true, 3, 1921, 942},
      }}));
  EXPECT_TRUE(plain.client->pump_once());
  EXPECT_EQ(plain.client->snapshot().state, takion::ClientState::connected);
  ASSERT_EQ(plain.transport->sent.size(), sent_count + 1);

  ClientHarness unconnected;
  const auto invalid_state = unconnected.client->post(
      takion::SendFeedbackHistory{0x1234, history_events()});
  ASSERT_FALSE(invalid_state);
  EXPECT_EQ(invalid_state.error(), takion::ClientError::invalid_state);
  EXPECT_TRUE(unconnected.transport->sent.empty());
}

}  // namespace
}  // namespace takion_test
