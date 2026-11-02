#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/platform/net/poller.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/detail/handshake_types.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/cloud/loopback_rig.hpp"
#include "tests/unit/core/play/cleared_launch.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::SignedInPlayer;
namespace {

namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;
using cloud_loopback::GkScript;
using cloud_loopback::HandshakeCrypto;
using cloud_loopback::LoopbackSink;
using Stage = remote_play::FailureStage;
using StreamState = remote_play::StreamState;
using QuitReason = remote_play::QuitReason;

class CloudLinkHandshake : public cloud_loopback::LoopbackTest {
 protected:
  void start(CloudService service) {
    http_.legacy_cloud_stream = service == CloudService::legacy_cloud_stream;
    auto peer = network_.sockets().create_udp_socket(
        platform::IpAddress::loopback_ipv4());
    ASSERT_TRUE(peer);
    ASSERT_TRUE(peer.value()->bind({platform::IpAddress::loopback_ipv4(), 0}));
    const auto peer_endpoint = peer.value()->local_endpoint();
    ASSERT_TRUE(peer_endpoint);
    ASSERT_NE(peer_endpoint.value().port, 0);
    http_.allocation_port = peer_endpoint.value().port;
    peer_ = std::move(peer.value());

    auto& cleared = cleared_.emplace(service);
    play::CloudLinkConfiguration config;
    config.user = SignedInPlayer::fresh();
    cleared.arm(config);
    config.time_zone = "UTC+00:00";
    config.datacenter = play::DatacenterPreference::named(
        play::DatacenterName::parse("dc1").value());
    config.prior_datacenters = {prior_row()};
    auto session = play::CloudLinkConfiguration::link(
        std::move(config), factory_, cleared.seams());
    ASSERT_TRUE(session);
    session_ = std::move(session.value());
    session_->set_event_sink(sink_);
    ASSERT_TRUE(session_->start());

    auto poller = network_.pollers().create_poller();
    ASSERT_TRUE(poller);
    poller_ = std::move(poller.value());
    profile_ = takion::resolve_takion_wire_profile(
        service == CloudService::cloud_stream
            ? takion::takion_version_cloud_stream
            : takion::takion_version_legacy_cloud_stream);
    ASSERT_TRUE(profile_);
  }

  void expect_launch_reports() {
    EXPECT_EQ(http_.calls, http_.legacy_cloud_stream ? 13U : 11U);
    EXPECT_EQ(session_->state(), StreamState::negotiating_stream);
    EXPECT_EQ(sink_.events, 2U);
    EXPECT_EQ(sink_.last_state, StreamState::negotiating_stream);
    std::vector<std::string> expected_progress{
        "Checking PlayStation Plus Premium and title streaming"};
    std::vector<Stage> expected_stages{Stage::cloud_launch_check};
    if (http_.legacy_cloud_stream) {
      expected_progress.insert(
          expected_progress.end(),
          {"Authorizing - Step 4 of 5", "Creating Session - Step 5 of 5"});
      expected_stages.insert(expected_stages.end(),
                             {Stage::cloud_title, Stage::cloud_title});
    }
    EXPECT_EQ(cleared_->clearance.clears, 1);
    EXPECT_TRUE(cleared_->clearance.remembered.empty());
    const std::vector<std::pair<std::string, Stage>> gk_steps{
        {"Getting Client IDs - Step 1 of 10", Stage::cloud_client_ids},
        {"Getting Configuration - Step 2 of 10", Stage::cloud_config},
        {"Starting Session - Step 3 of 10", Stage::cloud_start},
        {"Getting Tokens - Step 4 of 10", Stage::cloud_oauth},
        {"Getting Server Tokens - Step 5 of 10", Stage::cloud_oauth},
        {"Authorizing Session - Step 6 of 10", Stage::cloud_authorize},
        {"Locking Session - Step 7 of 10", Stage::cloud_lock},
        {"Getting Datacenters - Step 8 of 10", Stage::cloud_datacenter},
        {"Selecting Datacenter (dc1) - Step 9 of 10", Stage::cloud_datacenter},
        {"Allocating Streaming Slot - Step 10 of 10", Stage::cloud_allocate},
    };
    for (const auto& [text, stage] : gk_steps) {
      expected_progress.push_back(text);
      expected_stages.push_back(stage);
    }
    EXPECT_EQ(sink_.progress_texts, expected_progress);
    EXPECT_EQ(sink_.progress_stages, expected_stages);
    EXPECT_EQ(sink_.datacenter_events, 1U);
    ASSERT_EQ(sink_.datacenters.size(), 2U);
    EXPECT_EQ(sink_.datacenters[0].name, "dc1");
    EXPECT_EQ(sink_.datacenters[0].rtt_ms, 40);
    EXPECT_EQ(sink_.datacenters[0].mtu_in, 1454);
    EXPECT_EQ(sink_.datacenters[0].mtu_out, 1254);
    EXPECT_EQ(sink_.datacenters[0].public_ip, "192.0.2.1");
    EXPECT_TRUE(sink_.datacenters[0].measured);
    EXPECT_EQ(sink_.datacenters[1], prior_row());
    ASSERT_EQ(sink_.selections.size(), 1U);
    EXPECT_EQ(sink_.selections[0].name, "dc1");
    EXPECT_TRUE(sink_.selections[0].forced);
    EXPECT_EQ(sink_.selections[0].rtt_ms, 0);
  }

