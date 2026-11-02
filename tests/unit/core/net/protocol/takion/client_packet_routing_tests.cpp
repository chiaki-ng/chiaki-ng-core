#include "tests/unit/core/net/protocol/takion/client_rig.hpp"

#include <gtest/gtest.h>

namespace takion_test {
namespace {

class TakionClientPacketRouting : public ClientRig {};

TEST_F(TakionClientPacketRouting,
       OwnsVersion20MediaAndRejectsMalformedEnvelopes) {
  takion::ClientConfig config;
  config.bootstrap_version = 20;
  config.profile_selection = takion::FixedTakionProfile{20};
  config.envelope = takion::PsnEnvelope{0x42};
  ClientHarness fixture{config};

  EXPECT_TRUE(fixture.client->start());
  const takion::InitAcknowledgementValues acknowledgement{
      {takion::VerificationTag{0xa1b2c3d4}, {0x19000}, {100}, {100}, {1}},
      cookie()};
  fixture.transport->incoming.push_back(control_packet(
      takion::InitAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          acknowledgement},
      20, config.envelope));
  fixture.transport->incoming.push_back(
      control_packet(takion::CookieAcknowledgementChunk{takion::ControlHeader{
                         takion::VerificationTag{0x11223344}, {}, 0, 0}},
                     20, config.envelope));
  EXPECT_TRUE(fixture.client->pump_once());

  std::vector<std::byte> video(24, std::byte{0});
  video[8] = std::byte{0x3c};
  video[20] = std::byte{0x11};
  std::vector<std::byte> audio(23, std::byte{0});
  audio[8] = std::byte{5};
  audio[19] = std::byte{0x22};
  fixture.transport->incoming.push_back(
      packet(takion::PacketFamily::video, video, 20, config.envelope));
  fixture.transport->incoming.push_back(packet(
      takion::PacketFamily::audio_and_haptics, audio, 20, config.envelope));
  audio[18] = std::byte{0xe2};
  fixture.transport->incoming.push_back(packet(
      takion::PacketFamily::audio_and_haptics, audio, 20, config.envelope));
  fixture.transport->incoming.push_back({std::byte{0}});
  EXPECT_TRUE(fixture.client->pump_once());
  const auto units =
      fixture.events.values<takion::client_event::AvUnitReceived>();
  ASSERT_EQ(units.size(), 3);
  EXPECT_EQ(units[0].values.data.front(), std::byte{0x11});
  EXPECT_EQ(units[1].values.data.front(), std::byte{0x22});
  EXPECT_FALSE(units[1].values.is_haptics);
  ASSERT_TRUE(units[2].values.is_haptics);
  EXPECT_EQ(units[2].values.av_tag, 2);
  EXPECT_EQ(fixture.events.count<takion::client_event::EncodedVideo>(), 0);
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 1);
}

TEST_F(TakionClientPacketRouting,
       RoutesInboundFamiliesAndRejectsOutboundOnlyFeedback) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());

  const std::vector<std::byte> congestion_payload{
      std::byte{0x31}, std::byte{0x32}, std::byte{0x33}};
  fixture.transport->incoming.push_back(
      packet(takion::PacketFamily::congestion, congestion_payload));
  EXPECT_TRUE(fixture.client->pump_once());

  const auto raw =
      fixture.events.values<takion::client_event::RawFamilyPacketReceived>();
  ASSERT_EQ(raw.size(), 1);
  EXPECT_EQ(raw.front().family, takion::PacketFamily::congestion);
  EXPECT_EQ(raw.front().bytes, congestion_payload);

  const std::vector<std::byte> feedback_payload{std::byte{0x54}};
  for (const auto family : {
           takion::PacketFamily::feedback_state,
           takion::PacketFamily::feedback_history,
       }) {
    const auto rejected_before =
        fixture.events.count<takion::client_event::PacketRejected>();
    const auto raw_before =
        fixture.events.count<takion::client_event::RawFamilyPacketReceived>();
    fixture.transport->incoming.push_back(packet(family, feedback_payload));
    EXPECT_TRUE(fixture.client->pump_once());
    EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(),
              rejected_before + 1);
    EXPECT_EQ(
        fixture.events.count<takion::client_event::RawFamilyPacketReceived>(),
        raw_before);
    EXPECT_EQ(fixture.events.count<takion::client_event::ProbeReceived>(), 0);
    EXPECT_EQ(fixture.events.count<takion::client_event::EncodedAudio>(), 0);
    EXPECT_EQ(fixture.events.count<takion::client_event::EncodedVideo>(), 0);
    EXPECT_EQ(fixture.events.count<takion::client_event::Haptics>(), 0);
    EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
    EXPECT_EQ(fixture.transport->close_count, 0);
  }

  const std::vector<std::byte> video_payload{std::byte{0x56}};
  fixture.transport->incoming.push_back(
      packet(takion::PacketFamily::video, video_payload));
  EXPECT_TRUE(fixture.client->pump_once());
  const auto video =
      fixture.events.values<takion::client_event::EncodedVideo>();
  ASSERT_EQ(video.size(), 1);
  EXPECT_EQ(video.front().bytes, video_payload);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
}

