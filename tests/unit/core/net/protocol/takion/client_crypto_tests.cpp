#include "tests/unit/core/net/protocol/takion/client_rig.hpp"

#include <gtest/gtest.h>

namespace takion_test {
namespace {

class TakionClientCrypto : public ClientRig {};

// Header offsets and tag handling are derived from reference takion.c's
// av_packet_parse.
TEST_F(TakionClientCrypto, DecryptsOnlyMediaAtTheReferenceKeyPosition) {
  for (const auto version : {9U, 12U, 15U, 20U}) {
    for (const bool video : {false, true}) {
      for (const bool nalu : {false, true}) {
        if (!video && nalu && version < 15) continue;
        takion::ClientConfig config;
        config.bootstrap_version = version;
        config.profile_selection = takion::FixedTakionProfile{version};
        config.envelope =
            version == 20 ? takion::PacketEnvelope{takion::PsnEnvelope{0x42}}
                          : takion::PacketEnvelope{takion::PlainEnvelope{}};
        config.crypto_policy = takion::CryptoPolicy::required;
        ClientHarness fixture{config};
        ASSERT_NO_FATAL_FAILURE(
            fixture.connect(false, version, config.envelope));
        auto local = std::make_shared<ClientCryptoState>();
        auto remote = std::make_shared<ClientCryptoState>();
        remote->decrypt_mask = std::byte{0x5a};
        ClientCryptoProvider provider{local, remote};
        EXPECT_TRUE(fixture.client->post(takion::InstallCrypto{
            &provider, takion::CryptoMaterial{crypto_secret()},
            takion::CryptoMaterial{crypto_secret()}, 0, 0}));
        EXPECT_TRUE(fixture.client->pump_once());
        const std::size_t header = (video ? 20U : 18U) +
                                   (nalu ? (video ? 3U : 1U) : 0U) +
                                   (!video && version >= 12 ? 1U : 0U);
        std::vector<std::byte> payload(header, std::byte{0});
        payload[1] = std::byte{0x11};
        payload[3] = std::byte{0x22};
        payload[8] = video ? std::byte{0x3c} : std::byte{5};
        payload[16] = std::byte{0x40};
        if (!video && version >= 12) payload[header - 1] = std::byte{2};
        const std::vector<std::byte> expected{std::byte{0}, std::byte{1},
                                              std::byte{0xab}, std::byte{0xcd}};
        for (auto byte : expected) payload.push_back(byte ^ std::byte{0x5a});
        const auto family = video ? takion::PacketFamily::video
                                  : takion::PacketFamily::audio_and_haptics;
        auto wire = packet(family, payload, version, takion::PlainEnvelope{},
                           nalu ? 0x10 : 0);
        const std::size_t mac_offset = 10 + (version == 20 ? 8 : 0);
        const auto mac = fake_mac(0x40, wire);
        std::copy(mac.begin(), mac.end(),
                  wire.begin() + static_cast<std::ptrdiff_t>(mac_offset));
        auto damaged = wire;
        damaged.back() ^= std::byte{1};
        const auto damaged_datagram =
            takion::EnvelopeCodec::encode(damaged, config.envelope);
        fixture.transport->incoming.push_back(std::vector<std::byte>(
            damaged_datagram.value().begin(), damaged_datagram.value().end()));
        EXPECT_TRUE(fixture.client->pump_once());
        EXPECT_EQ(remote->decrypt_calls, 0);
        EXPECT_EQ(fixture.events.count<takion::client_event::AvUnitReceived>(),
                  0);
        const auto intact_datagram =
            takion::EnvelopeCodec::encode(wire, config.envelope);
        fixture.transport->incoming.push_back(std::vector<std::byte>(
            intact_datagram.value().begin(), intact_datagram.value().end()));
        EXPECT_TRUE(fixture.client->pump_once());
        const auto units =
            fixture.events.values<takion::client_event::AvUnitReceived>();
        ASSERT_EQ(units.size(), 1);
        EXPECT_EQ(units[0].values.data, expected);
        EXPECT_EQ(units[0].values.packet_index, 0x11);
        EXPECT_EQ(units[0].values.frame_index, 0x22);
        EXPECT_EQ(units[0].values.is_haptics, (!video && version >= 12));
        EXPECT_EQ(remote->last_position, 0x50);
        ASSERT_EQ(remote->decrypted_input.size(), expected.size());
        EXPECT_EQ(remote->decrypt_calls, 2);
        EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(),
                  1);
      }
    }
  }
}

TEST_F(TakionClientCrypto, PostponesMediaUntilCryptoIsInstalled) {
  takion::ClientConfig config;
  config.bootstrap_version = 12;
  config.profile_selection = takion::FixedTakionProfile{12};
  config.crypto_policy = takion::CryptoPolicy::required;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect(false, 12));
  std::vector<std::byte> payload(20, std::byte{0});
  payload[1] = std::byte{0x11};
  payload[3] = std::byte{0x22};
  payload[8] = std::byte{0x3c};
  payload[16] = std::byte{0x40};
  const std::vector<std::byte> expected{std::byte{0}, std::byte{1},
                                        std::byte{0xab}, std::byte{0xcd}};
  for (auto byte : expected) payload.push_back(byte ^ std::byte{0x5a});
  auto wire = packet(takion::PacketFamily::video, payload, 12);
  const auto mac = fake_mac(0x40, wire);
  std::copy(mac.begin(), mac.end(), wire.begin() + 10);
  for (std::size_t index = 0; index < 33; ++index)
    fixture.transport->incoming.push_back(wire);
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.events.count<takion::client_event::AvUnitReceived>(), 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 1);

  auto local = std::make_shared<ClientCryptoState>();
  auto remote = std::make_shared<ClientCryptoState>();
  remote->decrypt_mask = std::byte{0x5a};
  ClientCryptoProvider provider{local, remote};
  EXPECT_TRUE(fixture.client->post(
      takion::InstallCrypto{&provider, takion::CryptoMaterial{crypto_secret()},
                            takion::CryptoMaterial{crypto_secret()}, 0, 0}));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto units =
      fixture.events.values<takion::client_event::AvUnitReceived>();
  ASSERT_EQ(units.size(), 32);
  EXPECT_EQ(units[0].values.data, expected);
  EXPECT_EQ(units[0].values.frame_index, 0x22);
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 1);
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(
      fixture.events.values<takion::client_event::AvUnitReceived>().size(), 32);
}

