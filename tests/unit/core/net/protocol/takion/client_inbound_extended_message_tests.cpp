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

class TakionInboundExtendedMessage : public ::testing::Test {
 protected:
  static auto ext_message(std::uint16_t flags,
                          const std::vector<std::byte>& body)
      -> std::vector<std::byte> {
    std::vector<std::byte> payload(12, std::byte{});
    payload[8] = std::byte{1};
    payload[10] = static_cast<std::byte>(flags >> 8);
    payload[11] = static_cast<std::byte>(flags);
    payload.insert(payload.end(), body.begin(), body.end());
    return payload;
  }

  static auto unversioned(std::uint16_t flags) -> datagram::OwnedDatagram {
    datagram::OwnedDatagram wire{std::byte{0x0e}};
    const auto payload = ext_message(flags, rtt_ping);
    wire.insert(wire.end(), payload.begin(), payload.end());
    return wire;
  }

  static auto rejections(const InboundRecordingSink& sink) -> std::size_t {
    return static_cast<std::size_t>(
        std::ranges::count_if(sink.events, [](const auto& event) {
          return std::holds_alternative<takion::client_event::PacketRejected>(
              event);
        }));
  }

  static inline const std::vector<std::byte> rtt_ping{
      std::byte{0x08}, std::byte{0x00}, std::byte{0x12}, std::byte{0x06},
      std::byte{0x08}, std::byte{0x01}, std::byte{0x10}, std::byte{0x05},
      std::byte{0x18}, std::byte{0x05}};
  static inline const std::vector<std::byte> rtt_pong{
      std::byte{0x08}, std::byte{0x00}, std::byte{0x12}, std::byte{0x06},
      std::byte{0x08}, std::byte{0x00}, std::byte{0x10}, std::byte{0x05},
      std::byte{0x18}, std::byte{0x05}};
};

TEST_F(TakionInboundExtendedMessage, APlainRttPingIsAnsweredFromVersion18) {
  for (const std::uint32_t version : {18U, 19U, 20U}) {
    InboundHarness harness{InboundWire::fixed_config(version)};
    const auto payload = ext_message(0, rtt_ping);
    harness.receive(InboundWire::packet(takion::PacketFamily::extended_message,
                                        payload, version),
                    harness.session());
    ASSERT_EQ(harness.host.extended_messages.size(), 1U)
        << "version " << version;
    EXPECT_EQ(harness.host.extended_messages[0], rtt_pong)
        << "version " << version;
    ASSERT_EQ(harness.events.events.size(), 1U) << "version " << version;
    const auto* probe =
        harness.events.first<takion::client_event::ProbeReceived>();
    ASSERT_NE(probe, nullptr) << "version " << version;
    EXPECT_EQ(probe->family, takion::PacketFamily::extended_message)
        << "version " << version;
    EXPECT_EQ(probe->bytes, payload) << "version " << version;
  }
}

TEST_F(TakionInboundExtendedMessage, IsRejectedBelowVersion18) {
  for (const std::uint32_t version : {7U, 9U, 12U, 15U, 17U}) {
    InboundHarness harness{InboundWire::fixed_config(version)};
    harness.receive(unversioned(0), harness.session());
    EXPECT_EQ(harness.events.single_rejection(),
              takion::ClientError::unsupported_version)
        << "version " << version;
    EXPECT_TRUE(harness.host.extended_messages.empty())
        << "version " << version;
  }
}

TEST_F(TakionInboundExtendedMessage,
       AnEncryptedOneBelowVersion18IsRejectedWithoutCryptoWork) {
  InboundKeyedReceiver keyed{17};
  keyed.receive(unversioned(1));
  EXPECT_EQ(keyed.harness.events.single_rejection(),
            takion::ClientError::unsupported_version);
  EXPECT_EQ(keyed.provider.calls->remote_authentications, 0U);
  EXPECT_EQ(keyed.provider.calls->remote_decryptions, 0U);
  EXPECT_TRUE(keyed.harness.host.extended_messages.empty());
}

TEST_F(TakionInboundExtendedMessage, IsRejectedBeforeCryptoWithoutAPong) {
  for (const std::uint32_t version : {18U, 20U}) {
    for (const std::uint16_t flags : {std::uint16_t{0}, std::uint16_t{1}}) {
      InboundKeyedReceiver keyed{version};
      auto session = keyed.harness.session();
      session.crypto_installed = false;
      keyed.harness.receive(
          InboundWire::packet(takion::PacketFamily::extended_message,
                              ext_message(flags, rtt_ping), version),
          session);
      EXPECT_EQ(keyed.harness.events.single_rejection(),
                takion::ClientError::crypto_not_installed)
          << "version " << version << " flags " << flags;
      EXPECT_TRUE(keyed.harness.host.extended_messages.empty())
          << "version " << version << " flags " << flags;
      EXPECT_EQ(keyed.provider.calls->remote_authentications, 0U)
          << "version " << version << " flags " << flags;
      EXPECT_EQ(keyed.provider.calls->remote_decryptions, 0U)
          << "version " << version << " flags " << flags;
    }
  }
}

