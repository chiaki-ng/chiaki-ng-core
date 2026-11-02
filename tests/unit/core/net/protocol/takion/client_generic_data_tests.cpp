#include "tests/unit/core/net/protocol/takion/client_rig.hpp"

#include <gtest/gtest.h>

namespace takion_test {
namespace {

class TakionClientGenericData : public ClientRig {};

TEST_F(TakionClientGenericData, TransfersOverDirectMessagesAndBulkPackets) {
  ClientHarness gated;
  ASSERT_NO_FATAL_FAILURE(gated.connect());
  EXPECT_TRUE(
      gated.client->post(takion::SendGenericData{typed_bytes(10, 0), {}}));
  EXPECT_EQ(gated.client->pump_once().error(),
            takion::ClientError::packet_encode_failure);
  EXPECT_EQ(gated.events.count<takion::client_event::GenericDataSubmitted>(),
            0);

  ClientHarness fixture{version20_config()};
  ASSERT_NO_FATAL_FAILURE(fixture.connect(false, 20));
  takion::ReliableChannel peer =
      takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4))
          .value();
  const auto data = typed_bytes(600, 3);
  auto sent_from = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->post(takion::SendGenericData{data, "meta"}));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.events.values<takion::client_event::GenericDataSubmitted>()
                .back()
                .transfer_id,
            1);
  const auto announce = sent_generic_controls(fixture, sent_from);
  ASSERT_EQ(announce.size(), 1);
  EXPECT_EQ(announce[0],
            (takion::GenericDataControl{
                1, 0, 600U, std::string{"meta"}, 2U, std::string{}, {}}));

  sent_from = fixture.transport->sent.size();
  ASSERT_NO_FATAL_FAILURE(inject_reliable(
      fixture, peer,
      takion::TypedMessageCodec::encode_generic_data_direct_message(
          takion::GenericDataControl{1, 1, {}, {}, {}, {}, {}})
          .value()));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto chunks = sent_generic_chunks(fixture, sent_from);
  ASSERT_EQ(chunks.size(), 2);
  ASSERT_EQ(chunks[0].packet_index, 0);
  EXPECT_EQ(chunks[0].transfer_offset, 0);
  ASSERT_EQ(chunks[1].packet_index, 1);
  EXPECT_EQ(chunks[1].transfer_offset, 519);
  ASSERT_EQ(chunks[0].payload.size(), 519);
  ASSERT_EQ(chunks[1].payload.size(), 81);
  ASSERT_EQ(chunks[0].key_position, 0);
  EXPECT_EQ(chunks[0].tag, takion::Mac{});
  std::vector<std::byte> reassembled(chunks[0].payload.begin(),
                                     chunks[0].payload.end());
  reassembled.insert(reassembled.end(), chunks[1].payload.begin(),
                     chunks[1].payload.end());
  EXPECT_EQ(reassembled, data);

  ASSERT_NO_FATAL_FAILURE(inject_reliable(
      fixture, peer,
      takion::TypedMessageCodec::encode_generic_data_direct_message(
          takion::GenericDataControl{1, 3, {}, {}, {}, {}, {}})
          .value()));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto ended =
      fixture.events.values<takion::client_event::GenericDataTransferEnded>();
  ASSERT_EQ(ended.size(), 1);
  EXPECT_EQ(ended[0].transfer_id, 1);
  EXPECT_EQ(ended[0].outcome, takion::GenericDataOutcome::completed);

  sent_from = fixture.transport->sent.size();
  ASSERT_NO_FATAL_FAILURE(inject_reliable(
      fixture, peer,
      takion::TypedMessageCodec::encode_generic_data_direct_message(
          takion::GenericDataControl{50, 0, 20U, std::string{"in"}, 1U, {}, {}})
          .value()));
  EXPECT_TRUE(fixture.client->pump_once());
  auto replies = sent_generic_controls(fixture, sent_from);
  ASSERT_EQ(replies.size(), 1);
  EXPECT_EQ(replies[0],
            (takion::GenericDataControl{50, 1, {}, {}, {}, std::string{}, {}}));

  const auto incoming = typed_bytes(20, 9);
  const auto chunk_packet = [](takion::GenericDataChunk chunk) {
    return packet(takion::PacketFamily::generic_data,
                  takion::TypedMessageCodec::encode_generic_data_chunk_body(
                      chunk, profile(20))
                      .value()
                      .bytes(),
                  20);
  };
  auto mismatched = chunk_packet(takion::GenericDataChunk{
      {}, 0, 50, 0, 0, libchiaking::SecretBytes{incoming}});
  mismatched.push_back(std::byte{0});
  fixture.transport->incoming.push_back(std::move(mismatched));
  fixture.transport->incoming.push_back(chunk_packet(takion::GenericDataChunk{
      {}, 0, 50, 1, 0, libchiaking::SecretBytes{incoming}}));
  sent_from = fixture.transport->sent.size();
  fixture.transport->incoming.push_back(chunk_packet(takion::GenericDataChunk{
      {}, 0, 50, 0, 0, libchiaking::SecretBytes{incoming}}));
  const auto rejected_before =
      fixture.events.count<takion::client_event::PacketRejected>();
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(),
            rejected_before + 2);
  const auto received =
      fixture.events.values<takion::client_event::GenericDataReceived>();
  ASSERT_EQ(received.size(), 1);
  ASSERT_EQ(received[0].transfer_id, 50);
  EXPECT_EQ(received[0].metadata, "in");
  EXPECT_EQ(received[0].data, incoming);
  replies = sent_generic_controls(fixture, sent_from);
  ASSERT_EQ(replies.size(), 1);
  EXPECT_EQ(replies[0],
            (takion::GenericDataControl{50, 3, {}, {}, {}, std::string{}, {}}));

  EXPECT_TRUE(
      fixture.client->post(takion::SendGenericData{typed_bytes(5, 1), {}}));
  EXPECT_TRUE(fixture.client->pump_once());
  fixture.clock.now_us += 10'000'001;
  EXPECT_TRUE(fixture.client->pump_once());
  const auto timed_out =
      fixture.events.values<takion::client_event::GenericDataTransferEnded>();
  ASSERT_EQ(timed_out.size(), 2);
  EXPECT_EQ(timed_out[1].transfer_id, 2);
  EXPECT_EQ(timed_out[1].outcome, takion::GenericDataOutcome::timed_out);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
}

