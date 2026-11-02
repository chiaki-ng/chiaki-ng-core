#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/remote_play/stream/active_session.hpp"
#include "src/core/net/protocol/remote_play/stream/active_takion.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {

struct ActiveSessionTransportState {
  std::vector<libchiaking::net::transport::datagram::OwnedDatagram> sent;
  std::deque<libchiaking::net::transport::datagram::OwnedDatagram> incoming;
  unsigned close_count{};
  bool fail_send{};
  std::size_t failed_receives{};
  bool fail_when_drained{};
  std::size_t receive_calls{};
  const libchiaking::platform::Pollable* polled{};
  bool exact{};
};

class ActiveSessionPollable final : public libchiaking::platform::Pollable {
 public:
  auto owner() const noexcept
      -> const libchiaking::platform::NetworkProvider& override {
    std::abort();
  }
};

class ActiveSessionTransport final
    : public libchiaking::net::transport::datagram::DatagramTransport {
 public:
  using TransportError = libchiaking::net::transport::datagram::TransportError;

  explicit ActiveSessionTransport(
      std::shared_ptr<ActiveSessionTransportState> state)
      : state_(std::move(state)) {}

  auto send(std::span<const std::byte> bytes)
      -> libchiaking::Expected<std::size_t, TransportError> override {
    if (state_->fail_send)
      return libchiaking::unexpected(TransportError::io_failure);
    state_->sent.emplace_back(bytes.begin(), bytes.end());
    return bytes.size();
  }

  auto receive() -> libchiaking::Expected<
      std::optional<libchiaking::net::transport::datagram::Datagram>,
      TransportError> override {
    using libchiaking::net::transport::datagram::Datagram;
    ++state_->receive_calls;
    if (state_->failed_receives) {
      --state_->failed_receives;
      return libchiaking::unexpected(TransportError::io_failure);
    }
    if (state_->incoming.empty() && state_->fail_when_drained)
      return libchiaking::unexpected(TransportError::io_failure);
    if (state_->incoming.empty()) return std::optional<Datagram>{};
    received_ = std::move(state_->incoming.front());
    state_->incoming.pop_front();
    return std::optional<Datagram>{received_};
  }

  auto close() -> libchiaking::Expected<void, TransportError> override {
    ++state_->close_count;
    return {};
  }

  auto pollable() const noexcept
      -> const libchiaking::platform::Pollable* override {
    return state_->polled;
  }

  auto readiness_exact() const noexcept -> bool override {
    return state_->exact;
  }

 private:
  std::shared_ptr<ActiveSessionTransportState> state_;
  libchiaking::net::transport::datagram::OwnedDatagram received_;
};

class ActiveSessionRandom final
    : public libchiaking::platform::SecureRandomProvider {
 public:
  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    if (fail)
      return libchiaking::unexpected(
          libchiaking::platform::PlatformError::io_failure);
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = static_cast<std::byte>(i + 1);
    return {};
  }

  bool fail{};
};

class ActiveSessionKeyPair final : public libchiaking::platform::EcdhKeyPair {
 public:
  auto public_coordinates(std::span<std::byte>) const
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }

  auto derive_secret(std::span<const std::byte>,
                     std::span<std::byte> output) const
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    ++derive_calls;
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = static_cast<std::byte>(i + 1);
    return {};
  }

  mutable unsigned derive_calls{};
};

