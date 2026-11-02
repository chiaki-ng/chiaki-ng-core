#include "tests/unit/core/net/protocol/takion/client_rig.hpp"

#include <gtest/gtest.h>

namespace takion_test {
namespace {

class TakionClientExtendedMessages : public ClientRig {};

TEST_F(TakionClientExtendedMessages,
       TraceIsFeatureGatedAndAuthenticatedInsideItsProtobuf) {
  ClientHarness gated;
  ASSERT_NO_FATAL_FAILURE(gated.connect());
  EXPECT_TRUE(gated.client->post(takion::SendTrace{1}));
  EXPECT_EQ(gated.client->pump_once().error(),
            takion::ClientError::packet_encode_failure);
  gated.transport->incoming.push_back(
      packet(takion::PacketFamily::trace,
             takion::TypedMessageCodec::encode_trace_message(
                 takion::TraceValues{1, 0, 0})
                 .value()));
  EXPECT_TRUE(gated.client->pump_once());
  EXPECT_EQ(gated.events.count<takion::client_event::TraceReceived>(), 0);
  EXPECT_EQ(
      gated.events.values<takion::client_event::PacketRejected>().back().error,
      takion::ClientError::packet_decode_failure);

  ClientHarness plain{version20_config()};
  ASSERT_NO_FATAL_FAILURE(plain.connect(false, 20));
  EXPECT_TRUE(plain.client->post(takion::SendTrace{5}));
  EXPECT_TRUE(plain.client->pump_once());
  const auto plain_wire = takion::PacketCodec::decode_plain_packet(
      plain.transport->sent.back(), profile(20));
  ASSERT_TRUE(plain_wire);
  EXPECT_EQ(plain_wire.value().type.raw, 0x0a);
  ASSERT_TRUE(plain_wire.value().extended_header.has_value());
  EXPECT_EQ(takion::TypedMessageCodec::decode_trace_message(
                plain_wire.value().payload)
                .value(),
            (takion::TraceValues{5, 0, 0}));

  ClientHarness fixture{version20_config(takion::CryptoPolicy::required)};
  ASSERT_NO_FATAL_FAILURE(fixture.connect(false, 20));
  EXPECT_TRUE(fixture.client->post(takion::SendTrace{9}));
  EXPECT_EQ(fixture.client->pump_once().error(),
            takion::ClientError::crypto_not_installed);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  auto local = std::make_shared<ClientCryptoState>();
  auto remote = std::make_shared<ClientCryptoState>();
  ClientCryptoProvider provider{local, remote};
  ASSERT_NO_FATAL_FAILURE(install_fake_crypto(fixture, provider, 0x100, 0x200));

  for (const std::uint32_t sequence : {9U, 10U}) {
    EXPECT_TRUE(fixture.client->post(takion::SendTrace{sequence}));
    EXPECT_TRUE(fixture.client->pump_once());
    const auto sent = takion::PacketCodec::decode_plain_packet(
        fixture.transport->sent.back(), profile(20));
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent.value().type.raw, 0x0a);
    const auto values =
        takion::TypedMessageCodec::decode_trace_message(sent.value().payload);
    ASSERT_TRUE(values);
    const std::uint32_t position = sequence == 9 ? 0x100 : 0x110;
    EXPECT_EQ(values.value().sequence, sequence);
    EXPECT_EQ(values.value().key_position, position);
    const auto aad_body = takion::TypedMessageCodec::encode_trace_message(
        takion::TraceValues{sequence, 0, 0});
    takion::PlainPacket aad{sent.value().type, sent.value().extended_header,
                            aad_body.value()};
    const auto expected_tag = fake_mac(
        position, takion::PacketCodec::encode_plain_packet(aad, profile(20))
                      .value()
                      .bytes());
    EXPECT_EQ(values.value().tag,
              takion::TypedMessageCodec::trace_tag_value(expected_tag));
  }

