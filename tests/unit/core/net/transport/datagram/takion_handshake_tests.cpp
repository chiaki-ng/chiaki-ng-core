#include "tests/unit/core/net/transport/datagram/loopback_rig.hpp"

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/takion/client.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/detail/handshake_types.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/profile.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace datagram_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;
namespace pf = libchiaking::platform;

class NullSink final : public takion::EventSink {
 public:
  void on_event(takion::Event event) override { (void)event; }
};

class TakionOverUdpTransport : public UdpLoopbackRig {
 protected:
  static constexpr int repetitions = 25;

  static auto profile() -> const takion::WireProfile& {
    static const auto value = takion::resolve_takion_wire_profile(7).value();
    return value;
  }

  static auto decode(libchiaking::net::transport::datagram::Datagram datagram)
      -> std::optional<takion::ControlChunkPacket> {
    auto unwrapped =
        takion::EnvelopeCodec::decode(datagram, takion::PlainEnvelope{});
    if (!unwrapped) return std::nullopt;
    auto plain =
        takion::PacketCodec::decode_plain_packet(unwrapped.value(), profile());
    if (!plain) return std::nullopt;
    auto chunk =
        takion::ControlChunkCodec::decode_packet(plain.value().payload);
    if (!chunk) return std::nullopt;
    return chunk.value();
  }

  static auto handshake_cookie() -> takion::HandshakeCookie {
    takion::HandshakeCookie cookie{};
    for (std::size_t index = 0; index < cookie.size(); ++index)
      cookie[index] = static_cast<std::byte>(index ^ 0x5a);
    return cookie;
  }

  void send_control(const takion::ControlChunkPacket& packet) {
    const auto encoded = takion::ControlChunkCodec::encode_packet(packet);
    ASSERT_TRUE(encoded);
    auto plain = takion::PacketCodec::encode_plain_packet(
        takion::PlainPacket{takion::PacketType{static_cast<std::uint8_t>(
                                takion::PacketFamily::control)},
                            std::nullopt,
                            {encoded.value().begin(), encoded.value().end()}},
        profile());
    ASSERT_TRUE(plain);
    auto wrapped = takion::EnvelopeCodec::encode(plain.value().bytes(),
                                                 takion::PlainEnvelope{});
    ASSERT_TRUE(wrapped);
    ASSERT_TRUE(second->send(wrapped.value().bytes()));
  }

  void expect_from_client(std::optional<takion::ControlChunkPacket>& packet) {
    ASSERT_TRUE(ready(*second, stop));
    const auto datagram = second->receive();
    ASSERT_TRUE(datagram);
    ASSERT_TRUE(datagram.value().has_value());
    packet = decode(datagram.value().value());
    ASSERT_TRUE(packet);
  }

  void connect_once() {
    ASSERT_NO_FATAL_FAILURE(open_pair());
    auto& client_side = *first;
    pf::SteadyClock clock_source;
    pf::DefaultThreadingProvider threading;
    pf::SystemSecureRandom random_source;
    pf::ClockProvider& clock{clock_source};
    pf::SecureRandomProvider& random{random_source};
    NullSink sink;
    takion::Client client(std::move(first), clock, threading, random, sink,
                          takion::ClientConfig{});
    ASSERT_TRUE(client.start());

    std::optional<takion::ControlChunkPacket> init_packet;
    ASSERT_NO_FATAL_FAILURE(expect_from_client(init_packet));
    ASSERT_TRUE(std::holds_alternative<takion::InitChunk>(*init_packet));
    const auto init = std::get<takion::InitChunk>(*init_packet);
    EXPECT_EQ(init.header.verification_tag.value, 0U);
    EXPECT_NE(init.values.tag.value, 0U);

    const auto cookie_bytes = handshake_cookie();
    const takion::InitAcknowledgementValues acknowledgement{
        {takion::VerificationTag{0xa1b2c3d4},
         takion::AdvertisedReceiveWindow{
             takion::default_advertised_receive_window},
         takion::StreamCount{100}, takion::StreamCount{100},
         takion::InitialSequenceNumber{0x01020304}},
        cookie_bytes};
    ASSERT_NO_FATAL_FAILURE(send_control(
        takion::ControlChunkPacket{takion::InitAcknowledgementChunk{
            takion::ControlHeader{init.values.tag, {}, 0, 0},
            acknowledgement,
        }}));
    ASSERT_TRUE(ready(client_side, stop));
    ASSERT_TRUE(client.pump_once());

    std::optional<takion::ControlChunkPacket> cookie_packet;
    ASSERT_NO_FATAL_FAILURE(expect_from_client(cookie_packet));
    ASSERT_TRUE(std::holds_alternative<takion::CookieChunk>(*cookie_packet));
    const auto& cookie = std::get<takion::CookieChunk>(*cookie_packet);
    EXPECT_EQ(cookie.header.verification_tag.value,
              acknowledgement.init.tag.value);
    EXPECT_TRUE(std::equal(cookie.cookie.begin(), cookie.cookie.end(),
                           cookie_bytes.begin()));

    ASSERT_NO_FATAL_FAILURE(send_control(
        takion::ControlChunkPacket{takion::CookieAcknowledgementChunk{
            takion::ControlHeader{init.values.tag, {}, 0, 0}}}));
    ASSERT_TRUE(ready(client_side, stop));
    ASSERT_TRUE(client.pump_once());
    EXPECT_EQ(client.snapshot().state, takion::ClientState::connected);
    EXPECT_TRUE(client.request_stop());
    EXPECT_TRUE(second->close());
  }
};

TEST_F(TakionOverUdpTransport, CompletesTheHandshakeOverLoopback) {
  for (int round = 0; round < repetitions; ++round) {
    SCOPED_TRACE(round);
    ASSERT_NO_FATAL_FAILURE(connect_once());
  }
}

}  // namespace
}  // namespace datagram_test
