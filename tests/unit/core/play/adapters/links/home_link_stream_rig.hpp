#pragma once

#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/platform_crypto_provider.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "tests/unit/core/play/adapters/links/home_link_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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

namespace play_test::home_links {

struct StreamConsole {
  std::string_view name;
  Target target;
  std::uint32_t version;
  std::size_t ecdh_public_size;
  std::size_t ecdh_secret_size;
};

class StreamExchange {
 public:
  explicit StreamExchange(const StreamConsole& console)
      : console_(console), ps5_(console.target == Target::ps5_1) {}

  StreamExchange(const StreamExchange&) = delete;
  auto operator=(const StreamExchange&) -> StreamExchange& = delete;

  void connect() {
    auto& sockets = providers_.network().sockets();
    listener_ = Loopback::listener(sockets);
    ASSERT_NE(listener_, nullptr);
    const auto ctrl_endpoint = listener_->local_endpoint();
    ASSERT_TRUE(ctrl_endpoint);
    udp_peer_ = Loopback::udp_peer(sockets);
    ASSERT_NE(udp_peer_, nullptr);
    const auto udp_endpoint = udp_peer_->local_endpoint();
    ASSERT_TRUE(udp_endpoint);
    active_udp_peer_ = Loopback::udp_peer(sockets);
    ASSERT_NE(active_udp_peer_, nullptr);
    const auto active_udp_endpoint = active_udp_peer_->local_endpoint();
    ASSERT_TRUE(active_udp_endpoint);

    play::HomeLinkConfiguration config;
    config.console.target = console_.target;
    config.host = "127.0.0.1";
    config.ctrl_endpoint = {platform::IpAddress::any_ipv4(),
                            ctrl_endpoint.value().port};
    config.route.local = {platform::IpAddress::loopback_ipv4(), 0};
    config.route.remote = {platform::IpAddress::any_ipv4(),
                           udp_endpoint.value().port};
    config.route.dont_fragment = remote_play::DontFragmentPolicy::disabled;
    config.active_stream_port = active_udp_endpoint.value().port;
    config.video.width = 1920;
    config.video.height = 1080;
    config.video.bitrate_kbps = 15000;
    config.codec =
        ps5_ ? remote_play::VideoCodec::h264 : remote_play::VideoCodec::h265;
    auto connected =
        play::HomeLinkConfiguration::link(std::move(config), providers_);
    ASSERT_TRUE(connected);
    link_ = std::move(connected.value());
    link_->set_event_sink(sink_);
    ASSERT_TRUE(link_->start());
    EXPECT_TRUE(link_->send_controller_state(controller_));

    deadline_ = test_support::LiveProviders::now() + std::chrono::seconds{3};
    SessionInitConsole init{*link_, *listener_, deadline_};
    ASSERT_NO_FATAL_FAILURE(init.open_ctrl(
        ps5_ ? "/sie/ps5/rp/sess/init" : "/sie/ps4/rp/sess/init", &clock_));
    ctrl_ = init.take_ctrl();
    ASSERT_NE(ctrl_, nullptr);
    const auto reply = CtrlWire::reply_with_session_id(
        ps5_ ? "HTTP/1.1 200 OK\r\nRP-Server-Type: "
               "AgAAAAAAAAAAAAAAAAAAAA==\r\n\r\n"
             : "HTTP/1.1 200 OK\r\nRP-Server-Type: "
               "AAAAAAAAAAAAAAAAAAAAAA==\r\n\r\n");
    std::size_t sent{};
    while (sent < reply.size() &&
           test_support::LiveProviders::now() < deadline_) {
      const auto chunk =
          ctrl_->send(std::span<const std::byte>(reply).subspan(sent));
      if (chunk)
        sent += chunk.value();
      else
        ASSERT_EQ(chunk.error(), NetError::would_block);
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_EQ(sent, reply.size());
    while (link_->state() != StreamState::negotiating_stream &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_EQ(link_->state(), StreamState::negotiating_stream);
  }

  void calibrate() {
    ASSERT_NO_FATAL_FAILURE(open_calibration());
    ASSERT_NO_FATAL_FAILURE(negotiate_calibration());
    ASSERT_NO_FATAL_FAILURE(echo_probes());
    ASSERT_NO_FATAL_FAILURE(measure_mtu());
  }

  void open_active_stream() {
    while (!active_init_ && test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      const auto next = active_udp_peer_->receive_from(udp_bytes_);
      if (!next) {
        ASSERT_EQ(next.error(), NetError::would_block);
        test_support::LiveProviders::spin_pause();
        continue;
      }
      const auto packet = plain(received(next.value().size), *profile_);
      ASSERT_TRUE(packet);
      if (packet->type.family() != takion::PacketFamily::control) continue;
      const auto control =
          takion::ControlChunkCodec::decode_packet(packet->payload);
      ASSERT_TRUE(control);
      if (const auto* next_init =
              std::get_if<takion::InitChunk>(&control.value()))
        active_init_ = *next_init;
      if (active_init_) active_source_ = next.value().source;
    }
    ASSERT_TRUE(active_init_);
    ASSERT_TRUE(active_source_);
    active_version_ =
        ps5_ ? takion::takion_version_ps5 : takion::takion_version_ps4;
    active_profile_ = takion::resolve_takion_wire_profile(active_version_);
    ASSERT_TRUE(active_profile_);
    ASSERT_NO_FATAL_FAILURE(
        send_active_control(takion::InitAcknowledgementChunk{
            takion::ControlHeader{active_init_->values.tag, {}, 0, 0},
            takion::InitAcknowledgementValues{
                {takion::VerificationTag{0xb1b2c3d4},
                 takion::AdvertisedReceiveWindow{
                     takion::default_advertised_receive_window},
                 takion::StreamCount{100}, takion::StreamCount{100},
                 takion::InitialSequenceNumber{0x01020304}},
                cookie()}}));
    ASSERT_NO_FATAL_FAILURE(
        send_active_control(takion::CookieAcknowledgementChunk{
            takion::ControlHeader{active_init_->values.tag, {}, 0, 0}}));
    bool saw_big = false;
    std::optional<takion::ReliableChannel> active_decoder;
    while (!saw_big && test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      const auto next = active_udp_peer_->receive_from(udp_bytes_);
      if (!next) {
        ASSERT_EQ(next.error(), NetError::would_block);
        test_support::LiveProviders::spin_pause();
        continue;
      }
      const auto packet = plain(received(next.value().size), *active_profile_);
      ASSERT_TRUE(packet);
      if (packet->type.family() != takion::PacketFamily::control) continue;
      const auto control =
          takion::ControlChunkCodec::decode_packet(packet->payload);
      ASSERT_TRUE(control);
      const auto* data = std::get_if<takion::DataChunk>(&control.value());
      if (!data) continue;
      ASSERT_GE(data->payload.size(), 4U);
      if (!active_decoder)
        active_decoder.emplace(channel(0, sequence_of(data->payload), 2048));
      EXPECT_TRUE(data->header.flags == 0 || data->header.flags == 1)
          << static_cast<int>(data->header.flags);
      const auto decoded = active_decoder->receive(
          data->payload, (data->header.flags & 0x01U) != 0, 0);
      ASSERT_FALSE(decoded.anomaly);
      if (decoded.messages.empty()) continue;
      ASSERT_EQ(decoded.messages.size(), 1U);
      if (decoded.messages[0].channel != 1) continue;
      const auto message = codec_.decode(decoded.messages[0].payload);
      ASSERT_TRUE(message);
      if (const auto* big = std::get_if<takion::Big>(&message.value())) {
        EXPECT_EQ(big->session_key.view(), std::string(24, 'A'));
        ASSERT_TRUE(big->ecdh_pub_key);
        EXPECT_EQ(big->ecdh_pub_key->size(), console_.ecdh_public_size);
        saw_big = true;
      }
    }
    ASSERT_TRUE(saw_big);
  }

  void deliver_rumble() {
    active_peer_.emplace(channel(
        0xb1b2c3d4, active_init_->values.initial_sequence_number.value));
    ASSERT_NO_FATAL_FAILURE(send_rumble());
    while (sink_.rumble_before_streaming() == 0 &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      test_support::LiveProviders::spin_pause();
    }
    EXPECT_EQ(sink_.rumble_before_streaming(), 1U);
    EXPECT_EQ(link_->state(), StreamState::negotiating_stream);
    EXPECT_EQ(sink_.media_events(), 1U);
    sink_.forget_media();
  }

  void start_streaming() {
    takion::StreamInfo info;
    info.audio_header.assign(14, std::byte{});
    info.audio_header[0] = std::byte{2};
    info.audio_header[1] = std::byte{16};
    info.resolutions.push_back(takion::StreamInfoResolution{
        1280, 720, {std::byte{0x01}, std::byte{0x02}}});
    ASSERT_NO_FATAL_FAILURE(send_active_message(info));
    takion::Bang bang;
    bang.server_version = active_version_;
    bang.token = 1;
    bang.encrypted_key_accepted = true;
    bang.version_accepted = true;
    bang.session_key = libchiaking::net::protocol::takion::SharedSecretText{
        std::string(24, 'A')};
    bang.ecdh_pub_key = std::vector<std::byte>(console_.ecdh_public_size);
    bang.ecdh_pub_key->at(0) = std::byte{0x04};
    for (std::size_t index = 1; index < bang.ecdh_pub_key->size(); ++index)
      bang.ecdh_pub_key->at(index) = static_cast<std::byte>(index - 1);
    bang.ecdh_sig = std::vector<std::byte>(32);
    bang.protocol_ack = takion::ProtocolRequestAck{active_version_};
    ASSERT_NO_FATAL_FAILURE(send_active_message(std::move(bang)));
    while (link_->state() != StreamState::streaming &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_EQ(link_->state(), StreamState::streaming);
  }

  void press_cross() {
    clock_.release();
    controller_.buttons =
        static_cast<std::uint32_t>(remote_play::ControllerButton::cross);
    EXPECT_TRUE(link_->send_controller_state(controller_));
    auto invalid_controller = controller_;
    invalid_controller.controller_id = 4;
    const auto rejected = link_->send_controller_state(invalid_controller);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error(), StreamError::controller_rejected);
    EXPECT_EQ(link_->state(), StreamState::streaming);
    bool saw_history = false;
    bool saw_pressed_state = false;
    while ((!saw_history || !saw_pressed_state) &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      const auto next = active_udp_peer_->receive_from(udp_bytes_);
      if (!next) {
        ASSERT_EQ(next.error(), NetError::would_block);
        test_support::LiveProviders::spin_pause();
        continue;
      }
      const auto packet = plain(received(next.value().size), *active_profile_);
      ASSERT_TRUE(packet);
      const auto& payload = packet->payload;
      const auto family = packet->type.family();
      if (family == takion::PacketFamily::feedback_history) {
        ASSERT_GE(payload.size(), 14U);
        if (!saw_history) {
          EXPECT_EQ(payload[0], std::byte{0});
          EXPECT_EQ(payload[1], std::byte{0});
        }
        EXPECT_EQ(payload[2], std::byte{0});
        EXPECT_EQ(payload[11], std::byte{0x80});
        EXPECT_EQ(payload[12], std::byte{0x88});
        EXPECT_EQ(payload[13], std::byte{0xff});
        saw_history = true;
      } else if (family == takion::PacketFamily::feedback_state &&
                 payload.size() >= 3 && payload[0] == std::byte{0} &&
                 payload[1] == std::byte{1} && payload[2] == std::byte{0}) {
        saw_pressed_state = true;
      }
    }
    EXPECT_TRUE(saw_history);
    EXPECT_TRUE(saw_pressed_state);
  }

  void recover_video() {
    EXPECT_TRUE(link_->report_video_loss(258, 259));
    bool saw_recovery = false;
    while (!saw_recovery && test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      const auto datagram = active_udp_peer_->receive_from(udp_bytes_);
      if (!datagram) {
        ASSERT_EQ(datagram.error(), NetError::would_block);
        test_support::LiveProviders::spin_pause();
        continue;
      }
      const auto packet = takion::PacketCodec::decode_plain_packet(
          received(datagram.value().size), *active_profile_);
      ASSERT_TRUE(packet);
      if (packet.value().type.family() != takion::PacketFamily::control)
        continue;
      const auto chunk =
          takion::ControlChunkCodec::decode_packet(packet.value().payload);
      ASSERT_TRUE(chunk);
      const auto* data = std::get_if<takion::DataChunk>(&chunk.value());
      if (!data || data->payload.size() < 9 || data->payload[5] != std::byte{2})
        continue;
      const std::vector<std::byte> expected{
          std::byte{0x08}, std::byte{0x05}, std::byte{0x32}, std::byte{0x06},
          std::byte{0x08}, std::byte{0x82}, std::byte{0x02}, std::byte{0x10},
          std::byte{0x83}, std::byte{0x02}};
      EXPECT_EQ(data->header.flags, 1);
      ASSERT_EQ(data->payload.size(), 9 + expected.size());
      EXPECT_EQ(data->payload[4], std::byte{});
      EXPECT_EQ(data->payload[8], std::byte{});
      EXPECT_TRUE(std::equal(expected.begin(), expected.end(),
                             data->payload.begin() + 9));
      saw_recovery = true;
    }
    ASSERT_TRUE(saw_recovery);
    EXPECT_TRUE(link_->request_idr());
    EXPECT_TRUE(await_idr());
    EXPECT_TRUE(link_->report_video_loss(5, 4));
    EXPECT_TRUE(await_idr());
  }

  void send_microphone() {
    std::array<std::byte, 40> opus{};
    const auto short_opus =
        link_->send_microphone(std::span<const std::byte>{opus}.first(39));
    ASSERT_FALSE(short_opus);
    EXPECT_EQ(short_opus.error(), StreamError::invalid_configuration);
    for (std::uint8_t value = 1; value <= 3; ++value) {
      opus.fill(static_cast<std::byte>(value));
      EXPECT_TRUE(link_->send_microphone(opus));
    }
    const std::size_t microphone_header = ps5_ ? 20 : 19;
    bool saw_microphone = false;
    while (!saw_microphone && test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      const auto next = active_udp_peer_->receive_from(udp_bytes_);
      if (!next) {
        ASSERT_EQ(next.error(), NetError::would_block);
        test_support::LiveProviders::spin_pause();
        continue;
      }
      const auto packet = takion::PacketCodec::decode_plain_packet(
          received(next.value().size), *active_profile_);
      ASSERT_TRUE(packet);
      if (packet.value().type.family() !=
          takion::PacketFamily::audio_and_haptics)
        continue;
      const auto& payload = packet.value().payload;
      ASSERT_EQ(payload.size(), microphone_header - 1 + 120);
      EXPECT_EQ(payload[0], std::byte{0});
      EXPECT_EQ(payload[1], std::byte{0});
      EXPECT_EQ(payload[2], std::byte{0});
      EXPECT_EQ(payload[3], std::byte{1});
      saw_microphone = true;
    }
    ASSERT_TRUE(saw_microphone);

    EXPECT_TRUE(link_->connect_microphone());
    EXPECT_TRUE(link_->set_microphone_muted(false));
    std::vector<std::byte> ctrl_stream;
    std::vector<std::pair<std::uint16_t, std::size_t>> ctrl_frames;
    const auto has_microphone_frames = [&] {
      for (std::size_t index = 0; index + 1 < ctrl_frames.size(); ++index)
        if (ctrl_frames[index] ==
                std::pair<std::uint16_t, std::size_t>{0x30, 2} &&
            ctrl_frames[index + 1] ==
                std::pair<std::uint16_t, std::size_t>{0x36, 4})
          return true;
      return false;
    };
    while (!has_microphone_frames() &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      std::array<std::byte, 1024> buffer{};
      const auto part = ctrl_->receive(buffer);
      if (!part) {
        ASSERT_EQ(part.error(), NetError::would_block);
        test_support::LiveProviders::spin_pause();
        continue;
      }
      ctrl_stream.insert(
          ctrl_stream.end(), buffer.begin(),
          buffer.begin() + static_cast<std::ptrdiff_t>(part.value()));
      const std::string_view text(
          reinterpret_cast<const char*>(ctrl_stream.data()),
          ctrl_stream.size());
      const auto header_end = text.find("\r\n\r\n");
      if (header_end == std::string_view::npos) continue;
      ctrl_frames.clear();
      auto frames =
          std::span<const std::byte>(ctrl_stream).subspan(header_end + 4);
      while (!frames.empty()) {
        const auto decoded = remote_play::CtrlCodec::decode_frame(frames);
        if (!decoded) break;
        ctrl_frames.emplace_back(decoded.value().type,
                                 decoded.value().payload.size());
        frames = frames.subspan(decoded.value().consumed);
      }
    }
    EXPECT_TRUE(has_microphone_frames());
    EXPECT_EQ(link_->state(), StreamState::streaming);
  }

  void deliver_protected_media() {
    EXPECT_EQ(sink_.media_events(), 0U);
    std::vector<std::byte> secret(console_.ecdh_secret_size);
    for (std::size_t index = 0; index < secret.size(); ++index)
      secret[index] = static_cast<std::byte>(index + 1);
    const std::array<std::byte, 16> handshake_key{};
    ASSERT_TRUE(sender_.install_local(
        sender_provider_,
        takion::CryptoMaterial{takion::SecretBytes{handshake_key},
                               takion::SecretBytes{secret}, 3}));
    ASSERT_TRUE(sender_.set_local_key_position(0));
    video_type_ = takion::PacketType{
        static_cast<std::uint8_t>(takion::PacketFamily::video)};
    video_header_ = active_profile_->uses_extended_header(video_type_)
                        ? std::optional<takion::ExtendedPacketHeader>{{1, 2}}
                        : std::nullopt;
    std::vector<std::byte> video_payload(32);
    video_payload.back() = std::byte{0x51};
    const takion::PlainPacket protected_video{video_type_, video_header_,
                                              std::move(video_payload)};
    auto video_wire = takion::PacketCodec::encode_plain_packet(
        protected_video, *active_profile_);
    ASSERT_TRUE(video_wire);
    EXPECT_EQ(video_wire.value().size(),
              active_profile_->packet_header_size(protected_video.type) + 32);
    const auto video_decoded = takion::PacketCodec::decode_plain_packet(
        video_wire.value().bytes(), *active_profile_);
    ASSERT_TRUE(video_decoded);
    EXPECT_EQ(video_decoded.value().extended_header.has_value(),
              active_profile_->uses_extended_header(protected_video.type));
    auto video_protection = sender_.protect_local(
        video_wire.value().span(), takion::PacketFamily::video,
        *active_profile_,
        takion::PacketCryptoCoverage{0, video_wire.value().size(),
                                     video_wire.value().size(), 0},
        video_decoded.value().payload.size());
    ASSERT_TRUE(video_protection);
    EXPECT_TRUE(video_protection.value().commit());
    const auto sent_media =
        active_udp_peer_->send_to(video_wire.value().bytes(), *active_source_);
    ASSERT_TRUE(sent_media);
    EXPECT_EQ(sent_media.value(), video_wire.value().size());
    while (sink_.media_events() == 0 &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      test_support::LiveProviders::spin_pause();
    }
    EXPECT_EQ(sink_.media_events(), 1U);
    EXPECT_EQ(sink_.profile_headers(), 1U);
    const auto* media = sink_.last_as<remote_play::event::EncodedMedia>();
    ASSERT_NE(media, nullptr);
    EXPECT_EQ(media->kind, remote_play::EncodedMediaKind::video);
    ASSERT_FALSE(media->bytes.empty());
    EXPECT_EQ(media->bytes.back(), std::byte{0x51});

    const auto count_only = active_profile_->features().contains(
        takion::Feature::av_unit_count_only_encoding);
    const std::size_t audio_header = active_version_ >= 12 ? 19 : 18;
    std::vector<std::byte> audio_payload(audio_header + 3);
    audio_payload[3] = std::byte{1};
    audio_payload[5] = std::byte{2};
    audio_payload[6] = count_only ? std::byte{0} : std::byte{1};
    audio_payload[7] = std::byte{3};
    audio_payload[8] = std::byte{5};
    audio_payload[audio_header] = std::byte{0xa1};
    audio_payload[audio_header + 1] = std::byte{0xa2};
    audio_payload[audio_header + 2] = std::byte{0xa3};
    const takion::PacketType burst_type{
        static_cast<std::uint8_t>(takion::PacketFamily::audio_and_haptics)};
    ASSERT_NO_FATAL_FAILURE(send_protected(
        takion::PlainPacket{
            burst_type,
            active_profile_->uses_extended_header(burst_type)
                ? std::optional<takion::ExtendedPacketHeader>{{3, 4}}
                : std::nullopt,
            std::move(audio_payload)},
        takion::PacketFamily::audio_and_haptics));
  }

  void disconnect() {
    const auto encoded =
        codec_.encode(takion::Disconnect{"Server shutting down", std::nullopt});
    ASSERT_TRUE(encoded);
    const auto message =
        active_peer_->send(1, takion::MessageDataType{0}, encoded.value(), 0);
    ASSERT_TRUE(message.has_value());
    const auto chunk =
        takion::ControlChunkCodec::encode_packet(takion::DataChunk{
            takion::ControlHeader{active_init_->values.tag, {}, 0, 1},
            std::vector<std::byte>(message->fragments[0].payload.begin(),
                                   message->fragments[0].payload.end())});
    ASSERT_TRUE(chunk);
    ASSERT_NO_FATAL_FAILURE(send_protected(
        takion::PlainPacket{takion::PacketType{static_cast<std::uint8_t>(
                                takion::PacketFamily::control)},
                            std::nullopt,
                            {chunk.value().begin(), chunk.value().end()}},
        takion::PacketFamily::control));
    test_support::LiveProviders::nap(std::chrono::milliseconds{50});
    while (link_->state() == StreamState::streaming &&
           test_support::LiveProviders::now() < deadline_)
      static_cast<void>(link_->pump_once());
  }

  void fall_silent(std::chrono::microseconds by) {
    clock_.advance(by);
    for (int pump = 0; pump < 20; ++pump) ASSERT_TRUE(link_->pump_once());
  }

  void hear_again() {
    ASSERT_NO_FATAL_FAILURE(send_rumble());
    const auto heard = sink_.waiting().size();
    while (sink_.waiting().size() == heard &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      test_support::LiveProviders::spin_pause();
    }
  }

  void receive_burst() {
    while (sink_.media_events() == 1 &&
           test_support::LiveProviders::now() < deadline_)
      ASSERT_TRUE(link_->pump_once());
  }

  void hold_a_frame_and_report_congestion() {
    std::vector<Deadline> congestion_at;
    const auto drain_console = [&] {
      for (;;) {
        const auto next = active_udp_peer_->receive_from(udp_bytes_);
        if (!next) return;
        const auto packet = takion::PacketCodec::decode_plain_packet(
            received(next.value().size), *active_profile_);
        if (packet &&
            packet.value().type.family() == takion::PacketFamily::congestion)
          congestion_at.push_back(test_support::LiveProviders::now());
      }
    };
    const auto before_hole = sink_.media_events();
    drain_console();
    ASSERT_NO_FATAL_FAILURE(send_video(2, 1, std::byte{0x53}));
    const auto hole_at = test_support::LiveProviders::now();
    std::optional<std::chrono::steady_clock::duration> released_after;
    auto longest_wait = std::chrono::steady_clock::duration{};
    while (!released_after && test_support::LiveProviders::now() - hole_at <
                                  std::chrono::seconds{2}) {
      const auto wait_started = test_support::LiveProviders::now();
      ASSERT_TRUE(link_->wait(std::chrono::seconds{5}));
      longest_wait = std::max(
          longest_wait, test_support::LiveProviders::now() - wait_started);
      ASSERT_TRUE(link_->pump_once());
      if (sink_.media_events() > before_hole)
        released_after = test_support::LiveProviders::now() - hole_at;
    }
    ASSERT_TRUE(released_after);
    EXPECT_GE(*released_after, std::chrono::milliseconds{16});
    EXPECT_LT(*released_after, std::chrono::milliseconds{150});
    EXPECT_LT(longest_wait, std::chrono::milliseconds{150});
    EXPECT_EQ(sink_.media_events(), before_hole + 1);

    drain_console();
    congestion_at.clear();
    const auto waits_started = test_support::LiveProviders::now();
    while (test_support::LiveProviders::now() - waits_started <
           std::chrono::milliseconds{1100}) {
      ASSERT_TRUE(link_->wait(std::chrono::seconds{5}));
      drain_console();
    }
    ASSERT_GE(congestion_at.size(), 4U);
    for (std::size_t index = 1; index < congestion_at.size(); ++index) {
      SCOPED_TRACE(index);
      const auto gap = congestion_at[index] - congestion_at[index - 1];
      EXPECT_GE(gap, std::chrono::milliseconds{150});
      EXPECT_LE(gap, std::chrono::milliseconds{300});
    }
    EXPECT_EQ(link_->state(), StreamState::streaming);
  }

  void skip_frames_and_lose_ctrl() {
    test_support::LiveProviders::nap(std::chrono::milliseconds{250});
    for (std::uint16_t gap = 1; gap <= 70; ++gap)
      ASSERT_NO_FATAL_FAILURE(send_video(
          gap, static_cast<std::uint16_t>(gap * 2U), std::byte{0x52}));
    for (int round = 0; round < 4; ++round) {
      ASSERT_TRUE(link_->pump_once());
      EXPECT_EQ(link_->state(), StreamState::streaming);
    }
    EXPECT_TRUE(ctrl_->close());
    for (int round = 0; round < 200; ++round) {
      ASSERT_TRUE(link_->pump_once());
      EXPECT_EQ(link_->state(), StreamState::streaming);
      test_support::LiveProviders::spin_pause();
    }
  }

  void close_peers() {
    EXPECT_TRUE(active_udp_peer_->close());
    EXPECT_TRUE(udp_peer_->close());
    EXPECT_TRUE(listener_->close());
  }

  auto link() -> play::Link& { return *link_; }
  auto sink() const -> const EventCounter& { return sink_; }
  auto diagnostics() const -> const CapturingDiagnostics& {
    return diagnostics_;
  }
  auto controller() const -> const remote_play::ControllerState& {
    return controller_;
  }
  auto echoed_probes() const noexcept -> unsigned { return echoed_probes_; }
  auto ps5() const noexcept -> bool { return ps5_; }

 private:
  static auto cookie() -> takion::HandshakeCookie {
    takion::HandshakeCookie result{};
    for (std::size_t index = 0; index < result.size(); ++index)
      result[index] = static_cast<std::byte>(index);
    return result;
  }

  static auto sequence_of(std::span<const std::byte> payload) -> std::uint32_t {
    return (std::to_integer<std::uint32_t>(payload[0]) << 24U) |
           (std::to_integer<std::uint32_t>(payload[1]) << 16U) |
           (std::to_integer<std::uint32_t>(payload[2]) << 8U) |
           std::to_integer<std::uint32_t>(payload[3]);
  }

  static auto channel(std::uint32_t local, std::uint32_t remote,
                      std::size_t fragment_size = 1200)
      -> takion::ReliableChannel {
    return takion::ReliableChannel::create(
               takion::ReliableChannelConfig{
                   .initial_local_sequence = local,
                   .initial_remote_sequence = remote,
                   .maximum_message_size = 1024 * 1024,
                   .maximum_fragment_size = fragment_size,
                   .maximum_fragment_count = 1024,
                   .retransmission_window_size = 256,
                   .receive_reorder_size = 256,
                   .retransmission_timeout_us = 100'000,
                   .maximum_retries = 5,
               })
        .value();
  }

  auto received(std::size_t size) const -> std::span<const std::byte> {
    return std::span<const std::byte>(udp_bytes_).first(size);
  }

  static auto plain(std::span<const std::byte> raw,
                    const takion::WireProfile& profile)
      -> std::optional<takion::PlainPacket> {
    const auto wrapped =
        takion::EnvelopeCodec::decode(raw, takion::PlainEnvelope{});
    EXPECT_TRUE(wrapped);
    if (!wrapped) return std::nullopt;
    auto packet =
        takion::PacketCodec::decode_plain_packet(wrapped.value(), profile);
    EXPECT_TRUE(packet);
    if (!packet) return std::nullopt;
    return std::move(packet.value());
  }

  auto decode_senkusha(std::span<const std::byte> raw)
      -> std::optional<std::pair<std::uint32_t, takion::Senkusha>> {
    const auto packet = plain(raw, *profile_);
    if (!packet) return std::nullopt;
    if (packet->type.family() != takion::PacketFamily::control)
      return std::nullopt;
    const auto control =
        takion::ControlChunkCodec::decode_packet(packet->payload);
    EXPECT_TRUE(control);
    if (!control) return std::nullopt;
    const auto* data = std::get_if<takion::DataChunk>(&control.value());
    if (!data) return std::nullopt;
    EXPECT_GE(data->payload.size(), 4U);
    if (data->payload.size() < 4) return std::nullopt;
    const auto sequence = sequence_of(data->payload);
    auto decoder = channel(0, sequence);
    const auto decoded = decoder.receive(data->payload, true, 0);
    EXPECT_FALSE(decoded.anomaly);
    if (decoded.messages.size() != 1 || decoded.messages[0].channel != 8)
      return std::nullopt;
    const auto message = codec_.decode(decoded.messages[0].payload);
    EXPECT_TRUE(message);
    if (!message) return std::nullopt;
    const auto* senkusha = std::get_if<takion::Senkusha>(&message.value());
    if (!senkusha) return std::nullopt;
    return std::pair{sequence, *senkusha};
  }

  void send_control(const takion::ControlChunkPacket& chunk) {
    const auto encoded = takion::ControlChunkCodec::encode_packet(chunk);
    ASSERT_TRUE(encoded);
    const takion::PlainPacket response{
        takion::PacketType{
            static_cast<std::uint8_t>(takion::PacketFamily::control)},
        std::nullopt,
        {encoded.value().begin(), encoded.value().end()}};
    const auto packet =
        takion::PacketCodec::encode_plain_packet(response, *profile_);
    ASSERT_TRUE(packet);
    const auto wire =
        takion::EnvelopeCodec::encode(packet.value(), takion::PlainEnvelope{});
    ASSERT_TRUE(wire);
    const auto sent = udp_peer_->send_to(wire.value(), *source_);
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent.value(), wire.value().size());
  }

  void send_message(const takion::ControlMessage& message,
                    std::uint16_t on_channel = 1) {
    const auto encoded = codec_.encode(message);
    ASSERT_TRUE(encoded);
    const auto sent =
        peer_->send(on_channel, takion::MessageDataType{0}, encoded.value(), 0);
    ASSERT_TRUE(sent.has_value());
    ASSERT_EQ(sent->fragments.size(), 1U);
    ASSERT_NO_FATAL_FAILURE(send_control(takion::DataChunk{
        takion::ControlHeader{init_->values.tag, {}, 0, 1},
        std::vector<std::byte>(sent->fragments[0].payload.begin(),
                               sent->fragments[0].payload.end())}));
  }

  void acknowledge(std::uint32_t sequence) {
    ASSERT_NO_FATAL_FAILURE(send_control(takion::DataAcknowledgementChunk{
        takion::ControlHeader{init_->values.tag, {}, 0, 0},
        takion::DataAcknowledgement{sequence, 0x19000, {}, {}}}));
  }

  void send_active_control(const takion::ControlChunkPacket& chunk) {
    const auto encoded = takion::ControlChunkCodec::encode_packet(chunk);
    ASSERT_TRUE(encoded);
    const takion::PlainPacket response{
        takion::PacketType{
            static_cast<std::uint8_t>(takion::PacketFamily::control)},
        std::nullopt,
        {encoded.value().begin(), encoded.value().end()}};
    const auto packet =
        takion::PacketCodec::encode_plain_packet(response, *active_profile_);
    ASSERT_TRUE(packet);
    const auto wire =
        takion::EnvelopeCodec::encode(packet.value(), takion::PlainEnvelope{});
    ASSERT_TRUE(wire);
    const auto sent = active_udp_peer_->send_to(wire.value(), *active_source_);
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent.value(), wire.value().size());
  }