class ActiveSessionCrypto final : public libchiaking::platform::CryptoProvider {
 public:
  using CryptoError = libchiaking::platform::CryptoError;

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, CryptoError> override {
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte> key,
                   std::span<const std::byte> input,
                   std::span<std::byte> digest)
      -> libchiaking::Expected<void, CryptoError> override {
    ++hmac_calls;
    EXPECT_EQ(key.size(), 16U);
    EXPECT_EQ(input.size(), 65U);
    EXPECT_EQ(digest.size(), 32U);
    for (std::size_t i = 0; i < digest.size(); ++i)
      digest[i] = static_cast<std::byte>(0xa0 + i);
    return {};
  }

  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, CryptoError> override {
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, CryptoError> override {
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

  unsigned hmac_calls{};
};

class ActiveSessionPassThroughDirection final
    : public libchiaking::net::protocol::takion::CryptoDirection {
 public:
  using CryptoError = libchiaking::net::protocol::takion::CryptoError;
  using TakionMac = libchiaking::net::protocol::takion::Mac;

  auto apply_keystream(std::uint64_t, std::span<const std::byte> input,
                       std::span<std::byte> bytes)
      -> libchiaking::Expected<void, CryptoError> override {
    if (input.data() != bytes.data()) std::ranges::copy(input, bytes.begin());
    return {};
  }

  auto authenticate(std::uint64_t, std::span<const std::byte>)
      -> libchiaking::Expected<TakionMac, CryptoError> override {
    return TakionMac{};
  }
};

class ActiveSessionTakionCrypto final
    : public libchiaking::net::protocol::takion::CryptoProvider {
 public:
  using CryptoDirection = libchiaking::net::protocol::takion::CryptoDirection;
  using CryptoError = libchiaking::net::protocol::takion::CryptoError;
  using CryptoMaterial = libchiaking::net::protocol::takion::CryptoMaterial;

  explicit ActiveSessionTakionCrypto(bool check_indices = true)
      : check_indices_(check_indices) {}

  auto create_local(CryptoMaterial material)
      -> libchiaking::Expected<std::unique_ptr<CryptoDirection>,
                               CryptoError> override {
    if (check_indices_) {
      EXPECT_EQ(material.gkcrypt_index(), std::optional<std::uint8_t>{2});
    }
    return std::unique_ptr<CryptoDirection>{
        new ActiveSessionPassThroughDirection};
  }

  auto create_remote(CryptoMaterial material)
      -> libchiaking::Expected<std::unique_ptr<CryptoDirection>,
                               CryptoError> override {
    if (check_indices_) {
      EXPECT_EQ(material.gkcrypt_index(), std::optional<std::uint8_t>{3});
    }
    return std::unique_ptr<CryptoDirection>{
        new ActiveSessionPassThroughDirection};
  }

 private:
  bool check_indices_;
};

class ActiveSessionNegotiationKeyPair final
    : public libchiaking::platform::EcdhKeyPair {
 public:
  auto public_coordinates(std::span<std::byte> output) const
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = static_cast<std::byte>(i);
    return {};
  }

  auto derive_secret(std::span<const std::byte>, std::span<std::byte>) const
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }
};