TEST_F(TakionClientPacketRouting, RejectsMalformedInboundPacketsAtomically) {
  takion::ClientConfig version20_config;
  version20_config.bootstrap_version = 20;
  version20_config.profile_selection = takion::FixedTakionProfile{20};
  version20_config.envelope = takion::PsnEnvelope{0x42};
  ClientHarness version20{version20_config};
  ASSERT_NO_FATAL_FAILURE(
      version20.connect(false, 20, version20_config.envelope));
  const auto version20_snapshot = version20.client->snapshot();
  const auto version20_sent_count = version20.transport->sent.size();
  std::vector<std::byte> media(24, std::byte{0});
  media[8] = std::byte{0x3c};
  media[20] = std::byte{0xab};

  const auto reject_and_remain_connected = [&](datagram::OwnedDatagram
                                                   malformed) {
    const auto rejected_before =
        version20.events.count<takion::client_event::PacketRejected>();
    const auto video_before =
        version20.events.count<takion::client_event::AvUnitReceived>();
    const auto audio_before =
        version20.events.count<takion::client_event::EncodedAudio>();
    const auto haptics_before =
        version20.events.count<takion::client_event::Haptics>();
    const auto raw_before =
        version20.events.count<takion::client_event::RawFamilyPacketReceived>();
    const auto probe_before =
        version20.events.count<takion::client_event::ProbeReceived>();
    const auto acknowledged_before =
        version20.events.count<takion::client_event::ReliableAcked>();
    const auto closed_before =
        version20.events.count<takion::client_event::Closed>();
    const auto terminal_before =
        version20.events.count<takion::client_event::TerminalFailure>();
    version20.transport->incoming.push_back(std::move(malformed));
    EXPECT_TRUE(version20.client->pump_once());
    EXPECT_EQ(version20.events.count<takion::client_event::PacketRejected>(),
              rejected_before + 1);
    EXPECT_EQ(version20.events.count<takion::client_event::EncodedAudio>(),
              audio_before);
    EXPECT_EQ(version20.events.count<takion::client_event::AvUnitReceived>(),
              video_before);
    EXPECT_EQ(version20.events.count<takion::client_event::Haptics>(),
              haptics_before);
    EXPECT_EQ(
        version20.events.count<takion::client_event::RawFamilyPacketReceived>(),
        raw_before);
    EXPECT_EQ(version20.events.count<takion::client_event::ProbeReceived>(),
              probe_before);
    EXPECT_EQ(version20.events.count<takion::client_event::ReliableAcked>(),
              acknowledged_before);
    EXPECT_EQ(version20.events.count<takion::client_event::Closed>(),
              closed_before);
    EXPECT_EQ(version20.events.count<takion::client_event::TerminalFailure>(),
              terminal_before);
    const auto snapshot = version20.client->snapshot();
    EXPECT_EQ(snapshot.state, version20_snapshot.state);
    EXPECT_EQ(snapshot.active_version, version20_snapshot.active_version);
    EXPECT_EQ(snapshot.profile_committed, version20_snapshot.profile_committed);
    EXPECT_EQ(snapshot.incomplete_control_message,
              version20_snapshot.incomplete_control_message);
    ASSERT_EQ(version20.transport->sent.size(), version20_sent_count);
    EXPECT_EQ(version20.transport->close_count, 0);

    version20.transport->incoming.push_back(packet(
        takion::PacketFamily::video, media, 20, version20_config.envelope));
    EXPECT_TRUE(version20.client->pump_once());
    EXPECT_EQ(version20.events.count<takion::client_event::AvUnitReceived>(),
              video_before + 1);
    EXPECT_EQ(version20.client->snapshot().state,
              takion::ClientState::connected);
    ASSERT_EQ(version20.transport->sent.size(), version20_sent_count);
    EXPECT_EQ(version20.transport->close_count, 0);
  };

  const auto valid_psn =
      packet(takion::PacketFamily::video, media, 20, version20_config.envelope);
  auto plain =
      takion::EnvelopeCodec::decode(valid_psn, version20_config.envelope);
  ASSERT_TRUE(plain);
  ASSERT_EQ(plain.value().size(),
            1 + takion::extended_header_size + media.size());

  std::vector<std::byte> missing_extended(plain.value().begin(),
                                          plain.value().end());
  missing_extended.erase(
      missing_extended.begin() + 1,
      missing_extended.begin() + 1 +
          static_cast<std::ptrdiff_t>(takion::extended_header_size));
  auto missing_wrapped = takion::EnvelopeCodec::encode(
      missing_extended, version20_config.envelope);
  ASSERT_TRUE(missing_wrapped);
  reject_and_remain_connected(std::vector<std::byte>(
      missing_wrapped.value().begin(), missing_wrapped.value().end()));

  std::vector<std::byte> truncated_extended(plain.value().begin(),
                                            plain.value().end());
  truncated_extended.erase(
      truncated_extended.begin() + 5,
      truncated_extended.begin() + 5 +
          static_cast<std::ptrdiff_t>(takion::extended_header_size / 2));
  auto truncated_wrapped = takion::EnvelopeCodec::encode(
      truncated_extended, version20_config.envelope);
  ASSERT_TRUE(truncated_wrapped);
  reject_and_remain_connected(std::vector<std::byte>(
      truncated_wrapped.value().begin(), truncated_wrapped.value().end()));

  std::vector<std::byte> forbidden_family(plain.value().begin(),
                                          plain.value().end());
  forbidden_family[0] =
      (forbidden_family[0] & std::byte{0xf0}) | std::byte{0x07};
  auto forbidden_wrapped = takion::EnvelopeCodec::encode(
      forbidden_family, version20_config.envelope);
  ASSERT_TRUE(forbidden_wrapped);
  reject_and_remain_connected(std::vector<std::byte>(
      forbidden_wrapped.value().begin(), forbidden_wrapped.value().end()));

  ClientHarness version7;
  ASSERT_NO_FATAL_FAILURE(version7.connect());
  const auto version7_snapshot = version7.client->snapshot();
  const auto version7_sent_count = version7.transport->sent.size();
  version7.transport->incoming.push_back({});
  EXPECT_TRUE(version7.client->pump_once());
  EXPECT_EQ(version7.events.count<takion::client_event::PacketRejected>(), 1);
  EXPECT_EQ(version7.events.count<takion::client_event::EncodedAudio>(), 0);
  EXPECT_EQ(version7.events.count<takion::client_event::EncodedVideo>(), 0);
  EXPECT_EQ(version7.events.count<takion::client_event::Haptics>(), 0);
  EXPECT_EQ(
      version7.events.count<takion::client_event::RawFamilyPacketReceived>(),
      0);
  EXPECT_EQ(version7.events.count<takion::client_event::ProbeReceived>(), 0);
  EXPECT_EQ(version7.events.count<takion::client_event::ReliableAcked>(), 0);
  EXPECT_EQ(version7.events.count<takion::client_event::Closed>(), 0);
  EXPECT_EQ(version7.events.count<takion::client_event::TerminalFailure>(), 0);
  const auto short_snapshot = version7.client->snapshot();
  EXPECT_EQ(short_snapshot.state, version7_snapshot.state);
  EXPECT_EQ(short_snapshot.active_version, version7_snapshot.active_version);
  EXPECT_EQ(short_snapshot.profile_committed,
            version7_snapshot.profile_committed);
  EXPECT_EQ(short_snapshot.incomplete_control_message,
            version7_snapshot.incomplete_control_message);
  ASSERT_EQ(version7.transport->sent.size(), version7_sent_count);
  EXPECT_EQ(version7.transport->close_count, 0);

  const std::vector<std::byte> video_payload{std::byte{0x56}};
  version7.transport->incoming.push_back(
      packet(takion::PacketFamily::video, video_payload));
  EXPECT_TRUE(version7.client->pump_once());
  const auto video =
      version7.events.values<takion::client_event::EncodedVideo>();
  ASSERT_EQ(video.size(), 1);
  EXPECT_EQ(video.front().bytes, video_payload);
  EXPECT_EQ(version7.client->snapshot().state, takion::ClientState::connected);
  ASSERT_EQ(version7.transport->sent.size(), version7_sent_count);
  EXPECT_EQ(version7.transport->close_count, 0);
}

}  // namespace
}  // namespace takion_test