  void send_active_message(const takion::ControlMessage& message) {
    const auto encoded = codec_.encode(message);
    ASSERT_TRUE(encoded);
    const auto sent =
        active_peer_->send(1, takion::MessageDataType{0}, encoded.value(), 0);
    ASSERT_TRUE(sent.has_value());
    ASSERT_EQ(sent->fragments.size(), 1U);
    ASSERT_NO_FATAL_FAILURE(send_active_control(takion::DataChunk{
        takion::ControlHeader{active_init_->values.tag, {}, 0, 1},
        std::vector<std::byte>(sent->fragments[0].payload.begin(),
                               sent->fragments[0].payload.end())}));
  }

  void send_protected(const takion::PlainPacket& packet,
                      takion::PacketFamily family) {
    auto wire =
        takion::PacketCodec::encode_plain_packet(packet, *active_profile_);
    ASSERT_TRUE(wire);
    auto protection = sender_.protect_local(
        wire.value().span(), family, *active_profile_,
        takion::PacketCryptoCoverage{0, wire.value().size(),
                                     wire.value().size(), 0},
        packet.payload.size());
    ASSERT_TRUE(protection);
    EXPECT_TRUE(protection.value().commit());
    EXPECT_TRUE(
        active_udp_peer_->send_to(wire.value().bytes(), *active_source_));
  }

