#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/takion/client.hpp"
#include "src/core/net/protocol/takion/client/outbound.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/detail/crypto_context.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/ext_message_codec.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "tests/unit/core/net/protocol/takion/control_message_copy.hpp"

#include <iterator>
#include <type_traits>
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace takion_test {

namespace datagram = libchiaking::net::transport::datagram;
namespace takion = libchiaking::net::protocol::takion;

class ClientWire {
 public:
  using Keystream = std::pair<std::uint64_t, std::size_t>;

  static constexpr std::chrono::microseconds lean_drain{1'000'000};

  static auto fake_mac(std::uint64_t position, std::span<const std::byte> bytes)
      -> takion::Mac {
    std::uint32_t value = static_cast<std::uint32_t>(position) ^
                          static_cast<std::uint32_t>(position >> 32) ^
                          0x9e3779b9U;
    for (const auto byte : bytes)
      value =
          (value << 5U) ^ (value >> 2U) ^ std::to_integer<std::uint8_t>(byte);
    return {
        static_cast<std::byte>(value >> 24U),
        static_cast<std::byte>(value >> 16U),
        static_cast<std::byte>(value >> 8U),
        static_cast<std::byte>(value),
    };
  }

  static auto crypto_secret() -> takion::SecretBytes {
    const std::array values{std::byte{1}, std::byte{2}, std::byte{3},
                            std::byte{4}};
    return takion::SecretBytes{values};
  }

  static auto cookie() -> takion::HandshakeCookie {
    takion::HandshakeCookie result{};
    for (std::size_t index = 0; index < result.size(); ++index)
      result[index] = static_cast<std::byte>(index);
    return result;
  }

  static auto profile(std::uint32_t version) -> const takion::WireProfile& {
    static const auto profiles = [] {
      std::array<std::optional<takion::WireProfile>, 32> result{};
      for (std::uint32_t candidate = 0; candidate < result.size(); ++candidate)
        result[candidate] = takion::resolve_takion_wire_profile(candidate);
      return result;
    }();
    return profiles.at(version).value();
  }

  static auto packet(takion::PacketFamily family,
                     std::span<const std::byte> payload,
                     std::uint32_t version = 7,
                     takion::PacketEnvelope envelope = takion::PlainEnvelope{},
                     std::uint8_t type_flags = 0) -> datagram::OwnedDatagram {
    takion::PlainPacket value{
        takion::PacketType{static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(family) | type_flags)},
        std::nullopt,
        {payload.begin(), payload.end()},
    };
    if (version >= 20 && takion::PacketType::has_extended_header(
                             static_cast<std::uint8_t>(family)))
      value.extended_header = takion::ExtendedPacketHeader{1, 2};
    auto plain =
        takion::PacketCodec::encode_plain_packet(value, profile(version));
    EXPECT_TRUE(plain);
    auto wrapped =
        takion::EnvelopeCodec::encode(plain.value().bytes(), envelope);
    EXPECT_TRUE(wrapped);
    return datagram::OwnedDatagram(wrapped.value().begin(),
                                   wrapped.value().end());
  }

  template <class Chunk>
  static auto control_packet(
      Chunk chunk, std::uint32_t version = 7,
      takion::PacketEnvelope envelope = takion::PlainEnvelope{})
      -> datagram::OwnedDatagram {
    auto encoded = takion::ControlChunkCodec::encode_packet(
        takion::ControlChunkPacket{std::move(chunk)});
    EXPECT_TRUE(encoded);
    return packet(takion::PacketFamily::control, encoded.value().bytes(),
                  version, envelope);
  }

  static auto decode_data(
      const datagram::OwnedDatagram& datagram, std::uint32_t version = 7,
      takion::PacketEnvelope envelope = takion::PlainEnvelope{})
      -> takion::DataChunk {
    auto unwrapped = takion::EnvelopeCodec::decode(datagram, envelope);
    EXPECT_TRUE(unwrapped);
    auto plain = takion::PacketCodec::decode_plain_packet(unwrapped.value(),
                                                          profile(version));
    EXPECT_TRUE(plain);
    auto chunk =
        takion::ControlChunkCodec::decode_packet(plain.value().payload);
    EXPECT_TRUE(chunk);
    EXPECT_TRUE(std::holds_alternative<takion::DataChunk>(chunk.value()));
    return std::get<takion::DataChunk>(std::move(chunk).value());
  }

  static auto decode_ack(
      const datagram::OwnedDatagram& datagram, std::uint32_t version = 7,
      takion::PacketEnvelope envelope = takion::PlainEnvelope{})
      -> takion::DataAcknowledgementChunk {
    auto unwrapped = takion::EnvelopeCodec::decode(datagram, envelope);
    EXPECT_TRUE(unwrapped);
    auto plain = takion::PacketCodec::decode_plain_packet(unwrapped.value(),
                                                          profile(version));
    EXPECT_TRUE(plain);
    EXPECT_EQ(plain.value().type.family(), takion::PacketFamily::control);
    auto chunk =
        takion::ControlChunkCodec::decode_packet(plain.value().payload);
    EXPECT_TRUE(chunk);
    EXPECT_TRUE(std::holds_alternative<takion::DataAcknowledgementChunk>(
        chunk.value()));
    return std::get<takion::DataAcknowledgementChunk>(std::move(chunk).value());
  }

  static auto data_sequence(const takion::DataChunk& chunk) -> std::uint32_t {
    return (std::to_integer<std::uint32_t>(chunk.payload[0]) << 24U) |
           (std::to_integer<std::uint32_t>(chunk.payload[1]) << 16U) |
           (std::to_integer<std::uint32_t>(chunk.payload[2]) << 8U) |
           std::to_integer<std::uint32_t>(chunk.payload[3]);
  }

  static auto data_sequence(const datagram::OwnedDatagram& sent)
      -> std::uint32_t {
    return data_sequence(decode_data(sent));
  }

  static auto acknowledgement_of(std::uint32_t sequence)
      -> datagram::OwnedDatagram {
    return control_packet(takion::DataAcknowledgementChunk{
        takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
        takion::DataAcknowledgement{sequence, 0x19000, {}, {}}});
  }

  struct OutboundControl {
    std::vector<std::byte> payload;
    std::uint16_t channel{};
    takion::MessageDataType data_type{};
    std::vector<std::uint32_t> sequences;
  };

  static auto decode_outbound_control(
      const std::vector<datagram::OwnedDatagram>& datagrams, std::size_t start,
      std::size_t count, std::uint32_t version = 7,
      takion::PacketEnvelope envelope = takion::PlainEnvelope{},
      std::uint16_t channel = 1,
      std::uint32_t initial_remote_sequence = 0x11223344) -> OutboundControl {
    EXPECT_LE(start, datagrams.size());
    EXPECT_LE(count, datagrams.size() - start);
    takion::ReliableChannel peer =
        takion::ReliableChannel::create(
            {
                .initial_local_sequence = 0,
                .initial_remote_sequence = initial_remote_sequence,
                .maximum_message_size =
                    std::numeric_limits<std::size_t>::max() / 2,
                .maximum_fragment_size =
                    std::numeric_limits<std::size_t>::max() / 2,
                .maximum_fragment_count =
                    std::numeric_limits<std::size_t>::max() / 2,
                .retransmission_window_size = 256,
                .receive_reorder_size = 256,
                .retransmission_timeout_us = 100'000,
                .maximum_retries = 5,
            })
            .value();

    OutboundControl result;
    for (std::size_t index = start; index < start + count; ++index) {
      auto unwrapped =
          takion::EnvelopeCodec::decode(datagrams[index], envelope);
      EXPECT_TRUE(unwrapped) << "datagram " << index;
      auto plain = takion::PacketCodec::decode_plain_packet(unwrapped.value(),
                                                            profile(version));
      EXPECT_TRUE(plain) << "datagram " << index;
      EXPECT_EQ(plain.value().type.family(), takion::PacketFamily::control)
          << "datagram " << index;
      EXPECT_FALSE(plain.value().extended_header.has_value())
          << "datagram " << index;
      auto chunk = decode_data(datagrams[index], version, envelope);
      EXPECT_EQ(chunk.header.verification_tag.value, 0xa1b2c3d4U)
          << "datagram " << index;
      EXPECT_TRUE(chunk.header.flags == 0 || chunk.header.flags == 1)
          << "datagram " << index;
      const bool end = (chunk.header.flags & 0x01U) != 0;
      result.sequences.push_back(data_sequence(chunk));
      auto received = peer.receive(chunk.payload, end, 0);
      EXPECT_FALSE(received.anomaly) << "datagram " << index;
      for (auto& message : received.messages) {
        EXPECT_EQ(message.channel, channel) << "datagram " << index;
        result.channel = message.channel;
        result.data_type = message.data_type;
        result.payload.insert(result.payload.end(), message.payload.begin(),
                              message.payload.end());
      }
    }
    EXPECT_EQ(result.sequences.size(), count);
    return result;
  }

  static auto peer_channel_config(std::uint16_t,
                                  std::uint32_t initial_local_sequence,
                                  std::size_t maximum_fragment_size = 1200,
                                  std::size_t maximum_fragment_count = 1024)
      -> takion::ReliableChannelConfig {
    return takion::ReliableChannelConfig{
        .initial_local_sequence = initial_local_sequence,
        .initial_remote_sequence = 0x11223344,
        .maximum_message_size = 1024 * 1024,
        .maximum_fragment_size = maximum_fragment_size,
        .maximum_fragment_count = maximum_fragment_count,
        .retransmission_window_size = 256,
        .receive_reorder_size = 256,
        .retransmission_timeout_us = 100'000,
        .maximum_retries = 5,
    };
  }

  static auto supported_control_routes()
      -> std::vector<takion::ControlMessage> {
    const std::vector<std::byte> header{std::byte{0x01}, std::byte{0x02}};
    takion::ControlMessage routes[]{
        takion::Heartbeat{},
        takion::Disconnect{
            "route disconnect",
            takion::DisconnectEventCode{"event", "name", "description"}},
        takion::Big{9, takion::SharedSecretText{"session-key"},
                    takion::SharedSecretText{"launch-spec"}, header, header,
                    header},
        takion::Bang{
            9, 77, true, true, takion::SharedSecretText{"session-key"},
            takion::DisconnectEventCode{"event", "name", "description"},
            "server-version", header, header, takion::ProtocolRequestAck{20}},
        takion::StreamInfo{{{1920, 1080, header}, {1280, 720, header}},
                           header,
                           1,
                           2,
                           3,
                           4,
                           {{0, header, true}, {1, header, false}},
                           0xb8,
                           0,
                           500},
        takion::StreamInfoAcknowledgement{},
        takion::Senkusha{takion::SenkushaEchoCommand{true}},
        takion::Senkusha{takion::SenkushaMtuCommand{1, 2, 3, 4, 5, 6}},
        takion::Senkusha{
            takion::SenkushaBandwidthCommand{1, 2, 3, 4, 5, 6, 7, true, 0.5}},
        takion::Senkusha{takion::SenkushaClientMtuCommand{1, 2, true, 4}},
        takion::Senkusha{takion::UnknownSenkushaCommand{99}},
        takion::ConnectionQuality{1, 2, 0.5f, true, 0.25, 7},
        takion::CorruptFrame{1, 2},
        takion::ControllerConnection{
            1, true, takion::ControllerConnectionType::dualsense},
        takion::ControllerConnection{
            2, false,
            takion::ControllerConnectionTypeValue{
                takion::UnknownControllerConnectionType{99}}},
        takion::IdrRequest{},
        takion::ProtocolRequest{{7, 12, 20}},
        takion::ProtocolRequestAck{7},
    };
    return {std::make_move_iterator(std::begin(routes)),
            std::make_move_iterator(std::end(routes))};
  }

  static auto version20_config(
      takion::CryptoPolicy policy = takion::CryptoPolicy::disabled)
      -> takion::ClientConfig {
    takion::ClientConfig config;
    config.bootstrap_version = 20;
    config.profile_selection = takion::FixedTakionProfile{20};
    config.crypto_policy = policy;
    return config;
  }

  static auto typed_bytes(std::size_t size, std::uint8_t seed)
      -> std::vector<std::byte> {
    std::vector<std::byte> data(size);
    for (std::size_t index = 0; index < size; ++index)
      data[index] = static_cast<std::byte>((index + seed) & 0xffU);
    return data;
  }

  static auto sent_video_frame_reports(
      const std::vector<datagram::OwnedDatagram>& datagrams,
      std::uint32_t version) -> std::vector<takion::VideoFrameMetricsReport> {
    std::vector<takion::VideoFrameMetricsReport> reports;
    for (const auto& datagram : datagrams) {
      const auto plain =
          takion::PacketCodec::decode_plain_packet(datagram, profile(version));
      if (!plain ||
          plain.value().type.family() != takion::PacketFamily::extended_message)
        continue;
      const auto body = std::span{plain.value().payload}.subspan(12);
      if (auto report =
              takion::ExtMessageCodec::decode_video_frame_metrics(body))
        reports.push_back(*report);
    }
    return reports;
  }

  static auto sent_tos_reports(
      const std::vector<datagram::OwnedDatagram>& datagrams,
      std::uint32_t version) -> std::vector<takion::ReceivedTosMetricsReport> {
    std::vector<takion::ReceivedTosMetricsReport> reports;
    for (const auto& datagram : datagrams) {
      const auto plain =
          takion::PacketCodec::decode_plain_packet(datagram, profile(version));
      if (!plain ||
          plain.value().type.family() != takion::PacketFamily::extended_message)
        continue;
      const auto body = std::span{plain.value().payload}.subspan(12);
      if (auto report =
              takion::ExtMessageCodec::decode_received_tos_metrics(body)) {
        const std::vector<std::byte> encrypted_trailer{
            std::byte{1}, std::byte{0}, std::byte{0}, std::byte{1}};
        EXPECT_TRUE(std::ranges::equal(
            std::span{plain.value().payload}.subspan(8, 4), encrypted_trailer));
        reports.push_back(*report);
      }
    }
    return reports;
  }

  static auto authenticated_video(std::uint32_t version, std::uint16_t frame,
                                  std::uint32_t sender_us,
                                  std::uint32_t position)
      -> datagram::OwnedDatagram {
    std::vector<std::byte> payload(20, std::byte{0});
    payload[3] = static_cast<std::byte>(frame);
    payload[8] = std::byte{0x3c};
    payload[13] = static_cast<std::byte>(position >> 24U);
    payload[14] = static_cast<std::byte>(position >> 16U);
    payload[15] = static_cast<std::byte>(position >> 8U);
    payload[16] = static_cast<std::byte>(position);
    payload.insert(payload.end(),
                   {std::byte{0}, std::byte{0}, std::byte{1}, std::byte{2}});
    takion::PlainPacket value{takion::PacketType{0x02},
                              takion::ExtendedPacketHeader{sender_us, frame},
                              payload};
    auto wire =
        takion::PacketCodec::encode_plain_packet(value, profile(version))
            .value();
    const auto mac = fake_mac(position, wire.bytes());
    std::copy(mac.begin(), mac.end(), wire.begin() + 18);
    return datagram::OwnedDatagram(wire.begin(), wire.end());
  }
};

