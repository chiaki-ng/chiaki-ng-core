#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/remote_play/stream/local_prepare.hpp"
#include "src/core/net/protocol/remote_play/stream/local_start.hpp"
#include "src/core/net/protocol/senkusha/senkusha.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "src/core/net/protocol/takion/versions.hpp"
#include "src/core/play/adapters/links/cloud/datacenter_survey.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace senkusha_test {

class SenkushaRandom final
    : public libchiaking::platform::SecureRandomProvider {
 public:
  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    if (fail)
      return libchiaking::unexpected(
          libchiaking::platform::PlatformError::io_failure);
    constexpr std::array values{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
        std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88},
    };
    for (std::size_t index = 0; index < output.size(); ++index)
      output[index] = values[index % values.size()];
    return {};
  }

  bool fail{};
};

struct SenkushaTransportState final {
  std::vector<libchiaking::net::transport::datagram::OwnedDatagram> sent;
  std::deque<libchiaking::net::transport::datagram::OwnedDatagram> incoming;
  std::size_t close_count{};
  std::size_t failed_receives{};
  std::optional<std::size_t> forced_send_size{};
  bool fail_send{};
  bool wrap_incoming{};
  std::vector<bool> dont_fragment;
  bool fail_dont_fragment{};
  std::function<void(
      const libchiaking::net::transport::datagram::OwnedDatagram&)>
      on_send;
  std::deque<std::function<void()>> on_receive;
  std::size_t blocked_sends{};
  std::vector<std::chrono::microseconds> writable_waits;
};

class SenkushaTransport final
    : public libchiaking::net::transport::datagram::DatagramTransport {
 public:
  using TransportError = libchiaking::net::transport::datagram::TransportError;
  using Datagram = libchiaking::net::transport::datagram::Datagram;

  explicit SenkushaTransport(std::shared_ptr<SenkushaTransportState> state)
      : state_(std::move(state)) {}

  auto send(std::span<const std::byte> datagram)
      -> libchiaking::Expected<std::size_t, TransportError> override {
    if (state_->fail_send)
      return libchiaking::unexpected(TransportError::io_failure);
    if (state_->blocked_sends) {
      --state_->blocked_sends;
      return libchiaking::unexpected(TransportError::would_block);
    }
    state_->sent.emplace_back(datagram.begin(), datagram.end());
    if (state_->on_send) state_->on_send(state_->sent.back());
    if (state_->forced_send_size) return *state_->forced_send_size;
    return datagram.size();
  }

  auto receive() -> libchiaking::Expected<std::optional<Datagram>,
                                          TransportError> override {
    if (state_->failed_receives) {
      --state_->failed_receives;
      return libchiaking::unexpected(TransportError::io_failure);
    }
    if (state_->incoming.empty()) return std::optional<Datagram>{};
    received_ = std::move(state_->incoming.front());
    state_->incoming.pop_front();
    if (!state_->on_receive.empty()) {
      auto hook = std::move(state_->on_receive.front());
      state_->on_receive.pop_front();
      hook();
    }
    if (state_->wrap_incoming)
      received_.insert(received_.begin(), {std::byte{0}, std::byte{0},
                                           std::byte{0}, std::byte{1}});
    return std::optional<Datagram>{received_};
  }

  auto close() -> libchiaking::Expected<void, TransportError> override {
    ++state_->close_count;
    return {};
  }

  auto wait_writable(std::chrono::microseconds timeout)
      -> libchiaking::Expected<bool, TransportError> override {
    state_->writable_waits.push_back(timeout);
    return true;
  }

  auto set_dont_fragment(bool enabled)
      -> libchiaking::Expected<void, TransportError> override {
    state_->dont_fragment.push_back(enabled);
    if (state_->fail_dont_fragment)
      return libchiaking::unexpected(TransportError::io_failure);
    return {};
  }

 private:
  std::shared_ptr<SenkushaTransportState> state_;
  libchiaking::net::transport::datagram::OwnedDatagram received_;
};

class SenkushaEventRecorder final
    : public libchiaking::net::protocol::senkusha::EventSink {
 public:
  void on_event(libchiaking::net::protocol::senkusha::Event event) override {
    events.push_back(std::move(event));
  }

  [[nodiscard]] auto failures() const -> std::size_t {
    return static_cast<std::size_t>(
        std::ranges::count_if(events, [](const auto& value) {
          return std::holds_alternative<
              libchiaking::net::protocol::senkusha::event::Failed>(value);
        }));
  }

  std::vector<libchiaking::net::protocol::senkusha::Event> events;
};