  void handshake() {
    const auto waited =
        poller_->wait(requests(), std::chrono::seconds{1}, running_);
    ASSERT_TRUE(waited);
    bool got_packet = false;
    for (const auto& readiness : waited.value().ready) {
      if (readable(readiness)) got_packet = true;
    }
    ASSERT_TRUE(got_packet);
    std::array<std::byte, 2048> buffer{};
    const auto datagram = peer_->receive_from(buffer);
    ASSERT_TRUE(datagram);
    source_ = datagram.value().source;
    ASSERT_NE(source_.port, 0);
    const auto init_packet = decode(
        std::span<const std::byte>(buffer.data(), datagram.value().size));
    ASSERT_TRUE(init_packet);
    ASSERT_TRUE(std::holds_alternative<takion::InitChunk>(*init_packet));
    init_ = std::get<takion::InitChunk>(*init_packet);
    EXPECT_EQ(init_.header.verification_tag.value, 0U);
    EXPECT_NE(init_.values.tag.value, 0U);
    EXPECT_EQ(init_.values.initial_sequence_number.value,
              init_.values.tag.value);

    for (std::size_t index = 0; index < cookie_.size(); ++index)
      cookie_[index] = static_cast<std::byte>(index ^ 0x5a);
    acknowledgement_ = takion::InitAcknowledgementValues{
        {takion::VerificationTag{0xa1b2c3d4},
         takion::AdvertisedReceiveWindow{
             takion::default_advertised_receive_window},
         takion::StreamCount{100}, takion::StreamCount{100},
         takion::InitialSequenceNumber{0x01020304}},
        cookie_};
    ASSERT_NO_FATAL_FAILURE(
        send(takion::ControlChunkPacket{takion::InitAcknowledgementChunk{
            takion::ControlHeader{init_.values.tag, {}, 0, 0},
            acknowledgement_}}));

    const auto cookie = await<takion::CookieChunk>();
    ASSERT_TRUE(cookie);
    EXPECT_EQ(cookie->header.verification_tag.value,
              acknowledgement_.init.tag.value);
    EXPECT_EQ(cookie->cookie, cookie_);

    ASSERT_NO_FATAL_FAILURE(
        send(takion::ControlChunkPacket{takion::CookieAcknowledgementChunk{
            takion::ControlHeader{init_.values.tag, {}, 0, 0}}}));

    const auto data = await<takion::DataChunk>();
    ASSERT_TRUE(data);
    EXPECT_EQ(data->header.verification_tag.value,
              acknowledgement_.init.tag.value);
    EXPECT_FALSE(data->payload.empty());

    EXPECT_TRUE(session_->pump_once());
    EXPECT_EQ(session_->state(), StreamState::negotiating_stream);
  }