class ClientRandom final : public libchiaking::platform::SecureRandomProvider {
 public:
  bool fail{};

  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    if (fail)
      return libchiaking::unexpected(
          libchiaking::platform::PlatformError::io_failure);
    const std::array values{
        std::byte{0x11},
        std::byte{0x22},
        std::byte{0x33},
        std::byte{0x44},
    };
    EXPECT_EQ(output.size(), values.size());
    std::ranges::copy(values, output.begin());
    return {};
  }
};

struct ClientCryptoState {
  std::uint64_t last_position{};
  std::byte encrypt_mask{};
  std::vector<ClientWire::Keystream> keystreams;
  std::vector<std::uint64_t> authentications;
  std::byte decrypt_mask{};
  std::size_t decrypt_calls{};
  std::vector<std::byte> decrypted_input;
};

class ClientCryptoLocal final : public takion::CryptoDirection {
 public:
  explicit ClientCryptoLocal(std::shared_ptr<ClientCryptoState> state)
      : state_(std::move(state)) {}

  auto apply_keystream(std::uint64_t position, std::span<const std::byte> input,
                       std::span<std::byte> bytes)
      -> takion::Expected<void, takion::CryptoError> override {
    if (input.data() != bytes.data()) std::ranges::copy(input, bytes.begin());
    state_->last_position = position;
    if (!bytes.empty()) state_->keystreams.emplace_back(position, bytes.size());
    for (auto& byte : bytes) byte ^= state_->encrypt_mask;
    return {};
  }