TEST_F(TakionClientGenericData, PacketsKeepTheKeyPositionInAuthentication) {
  const auto config = version20_config(takion::CryptoPolicy::required);
  const auto& wire_profile = profile(20);
  {
    auto local = std::make_shared<ClientCryptoState>();
    auto remote = std::make_shared<ClientCryptoState>();
    ClientCryptoProvider provider{local, remote};
    takion::CryptoContext crypto{takion::CryptoPolicy::required};
    EXPECT_TRUE(crypto.install_local(provider,
                                     takion::CryptoMaterial{crypto_secret()}));
    EXPECT_TRUE(crypto.set_local_key_position(0x300));
    ClientTransport transport;
    test_support::FakeClock clock{0};
    transport.clock = &clock;
    takion::client_detail::ClientOutbound outbound{&transport, clock, crypto,
                                                   config};
    outbound.set_profile(&wire_profile);
    EXPECT_EQ(outbound
                  .send_generic_data_chunk(takion::GenericDataChunk{
                      {},
                      0,
                      0x44,
                      2,
                      1038,
                      libchiaking::SecretBytes{typed_bytes(7, 5)}})
                  .error(),
              takion::ClientError::crypto_not_installed);
    outbound.set_crypto_installed(true);
    EXPECT_TRUE(outbound.send_generic_data_chunk(takion::GenericDataChunk{
        {}, 0, 0x44, 2, 1038, libchiaking::SecretBytes{typed_bytes(7, 5)}}));
    auto sent = transport.sent.back();
    const auto parsed = takion::TypedMessageCodec::decode_generic_data_chunk(
        takion::PacketCodec::decode_plain_packet(sent, wire_profile).value(),
        wire_profile);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value().key_position, 0x300);
    ASSERT_EQ(parsed.value().transfer_id, 0x44);
    EXPECT_EQ(parsed.value().packet_index, 2);
    auto aad = sent;
    std::fill_n(aad.begin() + 9, 4, std::byte{});
    EXPECT_EQ(parsed.value().tag, fake_mac(0x300, aad));
    EXPECT_EQ(local->last_position, 0x300);
    EXPECT_EQ(crypto.local_key_position(), 0x300 + 23 + 7);
  }

  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect(false, 20));
  auto local = std::make_shared<ClientCryptoState>();
  auto remote = std::make_shared<ClientCryptoState>();
  remote->decrypt_mask = std::byte{0x5a};
  ClientCryptoProvider provider{local, remote};
  ASSERT_NO_FATAL_FAILURE(install_fake_crypto(fixture, provider, 0, 0x400));
  auto body = takion::TypedMessageCodec::encode_generic_data_chunk_body(
      takion::GenericDataChunk{
          {}, 0x420, 0x99, 0, 0, libchiaking::SecretBytes{typed_bytes(4, 0)}},
      wire_profile);
  auto wire =
      packet(takion::PacketFamily::generic_data, body.value().bytes(), 20);
  const auto mac = fake_mac(0x420, wire);
  std::copy(mac.begin(), mac.end(), wire.begin() + 9);
  auto tampered = wire;
  tampered[16] ^= std::byte{0x01};
  fixture.transport->incoming.push_back(tampered);
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(remote->decrypt_calls, 0);
  EXPECT_EQ(fixture.events.values<takion::client_event::PacketRejected>()
                .back()
                .error,
            takion::ClientError::crypto_failure);
  fixture.transport->incoming.push_back(wire);
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(remote->decrypt_calls, 2);
  EXPECT_EQ(remote->last_position, 0x430);
  EXPECT_EQ(remote->decrypted_input, typed_bytes(4, 0));
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 1);
}

}  // namespace
}  // namespace takion_test