  void send_rumble() {
    std::vector<std::byte> rumble(
        active_profile_->data_message_prefix_size(takion::MessageDataType{7}),
        std::byte{0});
    rumble.insert(rumble.end(),
                  {std::byte{0x00}, std::byte{0x40}, std::byte{0x80}});
    const auto sent_rumble =
        active_peer_->send(1, takion::MessageDataType{7}, rumble, 0);
    ASSERT_TRUE(sent_rumble.has_value());
    ASSERT_NO_FATAL_FAILURE(send_active_control(takion::DataChunk{
        takion::ControlHeader{active_init_->values.tag, {}, 0, 1},
        std::vector<std::byte>(sent_rumble->fragments[0].payload.begin(),
                               sent_rumble->fragments[0].payload.end())}));
  }

  void send_video(std::uint16_t index, std::uint16_t frame, std::byte last) {
    std::vector<std::byte> payload(32);
    payload[0] = static_cast<std::byte>(index >> 8U);
    payload[1] = static_cast<std::byte>(index & 0xffU);
    payload[2] = static_cast<std::byte>(frame >> 8U);
    payload[3] = static_cast<std::byte>(frame & 0xffU);
    payload.back() = last;
    ASSERT_NO_FATAL_FAILURE(send_protected(
        takion::PlainPacket{video_type_, video_header_, std::move(payload)},
        takion::PacketFamily::video));
  }