  auto authenticate(std::uint64_t position, std::span<const std::byte> bytes)
      -> takion::Expected<takion::Mac, takion::CryptoError> override {
    state_->last_position = position;
    state_->authentications.push_back(position);
    return ClientWire::fake_mac(position, bytes);
  }

 private:
  std::shared_ptr<ClientCryptoState> state_;
};

class ClientCryptoRemote final : public takion::CryptoDirection {
 public:
  explicit ClientCryptoRemote(std::shared_ptr<ClientCryptoState> state)
      : state_(std::move(state)) {}

  auto authenticate(std::uint64_t position, std::span<const std::byte> bytes)
      -> takion::Expected<takion::Mac, takion::CryptoError> override {
    state_->last_position = position;
    return ClientWire::fake_mac(position, bytes);
  }

  auto apply_keystream(std::uint64_t position, std::span<const std::byte> input,
                       std::span<std::byte> bytes)
      -> takion::Expected<void, takion::CryptoError> override {
    if (input.data() != bytes.data()) std::ranges::copy(input, bytes.begin());
    state_->last_position = position;
    ++state_->decrypt_calls;
    state_->decrypted_input.assign(bytes.begin(), bytes.end());
    for (auto& byte : bytes) byte ^= state_->decrypt_mask;
    return {};
  }