TEST_F(TakionInboundExtendedMessage,
       IsRejectedBeforeCryptoEvenWhenPlainControlIsAllowed) {
  auto bootstrap = InboundWire::keyed_config(20);
  bootstrap.allow_plain_control_until_crypto = true;
  InboundHarness harness{bootstrap};
  auto session = harness.session();
  session.crypto_installed = false;
  harness.receive(InboundWire::packet(takion::PacketFamily::extended_message,
                                      ext_message(0, rtt_ping), 20),
                  session);
  EXPECT_EQ(harness.events.single_rejection(),
            takion::ClientError::crypto_not_installed);
  EXPECT_TRUE(harness.host.extended_messages.empty());
}

TEST_F(TakionInboundExtendedMessage, RejectsANonProtobufOuterType) {
  for (const std::uint8_t outer :
       {std::uint8_t{1}, std::uint8_t{0x80}, std::uint8_t{0xff}}) {
    InboundHarness harness{InboundWire::fixed_config(18)};
    auto payload = ext_message(0, rtt_ping);
    payload[9] = std::byte{outer};
    harness.receive(InboundWire::packet(takion::PacketFamily::extended_message,
                                        payload, 18),
                    harness.session());
    EXPECT_EQ(harness.events.single_rejection(),
              takion::ClientError::packet_decode_failure)
        << "outer " << +outer;
    EXPECT_TRUE(harness.host.extended_messages.empty()) << "outer " << +outer;
  }
}

TEST_F(TakionInboundExtendedMessage,
       RejectsAnEncryptedNonProtobufOuterTypeAfterDecryptingIt) {
  InboundKeyedReceiver keyed{20};
  auto payload = ext_message(1, rtt_ping);
  payload[9] = std::byte{1};
  keyed.receive(keyed.protect(takion::PacketFamily::extended_message, payload,
                              6000, rtt_ping.size()));
  EXPECT_EQ(keyed.harness.events.single_rejection(),
            takion::ClientError::packet_decode_failure);
  EXPECT_EQ(keyed.provider.calls->remote_authentications, 1U);
  EXPECT_EQ(keyed.provider.calls->remote_decryptions, 1U);
  EXPECT_TRUE(keyed.harness.host.extended_messages.empty());
}

TEST_F(TakionInboundExtendedMessage, AnswersAnEncryptedProtobufPing) {
  InboundKeyedReceiver accepted{20};
  accepted.receive(accepted.protect(takion::PacketFamily::extended_message,
                                    ext_message(1, rtt_ping), 6000,
                                    rtt_ping.size()));
  EXPECT_EQ(accepted.provider.calls->remote_decryptions, 2U);
  EXPECT_EQ(accepted.harness.host.extended_messages.size(), 1U);
}

TEST_F(TakionInboundExtendedMessage, RttMessagesThatAreNotPingsGetNoPong) {
  InboundHarness harness{InboundWire::fixed_config(18)};
  const auto session = harness.session();
  harness.receive(InboundWire::packet(takion::PacketFamily::extended_message,
                                      ext_message(0, rtt_pong), 18),
                  session);
  const std::vector<std::byte> metric{std::byte{0x08}, std::byte{0x01}};
  harness.receive(InboundWire::packet(takion::PacketFamily::extended_message,
                                      ext_message(0, metric), 18),
                  session);
  const std::vector<std::byte> garbage{std::byte{0xff}};
  harness.receive(InboundWire::packet(takion::PacketFamily::extended_message,
                                      ext_message(0, garbage), 18),
                  session);
  EXPECT_TRUE(harness.host.extended_messages.empty());

  auto wrong_version = ext_message(0, rtt_ping);
  wrong_version[8] = std::byte{2};
  harness.receive(InboundWire::packet(takion::PacketFamily::extended_message,
                                      wrong_version, 18),
                  session);
  const std::vector<std::byte> short_header(11, std::byte{});
  harness.receive(InboundWire::packet(takion::PacketFamily::extended_message,
                                      short_header, 18),
                  session);
  harness.receive(InboundWire::packet(takion::PacketFamily::extended_message,
                                      ext_message(1, rtt_ping), 18),
                  session);
  EXPECT_TRUE(harness.host.extended_messages.empty());
  EXPECT_EQ(rejections(harness.events), 3U);
}

TEST_F(TakionInboundExtendedMessage,
       AnEncryptedRttPingIsDecryptedAtTheKeyPositionPlusABlock) {
  InboundKeyedReceiver keyed{19};
  const std::uint64_t position = 90001;
  auto wire =
      keyed.protect(takion::PacketFamily::extended_message,
                    ext_message(1, rtt_ping), position, rtt_ping.size());
  ASSERT_GE(wire.size(), rtt_ping.size());
  for (std::size_t index = 0; index < rtt_ping.size(); ++index)
    EXPECT_EQ(wire[wire.size() - rtt_ping.size() + index],
              rtt_ping[index] ^ static_cast<std::byte>(
                                    static_cast<std::uint8_t>(position + 16)))
        << "index " << index;
  keyed.receive(std::move(wire));
  EXPECT_EQ(keyed.harness.crypto.remote_key_position(), position);
  ASSERT_EQ(keyed.harness.host.extended_messages.size(), 1U);
  EXPECT_EQ(keyed.harness.host.extended_messages[0], rtt_pong);
  const auto* probe =
      keyed.harness.events.last<takion::client_event::ProbeReceived>();
  ASSERT_NE(probe, nullptr);
  ASSERT_GE(probe->bytes.size(), 12U);
  EXPECT_TRUE(std::equal(probe->bytes.begin() + 12, probe->bytes.end(),
                         rtt_ping.begin(), rtt_ping.end()));
}

}  // namespace
}  // namespace takion_test