  auto await_idr() -> testing::AssertionResult {
    while (test_support::LiveProviders::now() < deadline_) {
      if (!link_->pump_once())
        return testing::AssertionFailure() << "the link stopped pumping";
      const auto next = active_udp_peer_->receive_from(udp_bytes_);
      if (!next) {
        if (next.error() != NetError::would_block)
          return testing::AssertionFailure() << "the active peer failed";
        test_support::LiveProviders::spin_pause();
        continue;
      }
      const auto packet = takion::PacketCodec::decode_plain_packet(
          received(next.value().size), *active_profile_);
      if (!packet)
        return testing::AssertionFailure() << "an undecodable packet";
      if (packet.value().type.family() != takion::PacketFamily::control)
        continue;
      const auto chunk =
          takion::ControlChunkCodec::decode_packet(packet.value().payload);
      if (!chunk) return testing::AssertionFailure() << "an undecodable chunk";
      const auto* data = std::get_if<takion::DataChunk>(&chunk.value());
      if (!data || data->payload.size() != 11 ||
          data->payload[5] != std::byte{2})
        continue;
      if (data->header.flags == 1 && data->payload[9] == std::byte{0x08} &&
          data->payload[10] == std::byte{0x19})
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure() << "no IDR request before the deadline";
  }

  void open_calibration() {
    auto datagram = udp_peer_->receive_from(udp_bytes_);
    while (!datagram && test_support::LiveProviders::now() < deadline_) {
      ASSERT_EQ(datagram.error(), NetError::would_block);
      ASSERT_TRUE(link_->pump_once());
      test_support::LiveProviders::spin_pause();
      datagram = udp_peer_->receive_from(udp_bytes_);
    }
    ASSERT_TRUE(datagram);
    ASSERT_GT(datagram.value().size, 0U);
    source_ = datagram.value().source;
    profile_ = takion::resolve_takion_wire_profile(7);
    ASSERT_TRUE(profile_);
    const auto packet = plain(received(datagram.value().size), *profile_);
    ASSERT_TRUE(packet);
    const auto chunk =
        takion::ControlChunkCodec::decode_packet(packet->payload);
    ASSERT_TRUE(chunk);
    const auto* init = std::get_if<takion::InitChunk>(&chunk.value());
    ASSERT_NE(init, nullptr);
    init_ = *init;
    EXPECT_NE(init_->values.tag.value, 0U);
    EXPECT_EQ(init_->values.outbound_streams.value, 100);
    EXPECT_EQ(init_->values.inbound_streams.value, 100);

    ASSERT_NO_FATAL_FAILURE(send_control(takion::InitAcknowledgementChunk{
        takion::ControlHeader{init_->values.tag, {}, 0, 0},
        takion::InitAcknowledgementValues{
            {takion::VerificationTag{0xa1b2c3d4},
             takion::AdvertisedReceiveWindow{
                 takion::default_advertised_receive_window},
             takion::StreamCount{100}, takion::StreamCount{100},
             takion::InitialSequenceNumber{0x01020304}},
            cookie()}}));
    const auto readable_since = test_support::LiveProviders::now();
    EXPECT_TRUE(link_->wait(std::chrono::seconds{5}));
    EXPECT_LT(test_support::LiveProviders::now() - readable_since,
              std::chrono::seconds{1});
    ASSERT_NO_FATAL_FAILURE(send_control(takion::CookieAcknowledgementChunk{
        takion::ControlHeader{init_->values.tag, {}, 0, 0}}));
  }

  void negotiate_calibration() {
    bool saw_request = false;
    std::uint32_t request_sequence{};
    while (!saw_request && test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      const auto next = udp_peer_->receive_from(udp_bytes_);
      if (!next) {
        ASSERT_EQ(next.error(), NetError::would_block);
        test_support::LiveProviders::spin_pause();
        continue;
      }
      const auto packet = plain(received(next.value().size), *profile_);
      ASSERT_TRUE(packet);
      const auto control =
          takion::ControlChunkCodec::decode_packet(packet->payload);
      ASSERT_TRUE(control);
      if (const auto* data = std::get_if<takion::DataChunk>(&control.value())) {
        ASSERT_GE(data->payload.size(), 4U);
        request_sequence = sequence_of(data->payload);
        auto decoder = channel(0, request_sequence);
        const auto decoded = decoder.receive(data->payload, true, 0);
        ASSERT_FALSE(decoded.anomaly);
        ASSERT_EQ(decoded.messages.size(), 1U);
        EXPECT_EQ(decoded.messages[0].channel, 1);
        const auto message = codec_.decode(decoded.messages[0].payload);
        ASSERT_TRUE(message);
        saw_request =
            std::holds_alternative<takion::ProtocolRequest>(message.value());
      }
    }
    ASSERT_TRUE(saw_request);
    ASSERT_NO_FATAL_FAILURE(acknowledge(request_sequence));
    peer_.emplace(
        channel(0xa1b2c3d4, init_->values.initial_sequence_number.value));
    ASSERT_NO_FATAL_FAILURE(
        send_message(takion::ProtocolRequestAck{console_.version}));
    bool saw_echo_enable = false;
    bool sent_bang = false;
    std::optional<takion::ReliableChannel> echo_decoder;
    while (!saw_echo_enable && test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      const auto next = udp_peer_->receive_from(udp_bytes_);
      if (!next) {
        ASSERT_EQ(next.error(), NetError::would_block);
        test_support::LiveProviders::spin_pause();
        continue;
      }
      const auto packet = plain(received(next.value().size), *profile_);
      ASSERT_TRUE(packet);
      const auto control =
          takion::ControlChunkCodec::decode_packet(packet->payload);
      ASSERT_TRUE(control);
      const auto* data = std::get_if<takion::DataChunk>(&control.value());
      if (!data) continue;
      ASSERT_GE(data->payload.size(), 4U);
      const auto sequence = sequence_of(data->payload);
      ASSERT_NO_FATAL_FAILURE(acknowledge(sequence));
      if (!echo_decoder) echo_decoder.emplace(channel(0, sequence));
      EXPECT_TRUE(data->header.flags == 0 || data->header.flags == 1)
          << static_cast<int>(data->header.flags);
      const auto decoded = echo_decoder->receive(
          data->payload, (data->header.flags & 0x01U) != 0, 0);
      ASSERT_FALSE(decoded.anomaly);
      if (decoded.messages.size() == 1 && decoded.messages[0].channel == 1) {
        const auto message = codec_.decode(decoded.messages[0].payload);
        ASSERT_TRUE(message);
        if (std::holds_alternative<takion::Big>(message.value()) &&
            !sent_bang) {
          ASSERT_NO_FATAL_FAILURE(send_message(takion::Bang{
              console_.version, 1, false, false, takion::SharedSecretText{},
              std::nullopt, std::nullopt, std::nullopt, std::nullopt,
              takion::ProtocolRequestAck{console_.version}}));
          sent_bang = true;
        }
      }
      if (decoded.messages.size() == 1 && decoded.messages[0].channel == 8) {
        const auto message = codec_.decode(decoded.messages[0].payload);
        ASSERT_TRUE(message);
        if (const auto* senkusha =
                std::get_if<takion::Senkusha>(&message.value())) {
          const auto* echo =
              std::get_if<takion::SenkushaEchoCommand>(&senkusha->command);
          saw_echo_enable = echo && echo->state;
        }
      }
    }
    EXPECT_TRUE(sent_bang);
    ASSERT_TRUE(saw_echo_enable);
  }

  void echo_probes() {
    bool saw_echo_disable = false;
    std::uint32_t echo_disable_sequence{};
    while (!saw_echo_disable &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      const auto next = udp_peer_->receive_from(udp_bytes_);
      if (!next) {
        ASSERT_EQ(next.error(), NetError::would_block);
        test_support::LiveProviders::spin_pause();
        continue;
      }
      const auto raw = received(next.value().size);
      const auto packet = plain(raw, *profile_);
      ASSERT_TRUE(packet);
      if (packet->type.family() != takion::PacketFamily::control) {
        const auto sent_probe = udp_peer_->send_to(raw, *source_);
        ASSERT_TRUE(sent_probe);
        EXPECT_EQ(sent_probe.value(), raw.size());
        ++echoed_probes_;
        continue;
      }
      const auto control =
          takion::ControlChunkCodec::decode_packet(packet->payload);
      ASSERT_TRUE(control);
      const auto* data = std::get_if<takion::DataChunk>(&control.value());
      if (!data) continue;
      ASSERT_GE(data->payload.size(), 4U);
      const auto sequence = sequence_of(data->payload);
      auto decoder = channel(0, sequence);
      const auto decoded = decoder.receive(data->payload, true, 0);
      ASSERT_FALSE(decoded.anomaly);
      if (decoded.messages.size() == 1 && decoded.messages[0].channel == 8) {
        const auto message = codec_.decode(decoded.messages[0].payload);
        ASSERT_TRUE(message);
        if (const auto* senkusha =
                std::get_if<takion::Senkusha>(&message.value())) {
          const auto* echo =
              std::get_if<takion::SenkushaEchoCommand>(&senkusha->command);
          saw_echo_disable = echo && !echo->state;
          if (saw_echo_disable) echo_disable_sequence = sequence;
        }
      }
    }
    ASSERT_TRUE(saw_echo_disable);
    EXPECT_EQ(echoed_probes_, 10U);
    ASSERT_NO_FATAL_FAILURE(acknowledge(echo_disable_sequence));
  }

  void measure_mtu() {
    bool saw_inbound_mtu = false;
    while (!saw_inbound_mtu && test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      const auto next = udp_peer_->receive_from(udp_bytes_);
      if (!next) {
        ASSERT_EQ(next.error(), NetError::would_block);
        test_support::LiveProviders::spin_pause();
        continue;
      }
      const auto message = decode_senkusha(received(next.value().size));
      if (!message) continue;
      const auto* mtu =
          std::get_if<takion::SenkushaMtuCommand>(&message->second.command);
      saw_inbound_mtu = mtu && mtu->id == 1 && mtu->mtu_request == 1454;
    }
    ASSERT_TRUE(saw_inbound_mtu);
    takion::SenkushaProbeValues video_reply;
    video_reply.video = true;
    video_reply.frame_index = 1;
    video_reply.units_in_frame_total = 1;
    video_reply.codec = 0xff;
    const auto encoded_reply = takion::PacketCodec::encode_senkusha_probe(
        video_reply, *profile_, takion::PlainEnvelope{});
    ASSERT_TRUE(encoded_reply);
    const takion::PlainPacket video_packet{
        takion::PacketType{
            static_cast<std::uint8_t>(takion::PacketFamily::video)},
        std::nullopt, encoded_reply.value().encoded_av_payload};
    const auto encoded_video =
        takion::PacketCodec::encode_plain_packet(video_packet, *profile_);
    ASSERT_TRUE(encoded_video);
    const auto wire_video = takion::EnvelopeCodec::encode(
        encoded_video.value(), takion::PlainEnvelope{});
    ASSERT_TRUE(wire_video);
    const auto sent_video = udp_peer_->send_to(wire_video.value(), *source_);
    ASSERT_TRUE(sent_video);
    EXPECT_EQ(sent_video.value(), wire_video.value().size());
    bool saw_client_mtu = false;
    while (!saw_client_mtu && test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      const auto next = udp_peer_->receive_from(udp_bytes_);
      if (!next) {
        ASSERT_EQ(next.error(), NetError::would_block);
        test_support::LiveProviders::spin_pause();
        continue;
      }
      const auto message = decode_senkusha(received(next.value().size));
      if (!message) continue;
      const auto* mtu = std::get_if<takion::SenkushaClientMtuCommand>(
          &message->second.command);
      saw_client_mtu =
          mtu && mtu->id == 1 && mtu->state && mtu->mtu_request == 1454;
    }
    ASSERT_TRUE(saw_client_mtu);
    ASSERT_NO_FATAL_FAILURE(send_message(
        takion::Senkusha{takion::SenkushaClientMtuCommand{1, 1454, true, 1454}},
        8));
    bool saw_outbound_probe = false;
    while (!saw_outbound_probe &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      const auto next = udp_peer_->receive_from(udp_bytes_);
      if (!next) {
        ASSERT_EQ(next.error(), NetError::would_block);
        test_support::LiveProviders::spin_pause();
        continue;
      }
      const auto raw = received(next.value().size);
      const auto packet = plain(raw, *profile_);
      ASSERT_TRUE(packet);
      if (packet->type.family() == takion::PacketFamily::control) continue;
      EXPECT_EQ(raw.size() + 28, 1454U);
      const auto returned = udp_peer_->send_to(raw, *source_);
      ASSERT_TRUE(returned);
      EXPECT_EQ(returned.value(), raw.size());
      saw_outbound_probe = true;
    }
    ASSERT_TRUE(saw_outbound_probe);
    std::optional<std::uint32_t> final_mtu_sequence;
    while (!final_mtu_sequence &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_->pump_once());
      const auto next = udp_peer_->receive_from(udp_bytes_);
      if (!next) {
        ASSERT_EQ(next.error(), NetError::would_block);
        test_support::LiveProviders::spin_pause();
        continue;
      }
      const auto message = decode_senkusha(received(next.value().size));
      if (!message) continue;
      const auto* mtu = std::get_if<takion::SenkushaClientMtuCommand>(
          &message->second.command);
      if (mtu && mtu->id == 2 && !mtu->state && mtu->mtu_request == 1454)
        final_mtu_sequence = message->first;
    }
    ASSERT_TRUE(final_mtu_sequence);
    ASSERT_NO_FATAL_FAILURE(acknowledge(*final_mtu_sequence));
  }