 private:
  std::shared_ptr<ClientCryptoState> state_;
};

class ClientCryptoProvider final : public takion::CryptoProvider {
 public:
  ClientCryptoProvider(std::shared_ptr<ClientCryptoState> local,
                       std::shared_ptr<ClientCryptoState> remote)
      : local_(std::move(local)), remote_(std::move(remote)) {}

  auto create_local(takion::CryptoMaterial material)
      -> takion::Expected<std::unique_ptr<takion::CryptoDirection>,
                          takion::CryptoError> override {
    EXPECT_FALSE(material.ecdh_secret().empty());
    return std::unique_ptr<takion::CryptoDirection>{
        std::make_unique<ClientCryptoLocal>(local_)};
  }

  auto create_remote(takion::CryptoMaterial material)
      -> takion::Expected<std::unique_ptr<takion::CryptoDirection>,
                          takion::CryptoError> override {
    EXPECT_FALSE(material.ecdh_secret().empty());
    return std::unique_ptr<takion::CryptoDirection>{
        std::make_unique<ClientCryptoRemote>(remote_)};
  }

 private:
  std::shared_ptr<ClientCryptoState> local_;
  std::shared_ptr<ClientCryptoState> remote_;
};

class ClientTransport final : public datagram::DatagramTransport {
 public:
  auto send(std::span<const std::byte> datagram)
      -> takion::Expected<std::size_t, datagram::TransportError> override {
    if (fail_send) return takion::unexpected(send_error);
    if (blocked_sends) {
      --blocked_sends;
      return takion::unexpected(datagram::TransportError::would_block);
    }
    sent.emplace_back(datagram.begin(), datagram.end());
    if (forced_send_size) return *forced_send_size;
    return datagram.size();
  }