class SenkushaHarness {
 public:
  SenkushaHarness() = default;
  SenkushaHarness(const SenkushaHarness&) = delete;
  SenkushaHarness& operator=(const SenkushaHarness&) = delete;

  [[nodiscard]] auto transport() const -> std::unique_ptr<SenkushaTransport> {
    return std::make_unique<SenkushaTransport>(transport_state);
  }

  [[nodiscard]] auto state() const -> SenkushaTransportState& {
    return *transport_state;
  }

  test_support::FakeClock clock{1'000};
  SenkushaRandom random;
  SenkushaEventRecorder events;
  std::shared_ptr<SenkushaTransportState> transport_state{
      std::make_shared<SenkushaTransportState>()};
};

class SenkushaWire {
 public:
  using OwnedDatagram = libchiaking::net::transport::datagram::OwnedDatagram;

  static auto cookie() -> libchiaking::net::protocol::takion::HandshakeCookie {
    libchiaking::net::protocol::takion::HandshakeCookie result{};
    for (std::size_t index = 0; index < result.size(); ++index)
      result[index] = static_cast<std::byte>(index);
    return result;
  }

  static auto packet(libchiaking::net::protocol::takion::PacketFamily family,
                     std::span<const std::byte> payload,
                     std::uint32_t version = 7) -> OwnedDatagram {
    namespace takion = libchiaking::net::protocol::takion;
    const auto resolved = takion::resolve_takion_wire_profile(version);
    if (!resolved) {
      ADD_FAILURE() << "version " << version << " does not resolve";
      return {};
    }
    takion::PlainPacket value{
        takion::PacketType{static_cast<std::uint8_t>(family)},
        std::nullopt,
        {payload.begin(), payload.end()},
    };
    if (takion::PacketType::has_extended_header(
            static_cast<std::uint8_t>(family)) &&
        resolved->features().contains(takion::Feature::extended_packet_headers))
      value.extended_header = takion::ExtendedPacketHeader{1, 2};
    const auto plain =
        takion::PacketCodec::encode_plain_packet(value, *resolved);
    if (!plain) {
      ADD_FAILURE() << "the packet does not encode";
      return {};
    }
    const auto wrapped = takion::EnvelopeCodec::encode(plain.value().bytes(),
                                                       takion::PlainEnvelope{});
    if (!wrapped) {
      ADD_FAILURE() << "the envelope does not encode";
      return {};
    }
    return OwnedDatagram(wrapped.value().begin(), wrapped.value().end());
  }

  template <class Chunk>
  static auto control_packet(Chunk chunk, std::uint32_t version = 7)
      -> OwnedDatagram {
    namespace takion = libchiaking::net::protocol::takion;
    const auto encoded = takion::ControlChunkCodec::encode_packet(
        takion::ControlChunkPacket{std::move(chunk)});
    if (!encoded) {
      ADD_FAILURE() << "the control chunk does not encode";
      return {};
    }
    return packet(takion::PacketFamily::control, encoded.value().bytes(),
                  version);
  }

  static auto peer_config(std::uint32_t local_sequence,
                          std::uint32_t remote_sequence = 0x11223344)
      -> libchiaking::net::protocol::takion::ReliableChannelConfig {
    return {
        .initial_local_sequence = local_sequence,
        .initial_remote_sequence = remote_sequence,
        .maximum_message_size = 1024 * 1024,
        .maximum_fragment_size = 1200,
        .maximum_fragment_count = 1024,
        .retransmission_window_size = 256,
        .receive_reorder_size = 256,
        .retransmission_timeout_us = 100'000,
        .maximum_retries = 5,
    };
  }

  static auto peer(std::uint32_t local_sequence)
      -> libchiaking::net::protocol::takion::ReliableChannel {
    return libchiaking::net::protocol::takion::ReliableChannel::create(
               peer_config(local_sequence))
        .value();
  }