TEST_F(TakionClientCrypto, VerifiesPacketsBeforeReliableDispatch) {
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
  const auto install_result = fixture.client->pump_once();
  ASSERT_TRUE(install_result);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  ASSERT_EQ(fixture.transport->sent.size(), 2);

  const takion::ControlCodec codec;
  const auto heartbeat = codec.encode(takion::Heartbeat{});
  ASSERT_TRUE(heartbeat);
  takion::ReliableChannel peer =
      takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4))
          .value();
  const auto sent =
      peer.send(1, takion::MessageDataType{0}, heartbeat.value(), 0);
  ASSERT_EQ(sent->fragments.size(), 1);

  const auto plain_datagram = control_packet(takion::DataChunk{
      takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
      std::vector<std::byte>(sent->fragments[0].payload.begin(),
                             sent->fragments[0].payload.end())});
  auto unwrapped =
      takion::EnvelopeCodec::decode(plain_datagram, takion::PlainEnvelope{});
  ASSERT_TRUE(unwrapped);
  auto decoded =
      takion::PacketCodec::decode_plain_packet(unwrapped.value(), profile(7));
  ASSERT_TRUE(decoded);
  takion::CryptoContext sender{takion::CryptoPolicy::required};
  const auto install_sender =
      sender.install_local(provider, takion::CryptoMaterial{crypto_secret()});
  ASSERT_TRUE(install_sender);
  const auto local_position = sender.set_local_key_position(0);
  ASSERT_TRUE(local_position);
  datagram::OwnedDatagram authenticated(unwrapped.value().begin(),
                                        unwrapped.value().end());
  auto reservation = sender.protect_local(
      authenticated, takion::PacketFamily::control, profile(7),
      takion::PacketCryptoCoverage{0, authenticated.size(),
                                   authenticated.size(), 0},
      decoded.value().payload.size());
  ASSERT_TRUE(reservation);
  EXPECT_TRUE(reservation.value().commit());
  auto wrapped =
      takion::EnvelopeCodec::encode(authenticated, takion::PlainEnvelope{});
  ASSERT_TRUE(wrapped);

  auto corrupted = wrapped.value().clone();
  corrupted[5] = corrupted[5] ^ std::byte{1};
  fixture.transport->incoming.push_back(
      std::vector<std::byte>(corrupted.begin(), corrupted.end()));
  const auto rejected_result = fixture.client->pump_once();
  ASSERT_TRUE(rejected_result);
  const auto rejected =
      fixture.events.values<takion::client_event::PacketRejected>();
  ASSERT_EQ(rejected.size(), 1);
  EXPECT_EQ(rejected.front().error, takion::ClientError::crypto_failure);
  EXPECT_EQ(fixture.events.count<takion::client_event::ControlReceived>(), 0);
  ASSERT_EQ(fixture.transport->sent.size(), 2);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  EXPECT_EQ(fixture.transport->close_count, 0);

  fixture.transport->incoming.push_back(
      std::vector<std::byte>(wrapped.value().begin(), wrapped.value().end()));
  const auto accepted_result = fixture.client->pump_once();
  ASSERT_TRUE(accepted_result);
  const auto received =
      fixture.events.values<takion::client_event::ControlReceived>();
  ASSERT_EQ(received.size(), 1);
  EXPECT_EQ(received.front().message,
            takion::ControlMessage{takion::Heartbeat{}});
  ASSERT_EQ(fixture.transport->sent.size(), 3);
  const auto ack = decode_ack(fixture.transport->sent.back());
  EXPECT_EQ(ack.values.cumulative_sequence, 0xa1b2c3d4);
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::TerminalFailure>(), 0);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  EXPECT_EQ(fixture.transport->close_count, 0);
}

