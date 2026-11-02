#include "src/core/net/protocol/takion/client/outbound.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/client_outbound_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionClientOutbound : public ::testing::Test {
 protected:
  static auto envelope(bool psn) -> takion::PacketEnvelope {
    return psn ? takion::PacketEnvelope{takion::PsnEnvelope{7}}
               : takion::PacketEnvelope{takion::PlainEnvelope{}};
  }

  static auto established() -> takion::EstablishedHandshake {
    return takion::EstablishedHandshake{takion::VerificationTag{1},
                                        takion::VerificationTag{2}};
  }

  const std::vector<std::byte> payload{std::byte{0xab}};
  const std::vector<std::byte> pong{std::byte{0x08}, std::byte{0x00},
                                    std::byte{0x12}, std::byte{0x02},
                                    std::byte{0x08}, std::byte{0x00}};
};

using TakionClientOutboundCrypto = TakionClientOutbound;
using TakionClientOutboundKeyAdvance = TakionClientOutbound;
using TakionClientOutboundExtendedMessage = TakionClientOutbound;

TEST_F(TakionClientOutbound, CountsExtendedHeadersFromTheEpoch) {
  PlainOutbound plain{20, takion::CryptoPolicy::disabled};
  plain.clock.now_us = 1000;
  plain.outbound.begin_extended_epoch(plain.clock.now_us);
  plain.clock.now_us = 2234;

  EXPECT_TRUE(plain.outbound.send_plain(takion::PacketFamily::congestion,
                                        payload, true));
  EXPECT_TRUE(plain.outbound.send_plain(takion::PacketFamily::congestion,
                                        payload, true));
  EXPECT_TRUE(plain.outbound.send_plain(takion::PacketFamily::congestion,
                                        payload, true));

  ASSERT_EQ(plain.transport.sent.size(), 3U);
  const std::array<std::uint32_t, 3> expected_counters{0, 1, 2};
  for (std::size_t index = 0; index < plain.transport.sent.size(); ++index) {
    const auto plain_bytes = takion::EnvelopeCodec::decode(
        plain.transport.sent[index], plain.config.envelope);
    ASSERT_TRUE(plain_bytes) << "index " << index;
    const auto packet = takion::PacketCodec::decode_plain_packet(
        plain_bytes.value(), plain.profile);
    ASSERT_TRUE(packet) << "index " << index;
    ASSERT_TRUE(packet.value().extended_header.has_value())
        << "index " << index;
    EXPECT_EQ(packet.value().extended_header->timestamp, 1234U)
        << "index " << index;
    EXPECT_EQ(packet.value().extended_header->counter, expected_counters[index])
        << "index " << index;
  }
}

TEST_F(TakionClientOutbound, ReportsAShortSendAfterSubmittingTheWholeWire) {
  PlainOutbound plain{20, takion::CryptoPolicy::disabled};
  plain.transport.short_send = true;
  const auto sent = plain.outbound.send_plain(takion::PacketFamily::congestion,
                                              payload, true);
  ASSERT_FALSE(sent);
  EXPECT_EQ(sent.error(), takion::ClientError::short_send);
  ASSERT_EQ(plain.transport.sent.size(), 1U);
  const auto unwrapped = takion::EnvelopeCodec::decode(
      plain.transport.sent.front(), plain.config.envelope);
  ASSERT_TRUE(unwrapped);
  const auto decoded = takion::PacketCodec::decode_plain_packet(
      unwrapped.value(), plain.profile);
  ASSERT_TRUE(decoded);
  EXPECT_TRUE(std::ranges::equal(decoded.value().payload, payload));
}

TEST_F(TakionClientOutboundCrypto,
       BlocksUnprotectedSendsButSparesTheHandshake) {
  PlainOutbound plain{20, takion::CryptoPolicy::required};
  const std::vector<std::byte> gated_payload{std::byte{0xcd}};
  const auto control_gated = plain.outbound.send_plain(
      takion::PacketFamily::control, gated_payload, false);
  ASSERT_FALSE(control_gated);
  EXPECT_EQ(control_gated.error(), takion::ClientError::crypto_not_installed);
  const auto gated = plain.outbound.send_plain(takion::PacketFamily::congestion,
                                               gated_payload, false);
  ASSERT_FALSE(gated);
  EXPECT_EQ(gated.error(), takion::ClientError::crypto_not_installed);
  EXPECT_TRUE(plain.transport.sent.empty());

  EXPECT_TRUE(plain.outbound.send_plain(takion::PacketFamily::congestion,
                                        gated_payload, true));
  EXPECT_EQ(plain.transport.sent.size(), 1U);
}