  const auto inbound = [](std::uint32_t sequence, std::uint32_t position,
                          std::uint32_t tag_xor) {
    const auto aad_body = takion::TypedMessageCodec::encode_trace_message(
        takion::TraceValues{sequence, 0, 0});
    takion::PlainPacket aad{takion::PacketType{0x0a},
                            takion::ExtendedPacketHeader{3, 4},
                            aad_body.value()};
    const auto aad_bytes =
        takion::PacketCodec::encode_plain_packet(aad, profile(20));
    const auto signed_body = takion::TypedMessageCodec::encode_trace_message(
        takion::TraceValues{sequence,
                            takion::TypedMessageCodec::trace_tag_value(
                                fake_mac(position, aad_bytes.value().bytes())) ^
                                tag_xor,
                            position});
    aad.payload = signed_body.value();
    const auto encoded =
        takion::PacketCodec::encode_plain_packet(aad, profile(20));
    return std::vector<std::byte>(encoded.value().begin(),
                                  encoded.value().end());
  };
  fixture.transport->incoming.push_back(inbound(31, 0x240, 1));
  fixture.transport->incoming.push_back(inbound(32, 0x250, 0));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto traces =
      fixture.events.values<takion::client_event::TraceReceived>();
  ASSERT_EQ(traces.size(), 1);
  EXPECT_EQ(traces[0].sequence, 32);
  EXPECT_EQ(fixture.events.values<takion::client_event::PacketRejected>()
                .back()
                .error,
            takion::ClientError::crypto_failure);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
}

TEST_F(TakionClientExtendedMessages, CommandSendsType14FromFeature13) {
  takion::ClientConfig config;
  config.bootstrap_version = 20;
  config.profile_selection = takion::FixedTakionProfile{20};
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect(false, 20));
  const std::vector<std::byte> message{std::byte{0x08}, std::byte{0x04},
                                       std::byte{0x32}, std::byte{0x00}};
  const auto sent_before = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->post(takion::SendExtendedMessage{message}));
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
  const auto plain = takion::PacketCodec::decode_plain_packet(
      fixture.transport->sent.back(), profile(20));
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain.value().type.family(),
            takion::PacketFamily::extended_message);
  ASSERT_TRUE(plain.value().extended_header.has_value());
  ASSERT_EQ(plain.value().payload.size(), 12 + message.size());
  const std::vector<std::byte> plain_header{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
  EXPECT_TRUE(std::ranges::equal(std::span{plain.value().payload}.first(12),
                                 plain_header));
  EXPECT_TRUE(std::equal(message.begin(), message.end(),
                         plain.value().payload.begin() + 12));

  ClientHarness legacy;
  ASSERT_NO_FATAL_FAILURE(legacy.connect());
  const auto legacy_before = legacy.transport->sent.size();
  static_cast<void>(legacy.client->post(takion::SendExtendedMessage{message}));
  static_cast<void>(legacy.client->pump_once());
  ASSERT_EQ(legacy.transport->sent.size(), legacy_before);
  EXPECT_EQ(legacy.client->snapshot().state, takion::ClientState::connected);
}