  void disconnect(std::string_view text) {
    const auto encoded = takion::ControlCodec{}.encode(
        takion::Disconnect{std::string{text}, std::nullopt});
    ASSERT_TRUE(encoded);
    takion::ReliableChannel console =
        takion::ReliableChannel::create(
            {
                .initial_local_sequence = acknowledgement_.init.tag.value,
                .initial_remote_sequence =
                    init_.values.initial_sequence_number.value,
                .maximum_message_size = 1024 * 1024,
                .maximum_fragment_size = 1200,
                .maximum_fragment_count = 1024,
                .retransmission_window_size = 16,
                .receive_reorder_size = 256,
                .retransmission_timeout_us = 100'000,
                .maximum_retries = 5,
            })
            .value();
    const auto message =
        console.send(1, takion::MessageDataType{0}, encoded.value(), 0);
    ASSERT_TRUE(message);
    ASSERT_EQ(message->fragments.size(), 1U);
    ASSERT_NO_FATAL_FAILURE(send(takion::ControlChunkPacket{takion::DataChunk{
        takion::ControlHeader{init_.values.tag, {}, 0, 1},
        std::vector<std::byte>(message->fragments[0].payload.begin(),
                               message->fragments[0].payload.end())}}));
    const auto ended = LiveProviders::steady_us() + 1'000'000;
    while (session_->state() == StreamState::negotiating_stream &&
           LiveProviders::steady_us() < ended) {
      static_cast<void>(session_->pump_once());
      LiveProviders::nap(std::chrono::milliseconds{1});
    }
  }

  auto session() -> play::Link& { return *session_; }
  auto sink() const -> const LoopbackSink& { return sink_; }
  auto close_peer() -> bool { return static_cast<bool>(peer_->close()); }

 private:
  auto requests() const -> std::array<platform::PollRequest, 1> {
    return {{{peer_.get(), 0, platform::PollEvent::readable}}};
  }

  static auto readable(const auto& readiness) -> bool {
    return readiness.tag == 0 &&
           (readiness.ready & platform::PollEvent::readable) !=
               platform::PollEvent::none;
  }

  auto decode(std::span<const std::byte> bytes)
      -> std::optional<takion::ControlChunkPacket> {
    const auto unwrapped =
        takion::EnvelopeCodec::decode(bytes, takion::PsnEnvelope{7});
    EXPECT_TRUE(unwrapped);
    if (!unwrapped) return std::nullopt;
    const auto plain = takion::PacketCodec::decode_plain_packet(
        unwrapped.value(), profile_.value());
    EXPECT_TRUE(plain);
    if (!plain) return std::nullopt;
    const auto packet =
        takion::ControlChunkCodec::decode_packet(plain.value().payload);
    EXPECT_TRUE(packet);
    if (!packet) return std::nullopt;
    return packet.value();
  }

  template <class Chunk>
  auto await() -> std::optional<Chunk> {
    const auto deadline = LiveProviders::steady_us() + 1'000'000;
    while (LiveProviders::steady_us() < deadline) {
      EXPECT_TRUE(session_->pump_once());
      const auto ready =
          poller_->wait(requests(), std::chrono::milliseconds{10}, running_);
      EXPECT_TRUE(ready);
      if (!ready) continue;
      for (const auto& readiness : ready.value().ready) {
        if (!readable(readiness)) continue;
        std::array<std::byte, 2048> buffer{};
        const auto datagram = peer_->receive_from(buffer);
        EXPECT_TRUE(datagram);
        if (!datagram) continue;
        const auto packet = decode(
            std::span<const std::byte>(buffer.data(), datagram.value().size));
        if (packet && std::holds_alternative<Chunk>(*packet))
          return std::get<Chunk>(*packet);
      }
    }
    return std::nullopt;
  }

  void send(const takion::ControlChunkPacket& packet) {
    const auto encoded = takion::ControlChunkCodec::encode_packet(packet);
    ASSERT_TRUE(encoded);
    const auto plain = takion::PacketCodec::encode_plain_packet(
        takion::PlainPacket{takion::PacketType{static_cast<std::uint8_t>(
                                takion::PacketFamily::control)},
                            std::nullopt,
                            {encoded.value().begin(), encoded.value().end()}},
        profile_.value());
    ASSERT_TRUE(plain);
    const auto sent = peer_->send_to(plain.value(), source_);
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent.value(), plain.value().size());
  }