TEST_F(TakionClientOutboundCrypto,
       SendsPlainControlOnlyUntilCryptoIsInstalled) {
  PlainOutbound plain{20, takion::CryptoPolicy::required, true};
  EXPECT_TRUE(
      plain.outbound.send_plain(takion::PacketFamily::control, payload, false));
  ASSERT_EQ(plain.transport.sent.size(), 1U);
  const auto unwrapped = takion::EnvelopeCodec::decode(plain.transport.sent[0],
                                                       plain.config.envelope);
  ASSERT_TRUE(unwrapped);
  const auto decoded = takion::PacketCodec::decode_plain_packet(
      unwrapped.value(), plain.profile);
  ASSERT_TRUE(decoded);
  EXPECT_TRUE(std::ranges::equal(decoded.value().payload, payload));
  const auto denied = plain.outbound.send_plain(
      takion::PacketFamily::feedback_state, payload, false);
  ASSERT_FALSE(denied);
  EXPECT_EQ(denied.error(), takion::ClientError::crypto_not_installed);
  EXPECT_EQ(plain.transport.sent.size(), 1U);
  EXPECT_TRUE(plain.outbound.send_plain(takion::PacketFamily::congestion,
                                        payload, false));
  ASSERT_EQ(plain.transport.sent.size(), 2U);
  const auto congestion_bytes = takion::EnvelopeCodec::decode(
      plain.transport.sent[1], plain.config.envelope);
  ASSERT_TRUE(congestion_bytes);
  const auto congestion = takion::PacketCodec::decode_plain_packet(
      congestion_bytes.value(), plain.profile);
  ASSERT_TRUE(congestion);
  EXPECT_TRUE(std::ranges::equal(congestion.value().payload, payload));
  plain.transport.sent.pop_back();
  plain.outbound.set_crypto_installed(true);
  const auto protected_control =
      plain.outbound.send_plain(takion::PacketFamily::control, payload, false);
  ASSERT_FALSE(protected_control);
  EXPECT_EQ(protected_control.error(), takion::ClientError::crypto_failure);
  EXPECT_EQ(plain.transport.sent.size(), 1U);
}

TEST_F(TakionClientOutboundKeyAdvance,
       CongestionAdvancesByTheReferencePacketSize) {
  for (const std::uint32_t version : {12U, 16U, 20U}) {
    for (const bool psn : {false, true}) {
      KeyedOutbound keyed{version, envelope(psn)};
      EXPECT_TRUE(
          keyed.outbound->send_congestion({takion::CongestionValues{1, 2, 3}}))
          << "version " << version << " psn " << psn;
      EXPECT_EQ(keyed.transport.sent.size(), 1U)
          << "version " << version << " psn " << psn;
      EXPECT_EQ(keyed.advanced(), 15U + 15U % 16U)
          << "version " << version << " psn " << psn;
    }
  }
}

TEST_F(TakionClientOutboundKeyAdvance, DataAdvancesByTheMessageSizeOnly) {
  for (const bool psn : {false, true}) {
    KeyedOutbound keyed{16, envelope(psn)};
    takion::OutboundFragment fragment;
    fragment.layout = takion::FragmentLayout::first;
    fragment.end_of_message = true;
    fragment.payload.assign(std::vector<std::byte>(9 + 136, std::byte{0x42}));
    EXPECT_TRUE(keyed.outbound->send_fragment(fragment, established()))
        << "psn " << psn;
    EXPECT_EQ(keyed.advanced(), 136U + 136U % 16U) << "psn " << psn;

    takion::OutboundFragment continuation;
    continuation.layout = takion::FragmentLayout::continuation;
    continuation.end_of_message = true;
    continuation.payload.assign(
        std::vector<std::byte>(8 + 519, std::byte{0x43}));
    EXPECT_TRUE(keyed.outbound->send_fragment(continuation, established()))
        << "psn " << psn;
    EXPECT_EQ(keyed.advanced(), 144U + 519U + 519U % 16U) << "psn " << psn;
    EXPECT_EQ(keyed.transport.sent.size(), 2U) << "psn " << psn;
  }
}

TEST_F(TakionClientOutboundKeyAdvance,
       DataAcknowledgementAdvancesByTheWireSizeIncludingTheEnvelope) {
  KeyedOutbound plain{16, takion::PlainEnvelope{}};
  EXPECT_TRUE(plain.outbound->send_ack(
      takion::DataAcknowledgement{0x1234, 0x19000, {}, {}}, established()));
  ASSERT_EQ(plain.transport.sent.size(), 1U);
  EXPECT_EQ(plain.transport.sent[0].size(), 29U);
  EXPECT_EQ(plain.advanced(), 29U + 29U % 16U);

  KeyedOutbound wrapped{16, takion::PsnEnvelope{7}};
  EXPECT_TRUE(wrapped.outbound->send_ack(
      takion::DataAcknowledgement{0x1234, 0x19000, {}, {}}, established()));
  ASSERT_EQ(wrapped.transport.sent.size(), 1U);
  EXPECT_EQ(wrapped.transport.sent[0].size(), 33U);
  EXPECT_EQ(wrapped.advanced(), 33U + 33U % 16U);
}

