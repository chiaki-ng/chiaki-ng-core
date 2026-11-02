#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/protocol/takion/client/inbound.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/detail/control_message.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/detail/handshake_types.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/control_message_copy.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace takion_test {

namespace takion = libchiaking::net::protocol::takion;
namespace datagram = libchiaking::net::transport::datagram;

class InboundRecordingSink final : public takion::EventSink {
 public:
  std::vector<takion::Event> events;

  void on_event(takion::Event event) override {
    events.push_back(std::move(event));
  }

  auto single_rejection() const -> std::optional<takion::ClientError> {
    if (events.size() != 1) return std::nullopt;
    const auto* rejected =
        std::get_if<takion::client_event::PacketRejected>(&events.front());
    if (!rejected) return std::nullopt;
    return rejected->error;
  }

  template <class Input>
  auto last() const -> const Input* {
    if (events.empty()) return nullptr;
    return std::get_if<Input>(&events.back());
  }

  template <class Input>
  auto first() const -> const Input* {
    if (events.empty()) return nullptr;
    return std::get_if<Input>(&events.front());
  }

  auto last_rejection() const -> std::optional<takion::ClientError> {
    const auto* rejected = last<takion::client_event::PacketRejected>();
    if (!rejected) return std::nullopt;
    return rejected->error;
  }
};

class InboundRecordingHost final
    : public takion::client_detail::ClientInboundHost {
 public:
  bool connected{true};
  std::vector<std::uint32_t> acknowledgements;
  std::vector<takion::DataAcknowledgement> selective_acknowledgements;
  std::vector<takion::ControlMessage> controls;
  std::vector<takion::ClientError> failures;
  std::vector<std::vector<std::byte>> extended_messages;
  std::size_t init_acknowledgements{};
  std::size_t cookie_acknowledgements{};

  auto inbound_handle_init_ack(const takion::InitAcknowledgementChunk&)
      -> takion::ClientResult<void> override {
    ++init_acknowledgements;
    return {};
  }

  auto inbound_handle_cookie_ack(const takion::CookieAcknowledgementChunk&)
      -> takion::ClientResult<void> override {
    ++cookie_acknowledgements;
    return {};
  }

  auto inbound_send_ack(const takion::DataAcknowledgement& acknowledgement)
      -> takion::ClientResult<void> override {
    acknowledgements.push_back(acknowledgement.cumulative_sequence);
    selective_acknowledgements.push_back(acknowledgement);
    return {};
  }

  auto inbound_send_control(const takion::ControlMessage& message)
      -> takion::ClientResult<void> override {
    controls.push_back(copy_of(message));
    return {};
  }

  void inbound_fail_terminal(takion::ClientError error) override {
    failures.push_back(error);
  }

  auto inbound_send_extended_message(std::span<const std::byte> message)
      -> takion::ClientResult<void> override {
    extended_messages.emplace_back(message.begin(), message.end());
    return {};
  }

  auto inbound_connected() const -> bool override { return connected; }
};

class InboundWire {
 public:
  static auto profile(std::uint32_t version) -> takion::WireProfile {
    auto resolved = takion::resolve_takion_wire_profile(version);
    EXPECT_TRUE(resolved) << "version " << version;
    return resolved.value();
  }

  static auto packet(takion::PacketFamily family,
                     std::span<const std::byte> payload, std::uint32_t version)
      -> datagram::OwnedDatagram {
    const takion::PacketType type{static_cast<std::uint8_t>(family)};
    const auto wire_profile = profile(version);
    takion::PlainPacket value{
        type,
        wire_profile.uses_extended_header(type)
            ? std::optional<
                  takion::ExtendedPacketHeader>{takion::ExtendedPacketHeader{0,
                                                                             0}}
            : std::nullopt,
        {payload.begin(), payload.end()},
    };
    auto plain = takion::PacketCodec::encode_plain_packet(value, wire_profile);
    EXPECT_TRUE(plain);
    auto wrapped = takion::EnvelopeCodec::encode(plain.value().bytes(),
                                                 takion::PlainEnvelope{});
    EXPECT_TRUE(wrapped);
    return datagram::OwnedDatagram(wrapped.value().begin(),
                                   wrapped.value().end());
  }

  static auto plain_packet(std::uint8_t type, std::vector<std::byte> payload,
                           std::uint32_t version) -> datagram::OwnedDatagram {
    auto plain = takion::PacketCodec::encode_plain_packet(
        takion::PlainPacket{takion::PacketType{type}, std::nullopt,
                            std::move(payload)},
        profile(version));
    EXPECT_TRUE(plain);
    return datagram::OwnedDatagram(plain.value().begin(), plain.value().end());
  }