  static void queue_handshake(SenkushaTransportState& transport,
                              std::uint32_t version = 7) {
    namespace takion = libchiaking::net::protocol::takion;
    const takion::InitAcknowledgementValues acknowledgement{
        {takion::VerificationTag{0xa1b2c3d4},
         takion::AdvertisedReceiveWindow{
             takion::default_advertised_receive_window},
         takion::StreamCount{100}, takion::StreamCount{100},
         takion::InitialSequenceNumber{0x01020304}},
        cookie()};
    transport.incoming.push_back(control_packet(
        takion::InitAcknowledgementChunk{
            takion::ControlHeader{
                takion::VerificationTag{0x11223344}, {}, 0, 0},
            acknowledgement,
        },
        version));
    transport.incoming.push_back(control_packet(
        takion::CookieAcknowledgementChunk{
            takion::ControlHeader{
                takion::VerificationTag{0x11223344}, {}, 0, 0},
        },
        version));
  }

  static auto decode_data(const OwnedDatagram& datagram, std::uint32_t version)
      -> libchiaking::net::protocol::takion::DataChunk {
    namespace takion = libchiaking::net::protocol::takion;
    const auto unwrapped =
        takion::EnvelopeCodec::decode(datagram, takion::PlainEnvelope{});
    if (!unwrapped) {
      ADD_FAILURE() << "the envelope does not decode";
      return {};
    }
    const auto resolved = takion::resolve_takion_wire_profile(version);
    if (!resolved) {
      ADD_FAILURE() << "version " << version << " does not resolve";
      return {};
    }
    auto plain =
        takion::PacketCodec::decode_plain_packet(unwrapped.value(), *resolved);
    if (!plain) {
      ADD_FAILURE() << "the packet does not decode";
      return {};
    }
    auto chunk =
        takion::ControlChunkCodec::decode_packet(plain.value().payload);
    if (!chunk) {
      ADD_FAILURE() << "the control chunk does not decode";
      return {};
    }
    if (!std::holds_alternative<takion::DataChunk>(chunk.value())) {
      ADD_FAILURE() << "the control chunk is not data";
      return {};
    }
    return std::get<takion::DataChunk>(std::move(chunk).value());
  }

  static auto decode_outbound_control(const OwnedDatagram& datagram,
                                      std::uint32_t version,
                                      std::uint32_t initial_remote_sequence,
                                      std::uint16_t expected_channel = 1)
      -> std::optional<libchiaking::net::protocol::takion::ControlMessage> {
    namespace takion = libchiaking::net::protocol::takion;
    const auto data = decode_data(datagram, version);
    const auto end = (data.header.flags & 0x01U) != 0;
    auto peer_channel =
        takion::ReliableChannel::create(peer_config(0, initial_remote_sequence))
            .value();
    const auto received = peer_channel.receive(data.payload, end, 0);
    EXPECT_FALSE(received.anomaly);
    if (received.messages.size() != 1) {
      ADD_FAILURE() << received.messages.size() << " messages, not 1";
      return std::nullopt;
    }
    EXPECT_EQ(received.messages[0].channel, expected_channel);
    const takion::ControlCodec codec;
    auto decoded = codec.decode(received.messages[0].payload);
    if (!decoded) {
      ADD_FAILURE() << "the control message does not decode";
      return std::nullopt;
    }
    return std::move(decoded).value();
  }

  static auto outbound_reliable_sequence(const OwnedDatagram& datagram,
                                         std::uint32_t version)
      -> std::uint32_t {
    const auto data = decode_data(datagram, version);
    if (data.payload.size() < sizeof(std::uint32_t)) {
      ADD_FAILURE() << "the data payload holds no sequence";
      return 0;
    }
    return (std::to_integer<std::uint32_t>(data.payload[0]) << 24U) |
           (std::to_integer<std::uint32_t>(data.payload[1]) << 16U) |
           (std::to_integer<std::uint32_t>(data.payload[2]) << 8U) |
           std::to_integer<std::uint32_t>(data.payload[3]);
  }

  template <class Command>
  static auto senkusha_command(const OwnedDatagram& datagram,
                               std::uint32_t version = 7)
      -> std::optional<Command> {
    namespace takion = libchiaking::net::protocol::takion;
    const auto message = decode_outbound_control(
        datagram, version, outbound_reliable_sequence(datagram, version), 8);
    if (!message) return std::nullopt;
    const auto* senkusha_message = std::get_if<takion::Senkusha>(&*message);
    if (!senkusha_message) {
      ADD_FAILURE() << "the control message is not Senkusha";
      return std::nullopt;
    }
    const auto* command = std::get_if<Command>(&senkusha_message->command);
    if (!command) {
      ADD_FAILURE() << "the Senkusha command has another type";
      return std::nullopt;
    }
    return *command;
  }