TEST_F(TakionClientCrypto,
       AuthenticatesBangAndStreamInfoAcknowledgementsAfterInstall) {
  for (const auto version : {7U, 12U}) {
    takion::ClientConfig config;
    config.bootstrap_version = version;
    config.profile_selection = takion::FixedTakionProfile{version};
    config.crypto_policy = takion::CryptoPolicy::required;
    config.allow_plain_control_until_crypto = true;
    ClientHarness fixture{config};
    ASSERT_NO_FATAL_FAILURE(fixture.connect(false, version));
    const takion::ControlCodec codec;
    takion::ReliableChannel peer =
        takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4))
            .value();
    auto routes = supported_control_routes();
    const takion::ControlMessage messages[]{std::move(routes[3]),
                                            std::move(routes[4])};
    for (const auto& message : messages) {
      auto encoded = codec.encode(message);
      ASSERT_TRUE(encoded);
      auto sent =
          peer.send(1, takion::MessageDataType{0}, encoded.value().bytes(), 0);
      ASSERT_TRUE(sent.has_value());
      ASSERT_EQ(sent->fragments.size(), 1);
      fixture.transport->incoming.push_back(control_packet(
          takion::DataChunk{
              takion::ControlHeader{
                  takion::VerificationTag{0x11223344}, {}, 0, 1},
              std::vector<std::byte>(sent->fragments[0].payload.begin(),
                                     sent->fragments[0].payload.end())},
          version));
    }
    const auto sent_before = fixture.transport->sent.size();
    EXPECT_TRUE(fixture.client->pump_once());
    EXPECT_EQ(fixture.events.count<takion::client_event::ControlReceived>(), 2);
    ASSERT_EQ(fixture.transport->sent.size(), sent_before);

    auto local = std::make_shared<ClientCryptoState>();
    auto remote = std::make_shared<ClientCryptoState>();
    ClientCryptoProvider provider{local, remote};
    EXPECT_TRUE(fixture.client->post(takion::InstallCrypto{
        &provider, takion::CryptoMaterial{crypto_secret()},
        takion::CryptoMaterial{crypto_secret()}, 0, 0}));
    EXPECT_TRUE(fixture.client->pump_once());
    ASSERT_EQ(fixture.transport->sent.size(), sent_before + 2);

    const std::uint32_t expected_positions[]{0, 42};
    for (std::size_t index = 0; index < 2; ++index) {
      const auto& wire = fixture.transport->sent[sent_before + index];
      const auto ack = decode_ack(wire, version);
      EXPECT_EQ(ack.values.cumulative_sequence, 0xa1b2c3d4 + index);
      EXPECT_EQ(ack.header.key_position, expected_positions[index]);
      auto authenticated = wire;
      std::fill_n(authenticated.begin() + 5, 8, std::byte{});
      const auto mac = fake_mac(expected_positions[index], authenticated);
      EXPECT_TRUE(std::equal(mac.begin(), mac.end(),
                             ack.header.authentication_tag.begin()));
    }

    EXPECT_TRUE(fixture.client->post(
        takion::SendControl{takion::StreamInfoAcknowledgement{}}));
    EXPECT_TRUE(fixture.client->pump_once());
    ASSERT_EQ(fixture.transport->sent.size(), sent_before + 3);
    const auto stream_info_ack =
        decode_data(fixture.transport->sent.back(), version);
    EXPECT_EQ(stream_info_ack.header.key_position, 84);
  }
}