TEST_F(TakionClientExtendedMessages, VideoFrameReportsRunOnlyUnderFeature20) {
  for (const std::uint32_t version : {20U, 21U, 23U, 25U}) {
    takion::ClientConfig config;
    config.bootstrap_version = version;
    config.profile_selection = takion::FixedTakionProfile{version};
    config.crypto_policy = takion::CryptoPolicy::required;
    ClientHarness fixture{config};
    ASSERT_NO_FATAL_FAILURE(fixture.connect(false, version));
    auto local = std::make_shared<ClientCryptoState>();
    auto remote = std::make_shared<ClientCryptoState>();
    ClientCryptoProvider provider{local, remote};
    EXPECT_TRUE(fixture.client->post(takion::InstallCrypto{
        &provider, takion::CryptoMaterial{crypto_secret()},
        takion::CryptoMaterial{crypto_secret()}, 0, 0}));
    EXPECT_TRUE(fixture.client->pump_once());
    EXPECT_TRUE(fixture.client->pump_once());
    const bool reporting = version >= 23;

    fixture.clock.now_us += 624'999;
    EXPECT_TRUE(fixture.client->pump_once());
    EXPECT_TRUE(
        sent_video_frame_reports(fixture.transport->sent, version).empty());
    fixture.clock.now_us += 1;
    EXPECT_TRUE(fixture.client->pump_once());
    auto reports = sent_video_frame_reports(fixture.transport->sent, version);
    ASSERT_EQ(reports.size(), (reporting ? 1U : 0U));
    if (reporting) {
      ASSERT_EQ(reports[0].seq_num, 0);
      EXPECT_EQ(reports[0].send_int_us, 16666);
      ASSERT_EQ(reports[0].frame_no_wide, 0);
      EXPECT_EQ(reports[0].loss_ratio, 0.0);
    }

    const auto start = fixture.clock.now_us;
    fixture.transport->incoming.push_back(
        authenticated_video(version, 1, 1000, 0x40));
    EXPECT_TRUE(fixture.client->pump_once());
    fixture.clock.now_us = start + 16666;
    fixture.transport->incoming.push_back(
        authenticated_video(version, 2, 17666, 0x80));
    EXPECT_TRUE(fixture.client->pump_once());
    EXPECT_EQ(fixture.events.count<takion::client_event::AvUnitReceived>(), 2);
    EXPECT_TRUE(fixture.client->pump_once());
    reports = sent_video_frame_reports(fixture.transport->sent, version);
    const auto link = fixture.client->snapshot().link;
    EXPECT_EQ(link.features, takion::native_takion_feature_mask(version));
    ASSERT_TRUE(link.extension);
    ASSERT_EQ(link.extension->received, 2);
    EXPECT_EQ(link.extension->lost, 0);
    ASSERT_EQ(link.extension->jitter_us, 0);
    EXPECT_EQ(link.extension->loss_ratio, 0.0);
    ASSERT_TRUE(link.video);
    EXPECT_EQ(link.video->reports_present, reporting);
    ASSERT_EQ(link.video->frame_number, 2);
    EXPECT_FALSE(link.video->frame_estimated);
    ASSERT_EQ(link.video->send_interval_us, 16666);
    EXPECT_EQ(link.video->receive_interval_us, 16666);
    EXPECT_EQ(link.video->reports_sent, (reporting ? 3U : 0U));
    EXPECT_EQ(link.video->loss_ratio, 0.0);
    ASSERT_EQ(link.tos.has_value(), reporting);
    EXPECT_TRUE(!link.tos || link.tos->packets == 0);
    if (!reporting) {
      EXPECT_TRUE(reports.empty());
      continue;
    }
    ASSERT_EQ(reports.size(), 3);
    EXPECT_EQ(reports.back().seq_num, 2);
    EXPECT_EQ(reports.back().frame_no_wide, 2);
    EXPECT_EQ(reports.back().send_int_us, 16666);
    EXPECT_EQ(reports.back().recv_int_us, 16666);
    EXPECT_TRUE(reports.back().is_consecutive_frame_no);
    EXPECT_EQ(reports.back().frame_no_at_tt_updated, 2);
    EXPECT_EQ(reports.back().recv_size, 4 + 29);
    EXPECT_EQ(reports.back().proc_time_us, 0);
  }
}