  StreamConsole console_;
  bool ps5_;
  ZeroCrypto crypto_;
  FixedClock clock_;
  CapturingDiagnostics diagnostics_;
  platform::DefaultPlatformFactory providers_{platform::PlatformProviders{
      .clock = &clock_, .crypto = &crypto_, .diagnostics = &diagnostics_}};
  std::unique_ptr<platform::TcpListener> listener_;
  std::unique_ptr<platform::UdpSocket> udp_peer_;
  std::unique_ptr<platform::UdpSocket> active_udp_peer_;
  std::unique_ptr<play::Link> link_;
  EventCounter sink_;
  remote_play::ControllerState controller_{};
  Deadline deadline_{test_support::LiveProviders::now() +
                     std::chrono::seconds{3}};
  std::unique_ptr<platform::TcpSocket> ctrl_;
  std::array<std::byte, 2048> udp_bytes_{};
  std::optional<platform::SocketEndpoint> source_;
  std::optional<takion::WireProfile> profile_;
  std::optional<takion::InitChunk> init_;
  std::optional<takion::ReliableChannel> peer_;
  takion::ControlCodec codec_;
  unsigned echoed_probes_{};
  std::optional<takion::InitChunk> active_init_;
  std::optional<platform::SocketEndpoint> active_source_;
  std::uint32_t active_version_{};
  std::optional<takion::WireProfile> active_profile_;
  std::optional<takion::ReliableChannel> active_peer_;
  takion::PlatformTakionCryptoProvider sender_provider_{
      crypto_, test_support::LiveProviders::threading()};
  takion::CryptoContext sender_{takion::CryptoPolicy::required};
  takion::PacketType video_type_{
      static_cast<std::uint8_t>(takion::PacketFamily::video)};
  std::optional<takion::ExtendedPacketHeader> video_header_;
};

}  // namespace play_test::home_links
