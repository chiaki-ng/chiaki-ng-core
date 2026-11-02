#include "tests/unit/core/net/protocol/takion/client_inbound_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

namespace takion_test {
namespace {

class InboundStaleBelow final : public takion::VideoUnitGate {
 public:
  explicit InboundStaleBelow(std::uint16_t first_fresh)
      : first_fresh_(first_fresh) {}

  auto video_frame_stale(std::uint16_t frame_index) const noexcept
      -> bool override {
    return frame_index < first_fresh_;
  }

 private:
  std::uint16_t first_fresh_;
};

class TakionInboundCrypto : public ::testing::Test {
 protected:
  static auto short_video() -> std::vector<std::byte> {
    std::vector<std::byte> video(20 + 3, std::byte{0x33});
    video[4] = std::byte{0};
    video[5] = std::byte{0};
    video[6] = std::byte{0};
    video[7] = std::byte{0};
    return video;
  }

  static auto video_unit(std::uint16_t frame) -> std::vector<std::byte> {
    std::vector<std::byte> video(20 + 4, std::byte{});
    video[3] = static_cast<std::byte>(frame);
    video[8] = std::byte{0x3c};
    std::ranges::copy(plain, video.begin() + 20);
    return video;
  }

  static inline const std::vector<std::byte> plain{std::byte{1}, std::byte{2},
                                                   std::byte{3}, std::byte{4}};
};

TEST_F(TakionInboundCrypto,
       AnAvParseFailureStillCommitsTheAuthenticatedKeyPosition) {
  InboundKeyedReceiver keyed{9};
  keyed.receive(
      keyed.protect(takion::PacketFamily::video, short_video(), 5000));
  EXPECT_EQ(keyed.harness.events.single_rejection(),
            takion::ClientError::packet_decode_failure);
  EXPECT_EQ(keyed.provider.calls->remote_authentications, 1U);
  EXPECT_EQ(keyed.harness.crypto.remote_key_position(), 5000U);
}

TEST_F(TakionInboundCrypto, AForgedAvUnitLeavesTheKeyPositionAlone) {
  InboundKeyedReceiver keyed{9};
  keyed.receive(
      keyed.protect(takion::PacketFamily::video, short_video(), 5000));
  auto forged = keyed.protect(takion::PacketFamily::video, short_video(), 9000);
  forged[10] ^= std::byte{1};
  keyed.receive(std::move(forged));
  EXPECT_EQ(keyed.harness.events.last_rejection(),
            takion::ClientError::crypto_failure);
  EXPECT_EQ(keyed.harness.crypto.remote_key_position(), 5000U);
}

TEST_F(TakionInboundCrypto, DecryptsAnAuthenticatedAvPayloadAfterParsingIt) {
  InboundKeyedReceiver keyed{9};
  keyed.receive(keyed.protect(takion::PacketFamily::video, video_unit(0),
                              0x1234, plain.size()));
  ASSERT_EQ(keyed.harness.events.events.size(), 1U);
  const auto* unit =
      keyed.harness.events.first<takion::client_event::AvUnitReceived>();
  ASSERT_NE(unit, nullptr);
  EXPECT_EQ(unit->values.data, plain);
  EXPECT_EQ(keyed.harness.crypto.remote_key_position(), 0x1234U);
}

TEST_F(TakionInboundCrypto,
       AuthenticatesStaleVideoUnitsButLeavesThemEncrypted) {
  InboundKeyedReceiver keyed{9};
  const InboundStaleBelow gate{8};
  keyed.harness.config.video_gate = &gate;
  keyed.receive(keyed.protect(takion::PacketFamily::video, video_unit(7),
                              0x1234, plain.size()));
  ASSERT_EQ(keyed.harness.events.events.size(), 1U);
  const auto* stale =
      keyed.harness.events.first<takion::client_event::AvUnitReceived>();
  ASSERT_NE(stale, nullptr);
  EXPECT_TRUE(stale->values.encrypted);
  EXPECT_EQ(stale->values.frame_index, 7U);
  EXPECT_EQ(stale->values.data.size(), plain.size());
  EXPECT_NE(stale->values.data, plain);
  EXPECT_EQ(keyed.provider.calls->remote_authentications, 1U);
  EXPECT_EQ(keyed.provider.calls->remote_decryptions, 1U);
  EXPECT_EQ(keyed.harness.crypto.remote_key_position(), 0x1234U);

  keyed.receive(keyed.protect(takion::PacketFamily::video, video_unit(8),
                              0x1240, plain.size()));
  ASSERT_EQ(keyed.harness.events.events.size(), 2U);
  const auto* fresh =
      keyed.harness.events.last<takion::client_event::AvUnitReceived>();
  ASSERT_NE(fresh, nullptr);
  EXPECT_FALSE(fresh->values.encrypted);
  EXPECT_EQ(fresh->values.data, plain);
  EXPECT_EQ(keyed.provider.calls->remote_decryptions, 3U);
}

TEST_F(TakionInboundCrypto, DropsFeedbackFamiliesWithoutTouchingKeyState) {
  for (const auto family : {takion::PacketFamily::feedback_state,
                            takion::PacketFamily::feedback_history}) {
    InboundKeyedReceiver keyed{12};
    std::vector<std::byte> payload(0x20, std::byte{0x44});
    keyed.harness.crypto.set_remote_key_position(100);
    keyed.receive(keyed.protect(family, payload, 7000));
    EXPECT_EQ(keyed.harness.events.single_rejection(),
              takion::ClientError::invalid_state);
    EXPECT_EQ(keyed.provider.calls->remote_authentications, 0U);
    EXPECT_EQ(keyed.provider.calls->remote_decryptions, 0U);
    EXPECT_EQ(keyed.harness.crypto.remote_key_position(), 100U);
  }
}

class TakionInboundPlainControl : public ::testing::Test {
 protected:
  static auto bootstrap_config(bool allow_plain_control)
      -> takion::ClientConfig {
    auto config = InboundWire::keyed_config(12);
    config.allow_plain_control_until_crypto = allow_plain_control;
    return config;
  }