TEST_F(TakionClientExtendedMessages, ReceivedTosReportsRunOnlyUnderFeature21) {
  for (const std::uint32_t version : {20U, 21U, 23U, 25U}) {
    takion::ClientConfig config;
    config.bootstrap_version = version;
    config.profile_selection = takion::FixedTakionProfile{version};
    config.crypto_policy = takion::CryptoPolicy::required;
    ClientHarness fixture{config};
    ASSERT_NO_FATAL_FAILURE(fixture.connect(false, version));
    fixture.transport->incoming_traffic_class = 0xb9;
    auto local = std::make_shared<ClientCryptoState>();
    auto remote = std::make_shared<ClientCryptoState>();
    ClientCryptoProvider provider{local, remote};
    EXPECT_TRUE(fixture.client->post(takion::InstallCrypto{
        &provider, takion::CryptoMaterial{crypto_secret()},
        takion::CryptoMaterial{crypto_secret()}, 0, 0}));
    EXPECT_TRUE(fixture.client->pump_once());
    EXPECT_TRUE(fixture.client->pump_once());
    const bool reporting = version >= 23;
    EXPECT_EQ(fixture.transport->traffic_class_enables, (reporting ? 1U : 0U));

    const auto junk = std::vector<std::byte>{std::byte{0x02}, std::byte{0}};
    fixture.transport->incoming.push_back(junk);
    fixture.transport->incoming.push_back(
        std::vector<std::byte>{std::byte{0x0f}, std::byte{0}});
    EXPECT_TRUE(fixture.client->pump_once());
    fixture.clock.now_us += 1'000'000;
    fixture.transport->incoming.push_back(junk);
    EXPECT_TRUE(fixture.client->pump_once());
    const auto reports = sent_tos_reports(fixture.transport->sent, version);
    if (!reporting) {
      EXPECT_TRUE(reports.empty());
      EXPECT_FALSE(fixture.client->snapshot().link.tos);
      continue;
    }
    ASSERT_EQ(reports.size(), 1);
    EXPECT_EQ(reports[0].time_offset_msec, 1000);
    EXPECT_EQ(reports[0].dscp_counts,
              (std::vector<std::uint32_t>{(46U << 24U) | 2U}));
    EXPECT_EQ(reports[0].ecn_ect1_count, 2);
    ASSERT_EQ(reports[0].ecn_ect0_count, 0);
    EXPECT_EQ(reports[0].ecn_ce_count, 0);
    EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
    const auto tos = fixture.client->snapshot().link.tos;
    ASSERT_TRUE(tos);
    ASSERT_TRUE(tos->last);
    EXPECT_EQ(tos->last, (takion::TrafficClass{46, 1}));
    ASSERT_EQ(tos->packets, 2);
    EXPECT_EQ(tos->reports, 1);
    ASSERT_EQ(tos->dscp_counts[46], 2);
    EXPECT_EQ(tos->ecn_counts[1], 2);
  }

  takion::ClientConfig config;
  config.bootstrap_version = 25;
  config.profile_selection = takion::FixedTakionProfile{25};
  config.crypto_policy = takion::CryptoPolicy::required;
  ClientHarness unsupported{config};
  unsupported.transport->traffic_class_supported = false;
  ASSERT_NO_FATAL_FAILURE(unsupported.connect(false, 25));
  auto local = std::make_shared<ClientCryptoState>();
  auto remote = std::make_shared<ClientCryptoState>();
  ClientCryptoProvider provider{local, remote};
  EXPECT_TRUE(unsupported.client->post(
      takion::InstallCrypto{&provider, takion::CryptoMaterial{crypto_secret()},
                            takion::CryptoMaterial{crypto_secret()}, 0, 0}));
  EXPECT_TRUE(unsupported.client->pump_once());
  EXPECT_TRUE(unsupported.client->pump_once());
  EXPECT_TRUE(unsupported.client->pump_once());
  EXPECT_EQ(unsupported.transport->traffic_class_enables, 1);
  unsupported.clock.now_us += 2'000'000;
  unsupported.transport->incoming.push_back(
      std::vector<std::byte>{std::byte{0x02}, std::byte{0}});
  EXPECT_TRUE(unsupported.client->pump_once());
  EXPECT_TRUE(sent_tos_reports(unsupported.transport->sent, 25).empty());
  const auto link = unsupported.client->snapshot().link;
  ASSERT_EQ(link.features, 0x3fffff);
  EXPECT_FALSE(link.tos);
}

}  // namespace
}  // namespace takion_test