  static auto fragment_payload(std::uint32_t sequence, std::uint16_t channel,
                               const std::vector<std::byte>& message,
                               std::byte data_type = std::byte{})
      -> std::vector<std::byte> {
    std::vector<std::byte> payload;
    payload.reserve(9 + message.size());
    for (int shift = 24; shift >= 0; shift -= 8)
      payload.push_back(static_cast<std::byte>(
          (sequence >> static_cast<unsigned>(shift)) & 0xffU));
    payload.push_back(static_cast<std::byte>((channel >> 8U) & 0xffU));
    payload.push_back(static_cast<std::byte>(channel & 0xffU));
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{0});
    payload.push_back(data_type);
    payload.insert(payload.end(), message.begin(), message.end());
    return payload;
  }

  static auto data_datagram(std::uint32_t sequence, std::uint16_t channel,
                            const std::vector<std::byte>& message,
                            std::uint32_t version,
                            std::byte data_type = std::byte{})
      -> datagram::OwnedDatagram {
    auto encoded = takion::ControlChunkCodec::encode_packet(
        takion::ControlChunkPacket{takion::DataChunk{
            takion::ControlHeader{
                takion::VerificationTag{0x11223344}, {}, 0, 0x01},
            fragment_payload(sequence, channel, message, data_type),
        }});
    EXPECT_TRUE(encoded);
    return packet(takion::PacketFamily::control, encoded.value().bytes(),
                  version);
  }

  static auto acknowledgement_datagram(std::uint32_t version)
      -> datagram::OwnedDatagram {
    auto encoded = takion::ControlChunkCodec::encode_packet(
        takion::ControlChunkPacket{takion::DataAcknowledgementChunk{
            takion::ControlHeader{
                takion::VerificationTag{0x11223344}, {}, 0, 0},
            takion::DataAcknowledgement{
                100, takion::default_advertised_receive_window, {}, {}},
        }});
    EXPECT_TRUE(encoded);
    return packet(takion::PacketFamily::control, encoded.value().bytes(),
                  version);
  }

  static auto channel_config() -> takion::ReliableChannelConfig {
    return takion::ReliableChannelConfig{
        .initial_local_sequence = 100,
        .initial_remote_sequence = 1,
        .maximum_message_size = 4096,
        .maximum_fragment_size = 1200,
        .maximum_fragment_count = 64,
        .retransmission_window_size = 16,
        .receive_reorder_size = 16,
        .retransmission_timeout_us = 1000000,
        .maximum_retries = 5,
    };
  }

  static auto established() -> takion::EstablishedHandshake {
    return takion::EstablishedHandshake{
        takion::VerificationTag{0x11223344},
        takion::VerificationTag{0xa1b2c3d4},
        takion::InitialSequenceNumber{100},
        takion::InitialSequenceNumber{1},
        takion::StreamCount{100},
        takion::StreamCount{100},
    };
  }

  static auto negotiated_config() -> takion::ClientConfig {
    takion::ClientConfig config;
    config.bootstrap_version = 15;
    config.profile_selection = takion::NegotiatedTakionProfiles{{15, 20}};
    config.crypto_policy = takion::CryptoPolicy::disabled;
    return config;
  }

  static auto fixed_config(std::uint32_t version) -> takion::ClientConfig {
    takion::ClientConfig config;
    config.bootstrap_version = version;
    config.profile_selection = takion::FixedTakionProfile{version};
    config.crypto_policy = takion::CryptoPolicy::disabled;
    return config;
  }

  static auto keyed_config(std::uint32_t version) -> takion::ClientConfig {
    takion::ClientConfig config;
    config.bootstrap_version = version;
    config.profile_selection = takion::FixedTakionProfile{version};
    config.crypto_policy = takion::CryptoPolicy::required;
    return config;
  }

  static auto encoded_control(const takion::ControlMessage& message)
      -> std::vector<std::byte> {
    auto encoded = takion::ControlCodec{}.encode(message);
    EXPECT_TRUE(encoded);
    return std::vector<std::byte>(encoded.value().begin(),
                                  encoded.value().end());
  }

  static auto encoded_ack(std::uint32_t version) -> std::vector<std::byte> {
    return encoded_control(
        takion::ControlMessage{takion::ProtocolRequestAck{version}});
  }

  static auto encoded_request(const std::vector<std::uint32_t>& versions)
      -> std::vector<std::byte> {
    return encoded_control(
        takion::ControlMessage{takion::ProtocolRequest{versions}});
  }
};

class InboundHarness {
 public:
  explicit InboundHarness(takion::ClientConfig client_config)
      : config(std::move(client_config)) {}

  auto session() -> takion::client_detail::InboundSession {
    profiles.push_back(InboundWire::profile(config.bootstrap_version));
    return takion::client_detail::InboundSession{&profiles.back(), &handshake,
                                                 &reliable, true};
  }

  void receive(datagram::OwnedDatagram datagram,
               const takion::client_detail::InboundSession& inbound_session) {
    inbound.process_datagram(datagram, inbound_session);
  }

  void deliver(std::uint32_t sequence, std::uint32_t version,
               const std::vector<std::byte>& message,
               std::byte data_type = std::byte{}) {
    receive(InboundWire::data_datagram(sequence, config.control_channel,
                                       message, version, data_type),
            session());
  }

  void acknowledge_request() {
    receive(InboundWire::acknowledgement_datagram(config.bootstrap_version),
            session());
  }