  platform::DefaultNetworkProvider network_{LiveProviders::threading(), LiveProviders::steady()};
  HandshakeCrypto crypto_;
  GkScript http_;
  platform::DefaultPlatformFactory factory_{platform::PlatformProviders{
      .network = &network_, .crypto = &crypto_, .http = &http_}};
  std::optional<ClearedLaunch> cleared_;
  std::unique_ptr<platform::UdpSocket> peer_;
  LoopbackSink sink_;
  std::unique_ptr<play::Link> session_;
  std::unique_ptr<platform::Poller> poller_;
  std::optional<takion::WireProfile> profile_;
  const platform::StopToken running_;
  platform::SocketEndpoint source_{};
  takion::InitChunk init_{};
  takion::HandshakeCookie cookie_{};
  takion::InitAcknowledgementValues acknowledgement_{};
};

TEST_F(CloudLinkHandshake, ReportsEveryLaunchStepOnCloudStream) {
  ASSERT_NO_FATAL_FAILURE(start(CloudService::cloud_stream));
  expect_launch_reports();
}

TEST_F(CloudLinkHandshake, ReportsEveryLaunchStepOnLegacyCloudStream) {
  ASSERT_NO_FATAL_FAILURE(start(CloudService::legacy_cloud_stream));
  expect_launch_reports();
}

class CloudLinkHandshakeStop : public CloudLinkHandshake {
 protected:
  void handshake_and_stop(CloudService service) {
    ASSERT_NO_FATAL_FAILURE(start(service));
    ASSERT_NO_FATAL_FAILURE(handshake());
    const auto before_stop = sink().events;
    EXPECT_TRUE(session().stop());
    EXPECT_EQ(session().state(), StreamState::stopped);
    EXPECT_TRUE(session().stop());
    EXPECT_EQ(sink().events, before_stop + 1);
    EXPECT_EQ(sink().last_state, StreamState::stopped);
    EXPECT_EQ(sink().last_quit, QuitReason::stopped);
    EXPECT_EQ(session().quit().reason, QuitReason::stopped);
    EXPECT_TRUE(close_peer());
  }
};

TEST_F(CloudLinkHandshakeStop, StopsCleanlyAfterTheHandshakeOnCloudStream) {
  handshake_and_stop(CloudService::cloud_stream);
}

TEST_F(CloudLinkHandshakeStop,
       StopsCleanlyAfterTheHandshakeOnLegacyCloudStream) {
  handshake_and_stop(CloudService::legacy_cloud_stream);
}

TEST_F(CloudLinkHandshake, EndsWithoutAFailureWhenTheServerShutsDown) {
  ASSERT_NO_FATAL_FAILURE(start(CloudService::cloud_stream));
  ASSERT_NO_FATAL_FAILURE(handshake());
  ASSERT_NO_FATAL_FAILURE(disconnect("Server shutting down"));
  EXPECT_EQ(session().state(), StreamState::stopped);
  EXPECT_EQ(sink().failures, 0U);
  EXPECT_EQ(sink().last_state, StreamState::stopped);
  EXPECT_EQ(sink().last_quit, QuitReason::stream_connection_remote_shutdown);
  EXPECT_FALSE(remote_play::quit_reason_is_error(sink().last_quit));
  EXPECT_FALSE(session().failure());
  EXPECT_EQ(session().quit().reason,
            QuitReason::stream_connection_remote_shutdown);
  EXPECT_EQ(session().quit().text, "Server shutting down");
  EXPECT_TRUE(close_peer());
}

TEST_F(CloudLinkHandshake, FailsWhenTheConsoleEndsTheSession) {
  ASSERT_NO_FATAL_FAILURE(start(CloudService::cloud_stream));
  ASSERT_NO_FATAL_FAILURE(handshake());
  ASSERT_NO_FATAL_FAILURE(disconnect("Remote Play session ended"));
  EXPECT_EQ(session().state(), StreamState::failed);
  EXPECT_EQ(sink().failures, 1U);
  ASSERT_TRUE(session().failure());
  const auto quit = session().failure()->quit();
  EXPECT_EQ(quit.reason, QuitReason::stream_connection_remote_disconnected);
  EXPECT_EQ(quit.text, "Remote Play session ended");
  EXPECT_EQ(session().quit().reason,
            QuitReason::stream_connection_remote_disconnected);
  EXPECT_EQ(session().quit().text, "Remote Play session ended");
  EXPECT_TRUE(close_peer());
}

}  // namespace
}  // namespace play_test