class ActiveSessionNegotiationCrypto final
    : public libchiaking::platform::CryptoProvider {
 public:
  using CryptoError = libchiaking::platform::CryptoError;
  using EcdhCurve = libchiaking::platform::EcdhCurve;

  auto generate_ecdh(EcdhCurve curve) -> libchiaking::Expected<
      std::unique_ptr<libchiaking::platform::EcdhKeyPair>,
      CryptoError> override {
    generated.push_back(curve);
    return std::unique_ptr<libchiaking::platform::EcdhKeyPair>{
        new ActiveSessionNegotiationKeyPair};
  }

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, CryptoError> override {
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte> input,
                   std::span<std::byte> digest)
      -> libchiaking::Expected<void, CryptoError> override {
    EXPECT_TRUE(input.size() == 65 || input.size() == 133) << input.size();
    for (auto& byte : digest) byte = std::byte{0x11};
    return {};
  }

  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, CryptoError> override {
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, CryptoError> override {
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

  std::vector<EcdhCurve> generated;
};

class ActiveSessionLogSink final
    : public libchiaking::platform::DiagnosticsProvider {
 public:
  void write(libchiaking::platform::LogRecord record) noexcept override {
    messages.emplace_back(record.message);
  }

  std::vector<std::string> messages;
};

class ActiveTakionEvents final
    : public libchiaking::net::protocol::takion::EventSink {
 public:
  void on_event(libchiaking::net::protocol::takion::Event event) override {
    values.push_back(std::move(event));
  }

  std::vector<libchiaking::net::protocol::takion::Event> values;
};

class H264Bits {
 public:
  static auto header() -> std::vector<std::byte> {
    H264Bits bits;
    bits.u(100, 8);
    bits.u(0, 8);
    bits.u(40, 8);
    bits.ue(0);
    bits.ue(1);
    bits.ue(0);
    bits.ue(0);
    bits.u(0, 2);
    bits.ue(4);
    return bits.nal(7);
  }

  static auto frame(bool intra) -> std::vector<std::byte> {
    H264Bits bits;
    bits.ue(0);
    bits.ue(intra ? 2 : 0);
    if (!intra) {
      bits.ue(0);
      bits.u(3, 8);
      bits.u(0, 1);
      bits.u(1, 1);
      bits.ue(0);
      bits.ue(0);
      bits.ue(3);
    }
    return bits.nal(intra ? 5 : 1);
  }

 private:
  void u(std::uint32_t value, unsigned count) {
    for (unsigned index = count; index > 0; --index) {
      if (used_ % 8 == 0) data_.push_back(std::byte{0});
      if ((value >> (index - 1)) & 1U)
        data_.back() |= static_cast<std::byte>(1U << (7U - used_ % 8));
      ++used_;
    }
  }

  void ue(std::uint32_t value) {
    unsigned width = 0;
    for (auto n = value + 1; n; n >>= 1U) ++width;
    u(0, width - 1);
    u(value + 1, width);
  }

  auto nal(unsigned type) -> std::vector<std::byte> {
    u(1, 1);
    while (used_ % 8) u(0, 1);
    std::vector<std::byte> out{std::byte{0}, std::byte{0}, std::byte{0},
                               std::byte{1},
                               static_cast<std::byte>(type | 0x60U)};
    unsigned zeros = 0;
    for (const auto byte : data_) {
      if (zeros >= 2 && std::to_integer<unsigned>(byte) <= 3) {
        out.push_back(std::byte{3});
        zeros = 0;
      }
      out.push_back(byte);
      zeros = byte == std::byte{0} ? zeros + 1 : 0;
    }
    out.resize(out.size() + 32);
    return out;
  }

  std::vector<std::byte> data_;
  unsigned used_{};
};

class ActiveSessionWire {
 public:
  using OwnedDatagram = libchiaking::net::transport::datagram::OwnedDatagram;
  using Profile = libchiaking::net::protocol::takion::WireProfile;

  static auto secret(std::string_view text)
      -> std::shared_ptr<const libchiaking::SecretString> {
    return std::make_shared<const libchiaking::SecretString>(text);
  }

  static auto prepared(libchiaking::platform::EcdhCurve curve)
      -> libchiaking::net::protocol::remote_play::PreparedEcdh {
    libchiaking::net::protocol::remote_play::PreparedEcdh result{
        curve, std::make_unique<ActiveSessionKeyPair>(), {}};
    result.public_key.size =
        libchiaking::platform::ecdh_public_coordinates_size(curve) + 1;
    result.public_key.bytes[0] = std::byte{0x04};
    for (std::size_t i = 1; i < result.public_key.size; ++i)
      result.public_key.bytes[i] = static_cast<std::byte>(i - 1);
    for (std::size_t i = 0; i < result.public_key.signature.size(); ++i)
      result.public_key.signature[i] = static_cast<std::byte>(0xa0 + i);
    return result;
  }

  static auto route(
      std::shared_ptr<ActiveSessionTransportState> state, std::uint32_t version,
      const Profile& profile,
      libchiaking::net::protocol::remote_play::RouteKind kind =
          libchiaking::net::protocol::remote_play::RouteKind::local_direct,
      libchiaking::net::protocol::remote_play::DontFragmentPolicy
          dont_fragment = libchiaking::net::protocol::remote_play::
              DontFragmentPolicy::enabled)
      -> libchiaking::net::protocol::remote_play::EstablishedRoute {
    const libchiaking::net::protocol::senkusha::Result calibration{
        version, {}, 1454, 1454, profile};
    return libchiaking::net::protocol::remote_play::EstablishedRoute{
        {std::make_unique<ActiveSessionTransport>(std::move(state)),
         {libchiaking::platform::IpAddress::loopback_ipv4(), 50000},
         {libchiaking::platform::IpAddress::loopback_ipv4(), 9295},
         kind,
         dont_fragment},
        calibration};
  }

  static auto inbound_control(
      libchiaking::net::protocol::takion::ControlChunkPacket chunk,
      const Profile& profile) -> OwnedDatagram {
    namespace takion = libchiaking::net::protocol::takion;
    const auto encoded = takion::ControlChunkCodec::encode_packet(chunk);
    EXPECT_TRUE(encoded);
    const takion::PlainPacket packet{
        takion::PacketType{
            static_cast<std::uint8_t>(takion::PacketFamily::control)},
        std::nullopt,
        {encoded.value().begin(), encoded.value().end()}};
    const auto datagram =
        takion::PacketCodec::encode_plain_packet(packet, profile);
    EXPECT_TRUE(datagram);
    return OwnedDatagram(datagram.value().begin(), datagram.value().end());
  }

  static auto handshake_replies(const Profile& profile,
                                bool patterned_cookie = false)
      -> std::vector<OwnedDatagram> {
    namespace takion = libchiaking::net::protocol::takion;
    takion::HandshakeCookie cookie{};
    if (patterned_cookie)
      for (std::size_t i = 0; i < cookie.size(); ++i)
        cookie[i] = static_cast<std::byte>(i);
    const takion::InitAcknowledgementValues acknowledgement{
        {takion::VerificationTag{0xa1b2c3d4},
         takion::AdvertisedReceiveWindow{
             takion::default_advertised_receive_window},
         takion::StreamCount{100}, takion::StreamCount{100},
         takion::InitialSequenceNumber{0x01020304}},
        cookie};
    return {inbound_control(
                takion::InitAcknowledgementChunk{
                    takion::ControlHeader{
                        takion::VerificationTag{0x01020304}, {}, 0, 0},
                    acknowledgement},
                profile),
            inbound_control(
                takion::CookieAcknowledgementChunk{takion::ControlHeader{
                    takion::VerificationTag{0x01020304}, {}, 0, 0}},
                profile)};
  }

  static auto acknowledgement(std::uint32_t sequence, const Profile& profile)
      -> OwnedDatagram {
    namespace takion = libchiaking::net::protocol::takion;
    return inbound_control(
        takion::DataAcknowledgementChunk{
            takion::ControlHeader{
                takion::VerificationTag{0x01020304}, {}, 0, 0},
            takion::DataAcknowledgement{
                sequence, takion::default_advertised_receive_window, {}, {}}},
        profile);
  }

  static auto data_datagram(std::uint32_t sequence, std::uint8_t data_type,
                            std::vector<std::byte> payload,
                            const Profile& profile) -> OwnedDatagram {
    namespace takion = libchiaking::net::protocol::takion;
    takion::ReliableChannel peer =
        takion::ReliableChannel::create(
            {
                .initial_local_sequence = sequence,
                .initial_remote_sequence = 0x01020304,
                .maximum_message_size = 1024 * 1024,
                .maximum_fragment_size = 1200,
                .maximum_fragment_count = 1024,
                .retransmission_window_size = 256,
                .receive_reorder_size = 256,
                .retransmission_timeout_us = 100'000,
                .maximum_retries = 5,
            })
            .value();
    const auto sent =
        peer.send(1, takion::MessageDataType{data_type}, payload, 0);
    EXPECT_TRUE(sent.has_value());
    EXPECT_EQ(sent->fragments.size(), 1U);
    return inbound_control(
        takion::DataChunk{
            takion::ControlHeader{
                takion::VerificationTag{0x01020304}, {}, 0, 1},
            std::vector<std::byte>(sent->fragments[0].payload.begin(),
                                   sent->fragments[0].payload.end())},
        profile);
  }

  static auto inbound_message(
      libchiaking::net::protocol::takion::ControlMessage message,
      std::uint32_t sequence, const Profile& profile) -> OwnedDatagram {
    const libchiaking::net::protocol::takion::ControlCodec codec;
    const auto encoded = codec.encode(message);
    EXPECT_TRUE(encoded);
    return data_datagram(
        sequence, 0,
        std::vector<std::byte>(encoded.value().begin(), encoded.value().end()),
        profile);
  }

  static auto sequence_of(std::span<const std::byte> payload) -> std::uint32_t {
    std::uint32_t sequence = 0;
    for (std::size_t i = 0; i < 4; ++i)
      sequence = (sequence << 8U) | std::to_integer<std::uint32_t>(payload[i]);
    return sequence;
  }

  static auto data_chunk(const OwnedDatagram& wire, const Profile& profile,
                         bool control_only = true)
      -> std::optional<libchiaking::net::protocol::takion::DataChunk> {
    namespace takion = libchiaking::net::protocol::takion;
    const auto plain = takion::PacketCodec::decode_plain_packet(wire, profile);
    if (!plain) return std::nullopt;
    if (control_only &&
        plain.value().type.family() != takion::PacketFamily::control)
      return std::nullopt;
    const auto chunk =
        takion::ControlChunkCodec::decode_packet(plain.value().payload);
    if (!chunk) return std::nullopt;
    if (const auto* data = std::get_if<takion::DataChunk>(&chunk.value()))
      return *data;
    return std::nullopt;
  }

  static auto sent_message(const OwnedDatagram& wire, const Profile& profile)
      -> std::optional<std::pair<
          std::uint32_t, libchiaking::net::protocol::takion::ControlMessage>> {
    namespace takion = libchiaking::net::protocol::takion;
    const auto data = data_chunk(wire, profile);
    if (!data) return std::nullopt;
    const auto sequence = sequence_of(data->payload);
    takion::ReliableChannel decoder =
        takion::ReliableChannel::create(
            {
                .initial_local_sequence = 0,
                .initial_remote_sequence = sequence,
                .maximum_message_size = 1024 * 1024,
                .maximum_fragment_size = 2048,
                .maximum_fragment_count = 16,
                .retransmission_window_size = 16,
                .receive_reorder_size = 16,
                .retransmission_timeout_us = 100'000,
                .maximum_retries = 5,
            })
            .value();
    const auto received =
        decoder.receive(data->payload, (data->header.flags & 0x01U) != 0, 0);
    if (received.anomaly.has_value() || received.messages.size() != 1)
      return std::nullopt;
    auto message = takion::ControlCodec{}.decode(received.messages[0].payload);
    if (!message) return std::nullopt;
    return std::pair{sequence, std::move(message).value()};
  }

  static auto sent_control(const OwnedDatagram& wire, const Profile& profile)
      -> std::optional<std::pair<
          std::uint16_t, libchiaking::net::protocol::takion::ControlMessage>> {
    namespace takion = libchiaking::net::protocol::takion;
    const auto data = data_chunk(wire, profile);
    if (!data || data->payload.size() < takion::first_fragment_overhead)
      return std::nullopt;
    const auto channel = static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(data->payload[4]) << 8U |
        std::to_integer<std::uint16_t>(data->payload[5]));
    auto message = takion::ControlCodec{}.decode(
        std::span<const std::byte>{data->payload}.subspan(
            takion::first_fragment_overhead));
    if (!message) return std::nullopt;
    return std::pair{channel, std::move(message.value())};
  }

  static auto stream_info(bool valid)
      -> libchiaking::net::protocol::takion::StreamInfo {
    namespace takion = libchiaking::net::protocol::takion;
    takion::StreamInfo info;
    info.audio_header.assign(valid ? 14 : 13, std::byte{0});
    info.audio_header[0] = std::byte{2};
    info.audio_header[1] = std::byte{16};
    info.resolutions.push_back(takion::StreamInfoResolution{
        1280, 720, {std::byte{0x01}, std::byte{0x02}}});
    return info;
  }

  static auto signed_bang(bool tamper)
      -> libchiaking::net::protocol::takion::Bang {
    libchiaking::net::protocol::takion::Bang value;
    value.server_version = 12;
    value.session_key =
        libchiaking::net::protocol::takion::SharedSecretText{"session-id"};
    value.version_accepted = true;
    value.encrypted_key_accepted = true;
    value.ecdh_pub_key = std::vector<std::byte>(65);
    value.ecdh_pub_key->at(0) = std::byte{0x04};
    for (std::size_t i = 1; i < 65; ++i)
      value.ecdh_pub_key->at(i) = static_cast<std::byte>(i - 1);
    value.ecdh_sig = std::vector<std::byte>(32);
    for (std::size_t i = 0; i < 32; ++i)
      value.ecdh_sig->at(i) = static_cast<std::byte>(0xa0 + i);
    if (tamper) value.ecdh_sig->at(31) ^= std::byte{1};
    return value;
  }

  static auto congestion_packets(const ActiveSessionTransportState& state,
                                 const Profile& profile)
      -> std::vector<std::vector<std::byte>> {
    namespace takion = libchiaking::net::protocol::takion;
    std::vector<std::vector<std::byte>> packets;
    for (const auto& wire : state.sent) {
      auto plain = takion::PacketCodec::decode_plain_packet(wire, profile);
      if (plain &&
          plain.value().type.family() == takion::PacketFamily::congestion)
        packets.emplace_back(plain.value().payload.begin(),
                             plain.value().payload.end());
    }
    return packets;
  }

  static auto init_packets(const ActiveSessionTransportState& state,
                           const Profile& profile) -> std::size_t {
    namespace takion = libchiaking::net::protocol::takion;
    std::size_t count = 0;
    for (const auto& wire : state.sent) {
      auto plain = takion::PacketCodec::decode_plain_packet(wire, profile);
      if (!plain ||
          plain.value().type.family() != takion::PacketFamily::control)
        continue;
      auto chunk =
          takion::ControlChunkCodec::decode_packet(plain.value().payload);
      if (chunk && std::holds_alternative<takion::InitChunk>(chunk.value()))
        ++count;
    }
    return count;
  }
};

class ActiveSessionRig {
 public:
  using OwnedDatagram = libchiaking::net::transport::datagram::OwnedDatagram;
  using Profile = libchiaking::net::protocol::takion::WireProfile;
  using ControlProfile =
      libchiaking::net::protocol::remote_play::ActiveControlProfile;
  using Family = libchiaking::net::protocol::takion::PacketFamily;

  explicit ActiveSessionRig(std::uint32_t takion_version = 9)
      : version(takion_version), profile(resolve(takion_version)) {}

  static auto resolve(std::uint32_t version) -> Profile {
    const auto resolved =
        libchiaking::net::protocol::takion::resolve_takion_wire_profile(
            version);
    EXPECT_TRUE(resolved) << version;
    return resolved.value();
  }

  static auto console_units(bool idr_on_fec_failure = true) -> ControlProfile {
    return {
        libchiaking::net::protocol::takion::ControllerConnectionType::dualsense,
        libchiaking::net::protocol::remote_play::AudioAssemblyPolicy::
            console_units,
        {},
        false,
        idr_on_fec_failure};
  }

  void start(ControlProfile control = {},
             libchiaking::net::protocol::takion::ClientConfig config = {},
             libchiaking::platform::EcdhCurve curve =
                 libchiaking::platform::EcdhCurve::secp256k1) {
    auto prepared_key = ActiveSessionWire::prepared(curve);
    pair = static_cast<ActiveSessionKeyPair*>(prepared_key.key_pair.get());
    libchiaking::SecretArray<16> handshake_key;
    auto started =
        libchiaking::net::protocol::remote_play::ActiveSession::start(
            ActiveSessionWire::route(state, version, profile), clock,
            test_support::process_threading(), random, crypto, takion_crypto,
            std::move(handshake_key), std::move(prepared_key),
            ActiveSessionWire::secret("session-id"),
            ActiveSessionWire::secret("launch-spec"), std::move(config),
            std::move(control));
    ASSERT_TRUE(started);
    owner = std::move(started.value());
  }

  void answer_handshake(bool patterned_cookie = false) {
    for (auto& reply :
         ActiveSessionWire::handshake_replies(profile, patterned_cookie))
      state->incoming.push_back(std::move(reply));
  }

  void make_ready(
      ControlProfile control = console_units(),
      std::vector<std::byte> video_header = H264Bits::header(),
      std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> early = {},
      std::optional<std::uint32_t> congestion_interval_ms = std::nullopt) {
    using libchiaking::net::protocol::remote_play::ActiveHandshakeState;
    ASSERT_NO_FATAL_FAILURE(start(std::move(control)));
    answer_handshake();
    ASSERT_TRUE(owner->pump_once());
    ASSERT_TRUE(owner->pump_once());
    std::uint32_t sequence = 0xa1b2c3d4;
    if (!early.empty()) {
      for (auto& [data_type, payload] : early)
        state->incoming.push_back(ActiveSessionWire::data_datagram(
            sequence++, data_type, std::move(payload), profile));
      ASSERT_TRUE(owner->pump_once());
      ASSERT_EQ(owner->state(), ActiveHandshakeState::awaiting_bang);
      early_feedback.clear();
      while (auto feedback = owner->take_feedback_event())
        early_feedback.push_back(std::move(*feedback));
    }
    auto info = ActiveSessionWire::stream_info(true);
    info.resolutions[0].video_header = std::move(video_header);
    info.congestion_control_interval = congestion_interval_ms;
    state->incoming.push_back(ActiveSessionWire::inbound_message(
        std::move(info), sequence++, profile));
    state->incoming.push_back(ActiveSessionWire::inbound_message(
        ActiveSessionWire::signed_bang(false), sequence++, profile));
    next_sequence = sequence;
    ASSERT_TRUE(owner->pump_once());
    ASSERT_TRUE(owner->pump_once());
    ASSERT_EQ(owner->state(), ActiveHandshakeState::ready);
    ASSERT_TRUE(owner->pump_once());
    ASSERT_NO_FATAL_FAILURE(install_sender());
    acknowledge_all();
    ASSERT_TRUE(owner->pump_once());
  }

  void install_sender() {
    const std::array secret{std::byte{1}, std::byte{2}, std::byte{3},
                            std::byte{4}};
    ASSERT_TRUE(sender.install_local(
        sender_provider,
        libchiaking::net::protocol::takion::CryptoMaterial{
            libchiaking::net::protocol::takion::SecretBytes{secret}}));
    ASSERT_TRUE(sender.set_local_key_position(0));
  }

  void protect(std::span<std::byte> wire, Family family) {
    const auto decoded =
        libchiaking::net::protocol::takion::PacketCodec::decode_plain_packet(
            wire, profile);
    ASSERT_TRUE(decoded);
    auto reservation = sender.protect_local(
        wire, family, profile,
        libchiaking::net::protocol::takion::PacketCryptoCoverage{
            0, wire.size(), wire.size(), 0},
        decoded.value().payload.size());
    ASSERT_TRUE(reservation);
    ASSERT_TRUE(reservation.value().commit());
  }

  void enqueue_av(Family family, std::vector<std::byte> payload,
                  std::uint8_t flags = 0) {
    namespace takion = libchiaking::net::protocol::takion;
    const takion::PlainPacket packet{
        takion::PacketType{static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(family) | flags)},
        std::nullopt, std::move(payload)};
    auto wire = takion::PacketCodec::encode_plain_packet(packet, profile);
    ASSERT_TRUE(wire);
    std::vector<std::byte> bytes(wire.value().begin(), wire.value().end());
    ASSERT_NO_FATAL_FAILURE(protect(bytes, family));
    state->incoming.push_back(std::move(bytes));
  }

  void enqueue_video(std::uint16_t frame, std::vector<std::byte> bytes,
                     std::uint16_t unit = 0, std::uint16_t total = 1,
                     std::uint16_t fec = 0) {
    std::vector<std::byte> payload(20);
    const auto packet_index = next_video_packet++;
    payload[0] = static_cast<std::byte>(packet_index >> 8U);
    payload[1] = static_cast<std::byte>(packet_index);
    payload[2] = static_cast<std::byte>(frame >> 8U);
    payload[3] = static_cast<std::byte>(frame);
    const std::uint32_t word =
        (std::uint32_t{unit} << 21U) |
        (std::uint32_t{static_cast<std::uint16_t>(total - 1U)} << 10U) | fec;
    payload[4] = static_cast<std::byte>(word >> 24U);
    payload[5] = static_cast<std::byte>(word >> 16U);
    payload[6] = static_cast<std::byte>(word >> 8U);
    payload[7] = static_cast<std::byte>(word);
    payload[8] = std::byte{1};
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{0});
    payload.insert(payload.end(), bytes.begin(), bytes.end());
    enqueue_av(Family::video, std::move(payload));
  }

  void enqueue_audio(std::uint16_t frame, std::vector<std::byte> bytes,
                     std::uint8_t unit, std::uint8_t total,
                     std::uint16_t fec_word, bool haptics = false) {
    std::vector<std::byte> payload(18);
    payload[2] = static_cast<std::byte>(frame >> 8U);
    payload[3] = static_cast<std::byte>(frame);
    payload[4] = static_cast<std::byte>(unit);
    payload[5] = static_cast<std::byte>(total - 1U);
    payload[6] = static_cast<std::byte>(fec_word >> 8U);
    payload[7] = static_cast<std::byte>(fec_word);
    payload[8] = std::byte{5};
    payload.insert(payload.end(), bytes.begin(), bytes.end());
    enqueue_av(Family::audio_and_haptics, std::move(payload),
               haptics ? 0x10 : 0);
  }

  void enqueue_data(std::uint8_t data_type, std::vector<std::byte> payload) {
    auto wire = ActiveSessionWire::data_datagram(next_sequence++, data_type,
                                                 std::move(payload), profile);
    ASSERT_NO_FATAL_FAILURE(protect(wire, Family::control));
    state->incoming.push_back(std::move(wire));
  }

  void enqueue_control(
      libchiaking::net::protocol::takion::ControlMessage message) {
    const auto encoded =
        libchiaking::net::protocol::takion::ControlCodec{}.encode(message);
    ASSERT_TRUE(encoded);
    enqueue_data(0, std::vector<std::byte>(encoded.value().begin(),
                                           encoded.value().end()));
  }

  auto controls_since(std::size_t start) const
      -> std::vector<std::vector<std::byte>> {
    std::vector<std::vector<std::byte>> controls;
    for (std::size_t index = start; index < state->sent.size(); ++index) {
      const auto data =
          ActiveSessionWire::data_chunk(state->sent[index], profile);
      if (!data || data->payload.size() < 9) continue;
      controls.emplace_back(data->payload.begin() + 9, data->payload.end());
    }
    return controls;
  }

  auto count_idr_requests(std::size_t start) const -> std::size_t {
    std::size_t count = 0;
    for (const auto& control : controls_since(start))
      if (control == std::vector<std::byte>{std::byte{8}, std::byte{25}})
        ++count;
    return count;
  }

  auto corrupt_frames(std::size_t start) const
      -> std::vector<std::pair<std::uint16_t, std::uint16_t>> {
    namespace takion = libchiaking::net::protocol::takion;
    std::vector<std::pair<std::uint16_t, std::uint16_t>> ranges;
    for (const auto& control : controls_since(start)) {
      if (control.size() < 4 || control[0] != std::byte{8} ||
          control[1] != std::byte{5} || control[2] != std::byte{0x32})
        continue;
      const auto message = takion::ControlCodec{}.decode(control);
      EXPECT_TRUE(message);
      if (!message) continue;
      const auto* corrupt = std::get_if<takion::CorruptFrame>(&message.value());
      EXPECT_NE(corrupt, nullptr);
      if (!corrupt) continue;
      ranges.emplace_back(corrupt->start, corrupt->end);
    }
    return ranges;
  }

  void acknowledge_all() {
    std::optional<std::uint32_t> sequence;
    for (const auto& wire : state->sent) {
      const auto data = ActiveSessionWire::data_chunk(wire, profile, false);
      if (!data || data->payload.size() < 4) continue;
      sequence = ActiveSessionWire::sequence_of(data->payload);
    }
    if (!sequence) return;
    state->incoming.push_back(inbound_acknowledgement(*sequence));
  }

  auto inbound_acknowledgement(std::uint32_t sequence) const -> OwnedDatagram {
    namespace takion = libchiaking::net::protocol::takion;
    return ActiveSessionWire::inbound_control(
        takion::DataAcknowledgementChunk{
            takion::ControlHeader{
                takion::VerificationTag{0x01020304}, {}, 0, 0},
            takion::DataAcknowledgement{sequence, 0x19000, {}, {}}},
        profile);
  }

  auto take_video_frames() -> std::vector<std::uint16_t> {
    std::vector<std::uint16_t> frames;
    while (auto event = owner->take_media_event())
      if (const auto* video = std::get_if<
              libchiaking::net::protocol::takion::client_event::EncodedVideo>(
              &*event))
        if (!video->profile_header) frames.push_back(video->frame_index);
    return frames;
  }

  auto controller_connections(std::size_t start) const
      -> std::vector<libchiaking::net::protocol::takion::ControllerConnection> {
    namespace takion = libchiaking::net::protocol::takion;
    std::vector<takion::ControllerConnection> result;
    for (const auto& control : controls_since(start)) {
      auto message = takion::ControlCodec{}.decode(control);
      if (!message) continue;
      if (const auto* connection =
              std::get_if<takion::ControllerConnection>(&message.value()))
        result.push_back(*connection);
    }
    return result;
  }

  auto feedback_history_payloads(std::size_t start) const
      -> std::vector<std::vector<std::byte>> {
    namespace takion = libchiaking::net::protocol::takion;
    std::vector<std::vector<std::byte>> result;
    for (std::size_t index = start; index < state->sent.size(); ++index) {
      auto plain =
          takion::PacketCodec::decode_plain_packet(state->sent[index], profile);
      if (plain &&
          plain.value().type.family() == takion::PacketFamily::feedback_history)
        result.emplace_back(plain.value().payload.begin(),
                            plain.value().payload.end());
    }
    return result;
  }

  auto congestion_packets() const -> std::vector<std::vector<std::byte>> {
    return ActiveSessionWire::congestion_packets(*state, profile);
  }

  std::uint32_t version;
  std::shared_ptr<ActiveSessionTransportState> state{
      std::make_shared<ActiveSessionTransportState>()};
  test_support::FakeClock clock{1'000};
  ActiveSessionRandom random;
  ActiveSessionCrypto crypto;
  ActiveSessionTakionCrypto takion_crypto;
  ActiveSessionTakionCrypto sender_provider{false};
  libchiaking::net::protocol::takion::CryptoContext sender{
      libchiaking::net::protocol::takion::CryptoPolicy::required};
  Profile profile;
  std::unique_ptr<libchiaking::net::protocol::remote_play::ActiveSession> owner;
  ActiveSessionKeyPair* pair{};
  std::uint32_t next_sequence{0xa1b2c3d6};
  std::vector<libchiaking::net::protocol::remote_play::ActiveFeedbackEvent>
      early_feedback;
  std::uint16_t next_video_packet{};
};

}  // namespace remote_play_test