  static void queue_reliable_ack(SenkushaTransportState& transport,
                                 std::uint32_t sequence,
                                 std::uint32_t version = 7) {
    namespace takion = libchiaking::net::protocol::takion;
    transport.incoming.push_back(control_packet(
        takion::DataAcknowledgementChunk{
            takion::ControlHeader{
                takion::VerificationTag{0x11223344}, {}, 0, 0},
            takion::DataAcknowledgement{sequence, 0x19000, {}, {}},
        },
        version));
  }

  static void queue_reliable_control(
      SenkushaTransportState& transport,
      libchiaking::net::protocol::takion::ReliableChannel& peer_channel,
      const libchiaking::net::protocol::takion::ControlMessage& message,
      std::uint32_t version, std::uint16_t channel = 1) {
    namespace takion = libchiaking::net::protocol::takion;
    const takion::ControlCodec codec;
    const auto encoded = codec.encode(message);
    if (!encoded) {
      ADD_FAILURE() << "the control message does not encode";
      return;
    }
    const auto sent = peer_channel.send(channel, takion::MessageDataType{0},
                                        encoded.value().bytes(), 0);
    if (!sent.has_value()) {
      ADD_FAILURE() << "the peer does not send";
      return;
    }
    if (sent->fragments.size() != 1) {
      ADD_FAILURE() << sent->fragments.size() << " fragments, not 1";
      return;
    }
    transport.incoming.push_back(control_packet(
        takion::DataChunk{
            takion::ControlHeader{
                takion::VerificationTag{0x11223344}, {}, 0, 1},
            std::vector<std::byte>(sent->fragments[0].payload.begin(),
                                   sent->fragments[0].payload.end()),
        },
        version));
  }

  static auto inbound_video_response(std::uint16_t frame_index)
      -> OwnedDatagram {
    namespace takion = libchiaking::net::protocol::takion;
    takion::SenkushaProbeValues values;
    values.video = true;
    values.frame_index = frame_index;
    values.units_in_frame_total = 1;
    values.codec = 0xff;
    const auto profile = takion::resolve_takion_wire_profile(7);
    if (!profile) {
      ADD_FAILURE() << "version 7 does not resolve";
      return {};
    }
    const auto encoded = takion::PacketCodec::encode_senkusha_probe(
        values, profile.value(), takion::PlainEnvelope{});
    if (!encoded) {
      ADD_FAILURE() << "the probe does not encode";
      return {};
    }
    return packet(takion::PacketFamily::video,
                  encoded.value().encoded_av_payload);
  }
};

class SenkushaClientRig : public ::testing::Test, protected SenkushaWire {
 protected:
  using Client = libchiaking::net::protocol::senkusha::Client;
  using ClientConfig = libchiaking::net::protocol::senkusha::ClientConfig;
  using State = libchiaking::net::protocol::senkusha::State;
  using Error = libchiaking::net::protocol::senkusha::Error;
  using TransportHandoffPolicy =
      libchiaking::net::protocol::senkusha::TransportHandoffPolicy;

  struct CandidateRoute {
    libchiaking::platform::SocketEndpoint local_endpoint;
    libchiaking::platform::SocketEndpoint remote_endpoint;
    libchiaking::net::protocol::remote_play::RouteKind kind{
        libchiaking::net::protocol::remote_play::RouteKind::remote_rudp};
    libchiaking::net::protocol::remote_play::DontFragmentPolicy dont_fragment{
        libchiaking::net::protocol::remote_play::DontFragmentPolicy::enabled};
  };

  static auto console_versions_are_the_defaults() -> bool {
    namespace takion = libchiaking::net::protocol::takion;
    return takion::takion_version_ps4 == 9 &&
           takion::takion_version_ps5 == 20 &&
           takion::takion_build_version_override == 0;
  }

  void SetUp() override {
    if (!console_versions_are_the_defaults())
      GTEST_SKIP() << "the scripted exchanges assume the default console "
                      "Takion versions";
  }

  static auto threading() -> libchiaking::platform::DefaultThreadingProvider& {
    return test_support::process_threading();
  }

  static auto client(SenkushaHarness& harness, ClientConfig config = {})
      -> std::unique_ptr<Client> {
    return std::make_unique<Client>(harness.transport(), harness.clock,
                                    threading(), harness.random, harness.events,
                                    std::move(config));
  }

  static auto single_probe_client(
      SenkushaHarness& harness,
      TransportHandoffPolicy handoff = TransportHandoffPolicy::return_route)
      -> std::unique_ptr<Client> {
    ClientConfig config;
    config.policy.probe.count = 1;
    config.profile.handoff = handoff;
    return client(harness, std::move(config));
  }