  test_support::FakeClock clock{0};
  takion::CryptoContext crypto{takion::CryptoPolicy::required};
  InboundRecordingSink events;
  InboundRecordingHost host;
  takion::ControlCodec control_codec{};
  takion::ClientConfig config;
  takion::ReliableChannelConfig channel{InboundWire::channel_config()};
  takion::EstablishedHandshake handshake{InboundWire::established()};
  takion::ReliableChannel reliable =
      takion::ReliableChannel::create(InboundWire::channel_config()).value();
  takion::client_detail::ClientInbound inbound{host,   clock,         crypto,
                                               events, control_codec, config};

 private:
  std::deque<takion::WireProfile> profiles;
};

class InboundCryptoCalls {
 public:
  std::size_t remote_authentications{};
  std::size_t remote_decryptions{};
};

class InboundXorMac {
 public:
  static auto of(std::uint64_t position, std::span<const std::byte> bytes)
      -> takion::Mac {
    std::uint32_t value = static_cast<std::uint32_t>(position) ^ 0x5eedu;
    for (const auto byte : bytes)
      value =
          (value << 3) ^ (value >> 5) ^ std::to_integer<std::uint32_t>(byte);
    return {static_cast<std::byte>(value >> 24),
            static_cast<std::byte>(value >> 16),
            static_cast<std::byte>(value >> 8), static_cast<std::byte>(value)};
  }
};

class InboundXorLocal final : public takion::CryptoDirection {
 public:
  auto apply_keystream(std::uint64_t position, std::span<const std::byte> input,
                       std::span<std::byte> bytes)
      -> takion::Expected<void, takion::CryptoError> override {
    if (input.data() != bytes.data()) std::ranges::copy(input, bytes.begin());
    for (auto& byte : bytes) byte ^= static_cast<std::byte>(position);
    return {};
  }

  auto authenticate(std::uint64_t position, std::span<const std::byte> bytes)
      -> takion::Expected<takion::Mac, takion::CryptoError> override {
    return InboundXorMac::of(position, bytes);
  }
};

class InboundXorRemote final : public takion::CryptoDirection {
 public:
  explicit InboundXorRemote(std::shared_ptr<InboundCryptoCalls> calls)
      : calls_(std::move(calls)) {}

  auto authenticate(std::uint64_t position, std::span<const std::byte> bytes)
      -> takion::Expected<takion::Mac, takion::CryptoError> override {
    ++calls_->remote_authentications;
    return InboundXorMac::of(position, bytes);
  }

  auto apply_keystream(std::uint64_t position, std::span<const std::byte> input,
                       std::span<std::byte> bytes)
      -> takion::Expected<void, takion::CryptoError> override {
    if (input.data() != bytes.data()) std::ranges::copy(input, bytes.begin());
    ++calls_->remote_decryptions;
    for (auto& byte : bytes) byte ^= static_cast<std::byte>(position);
    return {};
  }

 private:
  std::shared_ptr<InboundCryptoCalls> calls_;
};

class InboundXorProvider final : public takion::CryptoProvider {
 public:
  std::shared_ptr<InboundCryptoCalls> calls{
      std::make_shared<InboundCryptoCalls>()};

  auto create_local(takion::CryptoMaterial)
      -> takion::Expected<std::unique_ptr<takion::CryptoDirection>,
                          takion::CryptoError> override {
    return std::unique_ptr<takion::CryptoDirection>{new InboundXorLocal};
  }

  auto create_remote(takion::CryptoMaterial)
      -> takion::Expected<std::unique_ptr<takion::CryptoDirection>,
                          takion::CryptoError> override {
    return std::unique_ptr<takion::CryptoDirection>{
        new InboundXorRemote{calls}};
  }
};

class InboundKeyedReceiver {
 public:
  explicit InboundKeyedReceiver(std::uint32_t version)
      : harness(InboundWire::keyed_config(version)) {
    EXPECT_TRUE(
        sender.install_local(provider, takion::CryptoMaterial{secret()}));
    EXPECT_TRUE(harness.crypto.install_remote(
        provider, takion::CryptoMaterial{secret()}));
  }

  static auto secret() -> takion::SecretBytes {
    const std::byte value[]{std::byte{1}};
    return takion::SecretBytes{value};
  }

  auto protect(takion::PacketFamily family, std::vector<std::byte> payload,
               std::uint64_t position, std::size_t encrypted_size = 0)
      -> datagram::OwnedDatagram {
    const auto version = harness.config.bootstrap_version;
    auto wire = InboundWire::packet(family, payload, version);
    EXPECT_TRUE(sender.set_local_key_position(position));
    auto reservation = sender.protect_local(
        wire, family, InboundWire::profile(version),
        takion::PacketCryptoCoverage{
            0, wire.size(), wire.size() - encrypted_size, encrypted_size},
        1);
    EXPECT_TRUE(reservation);
    if (reservation) {
      EXPECT_TRUE(reservation.value().commit());
    }
    return wire;
  }

  void receive(datagram::OwnedDatagram datagram) {
    harness.receive(std::move(datagram), harness.session());
  }

  InboundXorProvider provider;
  takion::CryptoContext sender{takion::CryptoPolicy::required};
  InboundHarness harness;
};

}  // namespace takion_test