TEST_F(TakionClientCrypto,
       EncryptsDirectMessageDataSixteenPastTheControlKeyPosition) {
  ClientHarness fixture{version20_config(takion::CryptoPolicy::required)};
  ASSERT_NO_FATAL_FAILURE(fixture.connect(false, 20));
  auto local = std::make_shared<ClientCryptoState>();
  auto remote = std::make_shared<ClientCryptoState>();
  local->encrypt_mask = std::byte{0x5a};
  remote->decrypt_mask = std::byte{0x5a};
  ClientCryptoProvider provider{local, remote};
  ASSERT_NO_FATAL_FAILURE(install_fake_crypto(fixture, provider));

  EXPECT_TRUE(fixture.client->post(
      takion::SendGenericData{typed_bytes(10, 0), "meta"}));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto announce =
      takion::TypedMessageCodec::encode_generic_data_direct_message(
          takion::GenericDataControl{
              1, 0, 10U, std::string{"meta"}, 1U, std::string{}, {}})
          .value();
  const auto tail =
      takion::TypedMessageCodec::direct_message_data_size(announce);
  EXPECT_EQ(tail, 16);
  ASSERT_FALSE(local->keystreams.empty());
  EXPECT_FALSE(local->authentications.empty());
  EXPECT_EQ(local->keystreams.back(),
            Keystream(local->authentications.back() +
                          takion::takion_crypto_block_size,
                      tail));
  const auto& sent = fixture.transport->sent.back();
  for (std::size_t index = 1; index <= tail; ++index)
    EXPECT_EQ(sent[sent.size() - index],
              (announce[announce.size() - index] ^ std::byte{0x5a}));

  takion::ReliableChannel peer =
      takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4))
          .value();
  const auto incoming =
      takion::TypedMessageCodec::encode_generic_data_direct_message(
          takion::GenericDataControl{50, 0, 20U, std::string{"in"}, 1U, {}, {}})
          .value();
  const auto incoming_tail =
      takion::TypedMessageCodec::direct_message_data_size(incoming);
  const auto fragment = peer.send(takion::control_channel_id,
                                  takion::MessageDataType{0}, incoming, 0);
  ASSERT_TRUE(fragment);
  ASSERT_EQ(fragment->fragments.size(), 1);
  auto wire = control_packet(
      takion::DataChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
          std::vector<std::byte>(fragment->fragments[0].payload.begin(),
                                 fragment->fragments[0].payload.end())},
      20);
  auto server_state = std::make_shared<ClientCryptoState>();
  server_state->encrypt_mask = std::byte{0x5a};
  ClientCryptoProvider server_provider{server_state, server_state};
  takion::CryptoContext server{takion::CryptoPolicy::required};
  EXPECT_TRUE(server.install_local(server_provider,
                                   takion::CryptoMaterial{crypto_secret()}));
  EXPECT_TRUE(server.set_local_key_position(0x40));
  auto reservation =
      server.protect_local(wire, takion::PacketFamily::control, profile(20),
                           takion::PacketCryptoCoverage{
                               0, wire.size(), wire.size() - incoming_tail,
                               incoming_tail, takion::takion_crypto_block_size},
                           wire.size());
  ASSERT_TRUE(reservation);
  EXPECT_TRUE(reservation.value().commit());
  EXPECT_EQ(server_state->keystreams.back(), Keystream(0x50, incoming_tail));

  const auto rejected =
      fixture.events.count<takion::client_event::PacketRejected>();
  const auto sent_from = fixture.transport->sent.size();
  fixture.transport->incoming.push_back(
      std::vector<std::byte>(wire.begin(), wire.end()));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(),
            rejected);
  EXPECT_EQ(remote->last_position, 0x50);
  ASSERT_EQ(remote->decrypted_input.size(), incoming_tail);
  EXPECT_GT(fixture.transport->sent.size(), sent_from);
  EXPECT_EQ(
      local->keystreams.back(),
      Keystream(
          local->authentications.back() + takion::takion_crypto_block_size,
          takion::TypedMessageCodec::direct_message_data_size(
              takion::TypedMessageCodec::encode_generic_data_direct_message(
                  takion::GenericDataControl{
                      50, 1, {}, {}, {}, std::string{}, {}})
                  .value())));
}