  auto enable_traffic_class()
      -> takion::Expected<void, datagram::TransportError> override {
    ++traffic_class_enables;
    if (!traffic_class_supported)
      return takion::unexpected(datagram::TransportError::io_failure);
    return {};
  }

  auto last_traffic_class() const noexcept
      -> std::optional<std::uint8_t> override {
    return traffic_class_enables != 0 && traffic_class_supported
               ? incoming_traffic_class
               : std::nullopt;
  }

  auto receive() -> takion::Expected<std::optional<datagram::Datagram>,
                                     datagram::TransportError> override {
    ++receive_calls;
    if (failed_receives) {
      --failed_receives;
      return takion::unexpected(datagram::TransportError::io_failure);
    }
    if (incoming.empty()) return std::optional<datagram::Datagram>{};
    received = std::move(incoming.front());
    incoming.pop_front();
    return std::optional<datagram::Datagram>{received};
  }

  auto pollable() const noexcept
      -> const libchiaking::platform::Pollable* override {
    return polled;
  }

  auto readiness_exact() const noexcept -> bool override { return exact; }

  auto wait_readable(std::chrono::nanoseconds timeout,
                     const libchiaking::platform::StopToken&)
      -> takion::Expected<bool, datagram::TransportError> override {
    if (!waits) return takion::unexpected(datagram::TransportError::io_failure);
    readable_waits.push_back(timeout);
    clock->now_us += static_cast<std::uint64_t>(
        std::chrono::ceil<std::chrono::microseconds>(timeout).count());
    if (on_wait) on_wait(*this);
    return !incoming.empty();
  }