TEST_F(TakionClientOutboundExtendedMessage,
       MatchesTheReferenceLayoutAndKeyAdvance) {
  for (const std::uint32_t version : {18U, 19U, 20U}) {
    KeyedOutbound keyed{version, takion::PsnEnvelope{0x4e}};
    EXPECT_TRUE(keyed.outbound->send_extended_message(pong))
        << "version " << version;
    ASSERT_EQ(keyed.transport.sent.size(), 1U) << "version " << version;
    const auto& wire = keyed.transport.sent[0];
    const std::size_t ext = version == 20 ? 8 : 0;
    ASSERT_EQ(wire.size(), 4 + 1 + ext + 12 + pong.size())
        << "version " << version;
    EXPECT_EQ(wire[3], std::byte{0x4e}) << "version " << version;
    EXPECT_EQ(wire[4], std::byte{0x0e}) << "version " << version;
    const auto* header = wire.data() + 5 + ext;
    EXPECT_EQ(header[0], std::byte{0xa0}) << "version " << version;
    EXPECT_EQ(header[3], std::byte{1000 & 0xff}) << "version " << version;
    EXPECT_EQ(header[4], std::byte{0}) << "version " << version;
    EXPECT_EQ(header[5], std::byte{0}) << "version " << version;
    EXPECT_EQ(header[6], std::byte{0x03}) << "version " << version;
    EXPECT_EQ(header[7], std::byte{0xe8}) << "version " << version;
    EXPECT_EQ(header[8], std::byte{1}) << "version " << version;
    EXPECT_EQ(header[9], std::byte{0}) << "version " << version;
    EXPECT_EQ(header[10], std::byte{0}) << "version " << version;
    EXPECT_EQ(header[11], std::byte{1}) << "version " << version;
    for (std::size_t index = 0; index < pong.size(); ++index)
      EXPECT_EQ(header[12 + index],
                pong[index] ^ static_cast<std::byte>((1000 + 16) & 0xff))
          << "version " << version << " index " << index;
    const std::size_t advance = 1 + ext + 12 + pong.size() + 16;
    EXPECT_EQ(keyed.advanced(), advance + advance % 16)
        << "version " << version;
    EXPECT_EQ(keyed.advanced(), version == 20 ? 54U : 38U)
        << "version " << version;
  }
}

TEST_F(TakionClientOutboundExtendedMessage, IsSentPlainWhenCryptoIsDisabled) {
  PlainOutbound plain{18, takion::CryptoPolicy::disabled};
  EXPECT_TRUE(plain.outbound.send_extended_message(pong));
  ASSERT_EQ(plain.transport.sent.size(), 1U);
  std::vector<std::byte> expected(13, std::byte{});
  expected[0] = std::byte{0x0e};
  expected[9] = std::byte{1};
  expected.insert(expected.end(), pong.begin(), pong.end());
  EXPECT_EQ(plain.transport.sent[0], expected);
}

TEST_F(TakionClientOutboundExtendedMessage, IsNotSentBelowVersion18) {
  for (const std::uint32_t version : {9U, 12U, 15U, 17U}) {
    KeyedOutbound keyed{version, takion::PlainEnvelope{}};
    const auto sent = keyed.outbound->send_extended_message(pong);
    ASSERT_FALSE(sent) << "version " << version;
    EXPECT_EQ(sent.error(), takion::ClientError::unsupported_version)
        << "version " << version;
    EXPECT_TRUE(keyed.transport.sent.empty()) << "version " << version;
    EXPECT_EQ(keyed.advanced(), 0U) << "version " << version;
  }
}

TEST_F(TakionClientOutboundExtendedMessage,
       IsNeverSentInPlaintextWhenCryptoIsRequired) {
  for (const bool bootstrap_control : {false, true}) {
    PlainOutbound plain{20, takion::CryptoPolicy::required, bootstrap_control};
    const auto sent = plain.outbound.send_extended_message(pong);
    ASSERT_FALSE(sent) << "bootstrap control " << bootstrap_control;
    EXPECT_EQ(sent.error(), takion::ClientError::crypto_not_installed)
        << "bootstrap control " << bootstrap_control;
    EXPECT_TRUE(plain.transport.sent.empty())
        << "bootstrap control " << bootstrap_control;
  }
}

}  // namespace
}  // namespace takion_test