TEST_F(TakionClientCrypto,
       RoutesLengthPrefixedPacketsWithoutExtensionOrCrypto) {
  ClientHarness fixture{version20_config(takion::CryptoPolicy::required)};
  ASSERT_NO_FATAL_FAILURE(fixture.connect(false, 20));
  const std::vector<std::byte> payload{std::byte{0}, std::byte{1},
                                       std::byte{0}};
  EXPECT_TRUE(fixture.client->post(takion::SendLengthPrefixed{payload}));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(
      fixture.transport->sent.back(),
      (datagram::OwnedDatagram{std::byte{0x04}, std::byte{0}, std::byte{3},
                               std::byte{0}, std::byte{1}, std::byte{0}}));

  EXPECT_TRUE(fixture.client->post(
      takion::SendLengthPrefixed{std::vector<std::byte>(65536)}));
  const auto sent_before = fixture.transport->sent.size();
  EXPECT_EQ(fixture.client->pump_once().error(),
            takion::ClientError::packet_encode_failure);
  ASSERT_EQ(fixture.transport->sent.size(), sent_before);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);

  auto local = std::make_shared<ClientCryptoState>();
  auto remote = std::make_shared<ClientCryptoState>();
  ClientCryptoProvider provider{local, remote};
  ASSERT_NO_FATAL_FAILURE(install_fake_crypto(fixture, provider));
  fixture.transport->incoming.push_back(datagram::OwnedDatagram{
      std::byte{0xf4}, std::byte{0}, std::byte{2}, std::byte{0xab},
      std::byte{0xcd}, std::byte{0xee}});
  fixture.transport->incoming.push_back(datagram::OwnedDatagram{
      std::byte{0x04}, std::byte{0}, std::byte{2}, std::byte{0xab}});
  EXPECT_TRUE(fixture.client->pump_once());
  const auto received =
      fixture.events.values<takion::client_event::LengthPrefixedReceived>();
  ASSERT_EQ(received.size(), 1);
  EXPECT_EQ(received[0].payload,
            (std::vector<std::byte>{std::byte{0xab}, std::byte{0xcd}}));
  EXPECT_EQ(fixture.events.values<takion::client_event::PacketRejected>()
                .back()
                .error,
            takion::ClientError::packet_decode_failure);
}

TEST_F(TakionClientCrypto, DispatchesClientInfoWithoutPacketAuthentication) {
  ClientHarness fixture{version20_config(takion::CryptoPolicy::required)};
  ASSERT_NO_FATAL_FAILURE(fixture.connect(false, 20));
  auto local = std::make_shared<ClientCryptoState>();
  auto remote = std::make_shared<ClientCryptoState>();
  ClientCryptoProvider provider{local, remote};
  ASSERT_NO_FATAL_FAILURE(install_fake_crypto(fixture, provider));
  auto key = std::make_shared<libchiaking::SecretString>();
  key->assign("session");
  const auto body = takion::TypedMessageCodec::encode_client_info_message(
      takion::ClientInfoValues{key, 0x01020304U, 77U});
  ASSERT_TRUE(body);
  fixture.transport->incoming.push_back(
      packet(takion::PacketFamily::client_information, body.value(), 20));
  fixture.transport->incoming.push_back(
      packet(takion::PacketFamily::client_information,
             std::vector<std::byte>{std::byte{0x08}, std::byte{0x03}}, 20));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto infos =
      fixture.events.values<takion::client_event::ClientInfoReceived>();
  ASSERT_EQ(infos.size(), 1);
  EXPECT_EQ(infos[0].values.session_key->view(), "session");
  EXPECT_EQ(infos[0].values.tag, 0x01020304U);
  EXPECT_EQ(infos[0].values.key_position, 77U);
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 1);
  EXPECT_EQ(fixture.events.count<takion::client_event::ControlReceived>(), 0);
}

}  // namespace
}  // namespace takion_test