  static auto candidate_route() -> CandidateRoute {
    CandidateRoute route;
    route.local_endpoint.address =
        libchiaking::platform::IpAddress::loopback_ipv4();
    route.local_endpoint.port = 54321;
    route.remote_endpoint.address =
        libchiaking::platform::IpAddress::ipv4({192, 0, 2, 1});
    route.remote_endpoint.port = 12345;
    return route;
  }

  static auto single_probe_candidate_client(
      SenkushaHarness& harness, const CandidateRoute& route,
      TransportHandoffPolicy policy = TransportHandoffPolicy::return_route)
      -> std::unique_ptr<Client> {
    ClientConfig config;
    config.policy.probe.count = 1;
    config.profile.handoff = policy;
    libchiaking::net::protocol::remote_play::RouteCandidate candidate{
        harness.transport(), route.local_endpoint, route.remote_endpoint,
        route.kind,          route.dont_fragment,
    };
    return std::make_unique<Client>(std::move(candidate), harness.clock,
                                    threading(), harness.random, harness.events,
                                    std::move(config));
  }

  static auto cloud_config(libchiaking::play::PingService service)
      -> ClientConfig {
    return libchiaking::play::PingProfile{service}.senkusha_config();
  }

  static auto fixed_v9_big_echo_config() -> ClientConfig {
    namespace takion = libchiaking::net::protocol::takion;
    ClientConfig config;
    config.profile.bootstrap_version = 9;
    config.profile.profile_selection = takion::FixedTakionProfile{9};
    config.profile.big_response = libchiaking::net::protocol::senkusha::
        BigResponsePolicy::fragmented_big_echo;
    return config;
  }

  static auto local_bang(std::uint32_t version)
      -> libchiaking::net::protocol::takion::Bang {
    namespace takion = libchiaking::net::protocol::takion;
    return takion::Bang{version,
                        1,
                        false,
                        false,
                        takion::SharedSecretText{},
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                        takion::ProtocolRequestAck{version}};
  }

  static auto last_sequence(const SenkushaHarness& harness,
                            std::uint32_t version = 7) -> std::uint32_t {
    return outbound_reliable_sequence(harness.state().sent.back(), version);
  }

  static void reach_local_rtt_measurement(Client& client,
                                          SenkushaHarness& harness,
                                          bool stop_before_echo = false) {
    namespace takion = libchiaking::net::protocol::takion;
    ASSERT_TRUE(client.start());
    queue_handshake(harness.state());
    ASSERT_TRUE(client.pump_once());
    ASSERT_EQ(client.snapshot().state, State::negotiating_version);

    const auto request_datagram = harness.state().sent.back();
    const auto request_sequence =
        outbound_reliable_sequence(request_datagram, 7);
    const auto request_message =
        decode_outbound_control(request_datagram, 7, request_sequence);
    ASSERT_TRUE(request_message);
    ASSERT_TRUE(
        std::holds_alternative<takion::ProtocolRequest>(*request_message));
    queue_reliable_ack(harness.state(), request_sequence);

    auto peer_channel = peer(0xa1b2c3d4);
    queue_reliable_control(harness.state(), peer_channel,
                           takion::ProtocolRequestAck{20}, 7);
    ASSERT_TRUE(client.pump_once());
    ASSERT_EQ(client.snapshot().state, State::awaiting_bang);

    queue_reliable_control(harness.state(), peer_channel, local_bang(20), 7);
    ASSERT_TRUE(client.pump_once());
    ASSERT_EQ(client.snapshot().state, State::enabling_echo);
    if (stop_before_echo) return;
    ASSERT_TRUE(client.pump_once());

    const auto echo_datagram = harness.state().sent.back();
    const auto echo =
        senkusha_command<takion::SenkushaEchoCommand>(echo_datagram);
    ASSERT_TRUE(echo);
    ASSERT_TRUE(echo->state);

    queue_reliable_ack(harness.state(),
                       outbound_reliable_sequence(echo_datagram, 7));
    ASSERT_TRUE(client.pump_once());
    ASSERT_EQ(client.snapshot().state, State::measuring_rtt);
  }