  static auto heartbeat() -> std::vector<std::byte> {
    return InboundWire::encoded_control(
        takion::ControlMessage{takion::Heartbeat{}});
  }

  InboundHarness harness{bootstrap_config(true)};
};

TEST_F(TakionInboundPlainControl,
       IsReceivedBeforeCryptoWhenTheBootstrapAllowsIt) {
  auto unkeyed = harness.session();
  unkeyed.crypto_installed = false;
  harness.receive(InboundWire::data_datagram(1, harness.config.control_channel,
                                             heartbeat(), 12),
                  unkeyed);
  ASSERT_EQ(harness.events.events.size(), 1U);
  const auto* control =
      harness.events.first<takion::client_event::ControlReceived>();
  ASSERT_NE(control, nullptr);
  EXPECT_TRUE(std::holds_alternative<takion::Heartbeat>(control->message));
  EXPECT_EQ(harness.host.acknowledgements.size(), 1U);
}

TEST_F(TakionInboundPlainControl,
       PostponesMediaAndRejectsOtherFamiliesBeforeCrypto) {
  auto unkeyed = harness.session();
  unkeyed.crypto_installed = false;
  const std::vector<std::byte> payload{std::byte{0x12}};
  harness.receive(InboundWire::packet(takion::PacketFamily::video, payload, 12),
                  unkeyed);
  EXPECT_TRUE(harness.events.events.empty());
  EXPECT_EQ(harness.inbound.postponed_count(), 1U);
  harness.receive(
      InboundWire::packet(takion::PacketFamily::congestion, payload, 12),
      unkeyed);
  EXPECT_EQ(harness.events.last_rejection(),
            takion::ClientError::crypto_not_installed);
}

TEST_F(TakionInboundPlainControl, IsRejectedOnceCryptoIsInstalled) {
  auto keyed = harness.session();
  harness.receive(InboundWire::data_datagram(2, harness.config.control_channel,
                                             heartbeat(), 12),
                  keyed);
  EXPECT_EQ(harness.events.last_rejection(),
            takion::ClientError::crypto_failure);

  auto forged =
      takion::ControlChunkCodec::encode_packet(takion::ControlChunkPacket{
          takion::CookieAcknowledgementChunk{takion::ControlHeader{
              takion::VerificationTag{0x11223344}, {}, 0, 0}}});
  ASSERT_TRUE(forged);
  harness.receive(InboundWire::packet(takion::PacketFamily::control,
                                      forged.value().bytes(), 12),
                  keyed);
  EXPECT_EQ(harness.events.last_rejection(),
            takion::ClientError::crypto_failure);
  EXPECT_EQ(harness.host.cookie_acknowledgements, 0U);
}

TEST_F(TakionInboundPlainControl,
       IsRejectedBeforeCryptoWithoutTheBootstrapAllowance) {
  InboundHarness strict{bootstrap_config(false)};
  auto strict_session = strict.session();
  strict_session.crypto_installed = false;
  strict.receive(InboundWire::data_datagram(1, strict.config.control_channel,
                                            heartbeat(), 12),
                 strict_session);
  EXPECT_EQ(strict.events.single_rejection(),
            takion::ClientError::crypto_not_installed);
}

}  // namespace
}  // namespace takion_test