  auto close() -> takion::Expected<void, datagram::TransportError> override {
    ++close_count;
    if (fail_close)
      return takion::unexpected(datagram::TransportError::io_failure);
    return {};
  }

  auto wait_writable(std::chrono::microseconds timeout)
      -> takion::Expected<bool, datagram::TransportError> override {
    writable_waits.push_back(timeout);
    return true;
  }

  std::vector<datagram::OwnedDatagram> sent;
  std::deque<datagram::OwnedDatagram> incoming;
  datagram::OwnedDatagram received;
  std::optional<std::uint8_t> incoming_traffic_class;
  std::size_t traffic_class_enables{};
  bool traffic_class_supported{true};
  std::size_t close_count{};
  std::optional<std::size_t> forced_send_size;
  bool fail_send{};
  std::size_t failed_receives{};
  datagram::TransportError send_error{datagram::TransportError::io_failure};
  bool fail_close{};
  test_support::FakeClock* clock{};
  std::size_t blocked_sends{};
  std::vector<std::chrono::microseconds> writable_waits;
  std::size_t receive_calls{};
  const libchiaking::platform::Pollable* polled{};
  bool exact{};
  bool waits{};
  std::vector<std::chrono::nanoseconds> readable_waits;
  std::function<void(ClientTransport&)> on_wait;
};

class ClientPollable final : public libchiaking::platform::Pollable {
 public:
  auto owner() const noexcept
      -> const libchiaking::platform::NetworkProvider& override {
    std::abort();
  }
};

class ClientEvents final : public takion::EventSink {
 public:
  void on_event(takion::Event event) override {
    if (hook) hook(event);
    events.push_back(std::move(event));
  }

  template <class T>
  auto count() const -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        events,
        [](const auto& event) { return std::holds_alternative<T>(event); }));
  }

  template <class T>
  auto values() const -> std::vector<T> {
    std::vector<T> result;
    for (const auto& event : events) {
      const auto* value = std::get_if<T>(&event);
      if (value == nullptr) continue;
      if constexpr (std::is_same_v<T, takion::client_event::ControlReceived>)
        result.push_back(
            T{copy_of(value->message), value->unsupported_payload});
      else
        result.push_back(*value);
    }
    return result;
  }

  std::vector<takion::Event> events;
  std::function<void(const takion::Event&)> hook;
};

class ClientHarness {
 public:
  explicit ClientHarness(takion::ClientConfig config = {}) {
    auto owned = std::make_unique<ClientTransport>();
    transport = owned.get();
    transport->clock = &clock;
    client = std::make_unique<takion::Client>(
        std::move(owned), clock, test_support::process_threading(), random,
        events, std::move(config));
  }

  void connect(bool duplicate_init_ack = false, std::uint32_t version = 7,
               takion::PacketEnvelope envelope = takion::PlainEnvelope{}) {
    ASSERT_TRUE(client->start());
    ASSERT_EQ(transport->sent.size(), 1U);
    const auto unwrapped =
        takion::EnvelopeCodec::decode(transport->sent.front(), envelope);
    ASSERT_TRUE(unwrapped);
    const auto plain = takion::PacketCodec::decode_plain_packet(
        unwrapped.value(), ClientWire::profile(version));
    ASSERT_TRUE(plain);
    const auto first =
        takion::ControlChunkCodec::decode_packet(plain.value().payload);
    ASSERT_TRUE(first);
    ASSERT_TRUE(std::holds_alternative<takion::InitChunk>(first.value()));
    const auto& init = std::get<takion::InitChunk>(first.value());
    ASSERT_EQ(init.header.verification_tag.value, 0U);
    ASSERT_EQ(init.values.tag.value, 0x11223344U);
    ASSERT_EQ(init.values.initial_sequence_number.value, init.values.tag.value);
    const takion::InitAcknowledgementValues acknowledgement{
        {takion::VerificationTag{0xa1b2c3d4},
         takion::AdvertisedReceiveWindow{peer_receive_window},
         takion::StreamCount{100}, takion::StreamCount{100},
         takion::InitialSequenceNumber{0x01020304}},
        ClientWire::cookie()};
    const auto init_ack = ClientWire::control_packet(
        takion::InitAcknowledgementChunk{
            takion::ControlHeader{
                takion::VerificationTag{0x11223344}, {}, 0, 0},
            acknowledgement,
        },
        version, envelope);
    transport->incoming.push_back(init_ack);
    if (duplicate_init_ack) transport->incoming.push_back(init_ack);
    transport->incoming.push_back(ClientWire::control_packet(
        takion::CookieAcknowledgementChunk{takion::ControlHeader{
            takion::VerificationTag{0x11223344}, {}, 0, 0}},
        version, envelope));
    ASSERT_TRUE(client->pump_once());
    ASSERT_EQ(client->snapshot().state, takion::ClientState::connected);
  }