  static void reach_zero_pong_echo_disable(Client& client,
                                           SenkushaHarness& harness) {
    namespace takion = libchiaking::net::protocol::takion;
    ASSERT_NO_FATAL_FAILURE(reach_local_rtt_measurement(client, harness));
    for (std::uint32_t index = 0; index < 10; ++index) {
      ASSERT_TRUE(client.pump_once()) << "probe " << index;
      harness.clock.now_us += 1'000'000;
      ASSERT_TRUE(client.pump_once()) << "probe " << index;
    }
    ASSERT_EQ(client.snapshot().state, State::disabling_echo);
    ASSERT_FALSE(client.snapshot().rtt_statistics);
    ASSERT_EQ(harness.events.failures(), 0U);

    const auto before = harness.state().sent.size();
    ASSERT_TRUE(client.pump_once());
    ASSERT_EQ(harness.state().sent.size(), before + 1);
    const auto echo = senkusha_command<takion::SenkushaEchoCommand>(
        harness.state().sent.back());
    ASSERT_TRUE(echo);
    ASSERT_FALSE(echo->state);
  }

  static void reach_inbound_mtu_measurement(Client& client,
                                            SenkushaHarness& harness) {
    namespace takion = libchiaking::net::protocol::takion;
    ASSERT_NO_FATAL_FAILURE(reach_local_rtt_measurement(client, harness));
    ASSERT_TRUE(client.pump_once());
    const auto probe = harness.state().sent.back();
    harness.clock.now_us += 100;
    harness.state().incoming.push_back(probe);
    ASSERT_TRUE(client.pump_once());
    ASSERT_EQ(client.snapshot().state, State::disabling_echo);

    ASSERT_TRUE(client.pump_once());
    const auto disable_datagram = harness.state().sent.back();
    const auto disable =
        senkusha_command<takion::SenkushaEchoCommand>(disable_datagram);
    ASSERT_TRUE(disable);
    ASSERT_FALSE(disable->state);

    queue_reliable_ack(harness.state(),
                       outbound_reliable_sequence(disable_datagram, 7));
    ASSERT_TRUE(client.pump_once());
    ASSERT_EQ(client.snapshot().state, State::measuring_inbound_mtu);
  }

  static void reach_starting_outbound_mtu(Client& client,
                                          SenkushaHarness& harness,
                                          bool flush_command = true) {
    ASSERT_NO_FATAL_FAILURE(reach_inbound_mtu_measurement(client, harness));
    ASSERT_TRUE(client.pump_once());
    harness.state().incoming.push_back(inbound_video_response(2));
    ASSERT_TRUE(client.pump_once());
    harness.state().incoming.push_back(inbound_video_response(1));
    ASSERT_TRUE(client.pump_once());
    ASSERT_EQ(client.snapshot().state, State::starting_outbound_mtu);
    if (flush_command) {
      ASSERT_TRUE(client.pump_once());
    }
    ASSERT_TRUE(client.snapshot().deadline_us.has_value());
  }

  static void reach_measuring_outbound_mtu(Client& client,
                                           SenkushaHarness& harness) {
    namespace takion = libchiaking::net::protocol::takion;
    ASSERT_NO_FATAL_FAILURE(reach_starting_outbound_mtu(client, harness));
    auto peer_channel = peer(0xa1b2c3d6);
    queue_reliable_control(
        harness.state(), peer_channel,
        takion::Senkusha{takion::SenkushaClientMtuCommand{1, 1454, true, 1454}},
        7, 8);
    ASSERT_TRUE(client.pump_once());
    ASSERT_EQ(client.snapshot().state, State::measuring_outbound_mtu);
    ASSERT_TRUE(client.pump_once());
    ASSERT_TRUE(client.snapshot().deadline_us.has_value());
  }

  static void reach_final_outbound_mtu_command(Client& client,
                                               SenkushaHarness& harness) {
    ASSERT_NO_FATAL_FAILURE(reach_measuring_outbound_mtu(client, harness));
    const auto probe = harness.state().sent.back();
    harness.clock.now_us += 50;
    harness.state().incoming.push_back(probe);
    ASSERT_TRUE(client.pump_once());
    ASSERT_EQ(client.snapshot().state, State::finishing_outbound_mtu);
    ASSERT_TRUE(client.pump_once());
  }

  static void complete(Client& client, SenkushaHarness& harness) {
    ASSERT_NO_FATAL_FAILURE(reach_final_outbound_mtu_command(client, harness));
    queue_reliable_ack(harness.state(), last_sequence(harness));
    ASSERT_TRUE(client.pump_once());
  }
};

}  // namespace senkusha_test