  test_support::FakeClock clock{0};
  ClientRandom random;
  ClientEvents events;
  ClientTransport* transport{};
  std::unique_ptr<takion::Client> client;
  std::uint32_t peer_receive_window{takion::default_advertised_receive_window};
};

class ClientRig : public ::testing::Test, protected ClientWire {
 protected:
  static inline libchiaking::platform::DefaultThreadingProvider& threading =
      test_support::process_threading();

  static void install_fake_crypto(ClientHarness& fixture,
                                  ClientCryptoProvider& provider,
                                  std::uint64_t local_position = 0,
                                  std::uint64_t remote_position = 0) {
    ASSERT_TRUE(fixture.client->post(takion::InstallCrypto{
        &provider, takion::CryptoMaterial{crypto_secret()},
        takion::CryptoMaterial{crypto_secret()}, local_position,
        remote_position}));
    ASSERT_TRUE(fixture.client->pump_once());
  }

  static void inject_reliable(ClientHarness& fixture,
                              takion::ReliableChannel& peer,
                              const std::vector<std::byte>& message,
                              std::uint32_t version = 20) {
    auto sent = peer.send(takion::control_channel_id,
                          takion::MessageDataType{0}, message, 0);
    ASSERT_TRUE(sent.has_value());
    ASSERT_EQ(sent->fragments.size(), 1U);
    fixture.transport->incoming.push_back(control_packet(
        takion::DataChunk{
            takion::ControlHeader{
                takion::VerificationTag{0x11223344}, {}, 0, 1},
            std::vector<std::byte>(sent->fragments[0].payload.begin(),
                                   sent->fragments[0].payload.end())},
        version));
  }

  static auto sent_generic_controls(const ClientHarness& fixture,
                                    std::size_t from)
      -> std::vector<takion::GenericDataControl> {
    std::vector<takion::GenericDataControl> controls;
    for (std::size_t index = from; index < fixture.transport->sent.size();
         ++index) {
      auto plain = takion::PacketCodec::decode_plain_packet(
          fixture.transport->sent[index], profile(20));
      EXPECT_TRUE(plain) << "datagram " << index;
      if (plain.value().type.family() != takion::PacketFamily::control)
        continue;
      auto chunk =
          takion::ControlChunkCodec::decode_packet(plain.value().payload);
      if (!chunk || !std::holds_alternative<takion::DataChunk>(chunk.value()))
        continue;
      const auto sequence =
          data_sequence(std::get<takion::DataChunk>(chunk.value()));
      const auto decoded = decode_outbound_control(
          fixture.transport->sent, index, 1, 20, takion::PlainEnvelope{},
          takion::control_channel_id, sequence);
      auto control =
          takion::TypedMessageCodec::decode_generic_data_direct_message(
              decoded.payload);
      EXPECT_TRUE(control) << "datagram " << index;
      EXPECT_TRUE(control.value()) << "datagram " << index;
      controls.push_back(*control.value());
    }
    return controls;
  }

  static auto sent_generic_chunks(const ClientHarness& fixture,
                                  std::size_t from)
      -> std::vector<takion::GenericDataChunk> {
    std::vector<takion::GenericDataChunk> chunks;
    for (std::size_t index = from; index < fixture.transport->sent.size();
         ++index) {
      auto plain = takion::PacketCodec::decode_plain_packet(
          fixture.transport->sent[index], profile(20));
      EXPECT_TRUE(plain) << "datagram " << index;
      if (plain.value().type.family() != takion::PacketFamily::generic_data)
        continue;
      EXPECT_TRUE(plain.value().extended_header.has_value())
          << "datagram " << index;
      auto chunk = takion::TypedMessageCodec::decode_generic_data_chunk(
          plain.value(), profile(20));
      EXPECT_TRUE(chunk) << "datagram " << index;
      chunks.push_back(std::move(chunk).value());
    }
    return chunks;
  }
};

}  // namespace takion_test
