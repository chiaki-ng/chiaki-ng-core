#pragma once

#include "libchiaking/platform/factory.hpp"
#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "tests/support/setup.hpp"

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

namespace public_route_fixture {
namespace holepunch = libchiaking::net::nat::holepunch;
namespace rudp = libchiaking::net::transport::rudp;
namespace platform = libchiaking::platform;
namespace takion = libchiaking::net::protocol::takion;

class RouteSocket final : public platform::UdpSocket {
 public:
  RouteSocket(platform::NetworkProvider& owner, unsigned& closes,
              unsigned& probe_sends, unsigned& rudp_sends,
              unsigned& cookie_sends, unsigned& registration_sends,
              unsigned& ctrl_http_sends, unsigned& ack_sends,
              unsigned& session_id_queued, unsigned& takion_init_sends,
              unsigned& takion_cookie_sends, unsigned& takion_data_sends,
              unsigned& distinct_takion_data_sends,
              unsigned& senkusha_control_sends, unsigned& takion_big_sends,
              unsigned& active_big_ack_sends,
              unsigned& active_stream_info_sends, unsigned& takion_probe_sends,
              unsigned& client_mtu_sends, unsigned& client_mtu_final_sends,
              unsigned& switch_request_sends, unsigned& switch_ack_sends,
              bool respond_to_init, bool respond_to_cookie,
              bool respond_to_registration, bool respond_to_ctrl,
              bool queue_session_id, bool queue_pin_before_session_id,
              bool respond_to_takion_init, bool respond_to_takion_cookie,
              bool respond_to_takion_data, bool respond_to_takion_big,
              bool respond_to_senkusha_echo, bool respond_to_rtt_echo,
              bool respond_to_outbound_mtu_echo, bool respond_to_echo_disable,
              bool respond_to_inbound_mtu, bool respond_to_client_mtu,
              bool respond_to_client_mtu_final, bool respond_to_switch_request,
              bool registration_ps4)
      : owner_(owner),
        closes_(closes),
        probe_sends_(probe_sends),
        rudp_sends_(rudp_sends),
        cookie_sends_(cookie_sends),
        registration_sends_(registration_sends),
        ctrl_http_sends_(ctrl_http_sends),
        ack_sends_(ack_sends),
        session_id_queued_(session_id_queued),
        takion_init_sends_(takion_init_sends),
        takion_cookie_sends_(takion_cookie_sends),
        takion_data_sends_(takion_data_sends),
        distinct_takion_data_sends_(distinct_takion_data_sends),
        senkusha_control_sends_(senkusha_control_sends),
        takion_big_sends_(takion_big_sends),
        active_big_ack_sends_(active_big_ack_sends),
        active_stream_info_sends_(active_stream_info_sends),
        takion_probe_sends_(takion_probe_sends),
        client_mtu_sends_(client_mtu_sends),
        client_mtu_final_sends_(client_mtu_final_sends),
        switch_request_sends_(switch_request_sends),
        switch_ack_sends_(switch_ack_sends),
        respond_to_init_(respond_to_init),
        respond_to_cookie_(respond_to_cookie),
        respond_to_registration_(respond_to_registration),
        respond_to_ctrl_(respond_to_ctrl),
        queue_session_id_(queue_session_id),
        queue_pin_before_session_id_(queue_pin_before_session_id),
        respond_to_takion_init_(respond_to_takion_init),
        respond_to_takion_cookie_(respond_to_takion_cookie),
        respond_to_takion_data_(respond_to_takion_data),
        respond_to_takion_big_(respond_to_takion_big),
        respond_to_senkusha_echo_(respond_to_senkusha_echo),
        respond_to_rtt_echo_(respond_to_rtt_echo),
        respond_to_outbound_mtu_echo_(respond_to_outbound_mtu_echo),
        respond_to_echo_disable_(respond_to_echo_disable),
        respond_to_inbound_mtu_(respond_to_inbound_mtu),
        respond_to_client_mtu_(respond_to_client_mtu),
        respond_to_client_mtu_final_(respond_to_client_mtu_final),
        respond_to_switch_request_(respond_to_switch_request),
        registration_ps4_(registration_ps4),
        response_(stun_response_.begin(), stun_response_.end()) {}
  ~RouteSocket() override {
    if (!closed_) ++closes_;
  }
  auto owner() const noexcept -> const platform::NetworkProvider& override {
    return owner_;
  }
  auto bind(const platform::SocketEndpoint&)
      -> libchiaking::Expected<void, platform::NetError> override {
    bound_ = true;
    return {};
  }
  auto send_to(std::span<const std::byte> bytes,
               const platform::SocketEndpoint&)
      -> libchiaking::Expected<std::size_t, platform::NetError> override {
    ++send_calls;
    raw_sends.emplace_back(bytes.begin(), bytes.end());
    auto ctrl_frames = rudp::FrameCodec::decode(bytes);
    if (ctrl_frames) {
      for (const auto& frame : ctrl_frames.value()) {
        if (frame.type == 0x0230 && frame.payload.size() >= 10)
          ctrl_sends.emplace_back(
              static_cast<std::uint16_t>(
                  (std::to_integer<unsigned>(frame.payload[6]) << 8U) |
                  std::to_integer<unsigned>(frame.payload[7])),
              std::vector<std::byte>(frame.payload.begin() + 10,
                                     frame.payload.end()));
      }
      for (const auto& frame : ctrl_frames.value()) {
        if (frame.type != 0x0230 || frame.payload.size() != 10) continue;
        const auto& p = frame.payload;
        if (p[2] == std::byte{} && p[3] == std::byte{} && p[4] == std::byte{} &&
            p[5] == std::byte{} && p[6] == std::byte{1} &&
            p[7] == std::byte{0xfe}) {
          if (fail_heartbeat_send)
            return libchiaking::unexpected(platform::NetError::network_down);
          ++heartbeat_replies;
          if (!withheld_switch_response_.empty()) {
            response_ = std::move(withheld_switch_response_);
            pending_ = true;
            switch_response_sent_ = true;
          }
        }
      }
    }
    bool valid_probe = false;
    if (bytes.size() == holepunch::CandidateProbe::kEncodedSize) {
      auto probe = holepunch::ProbeCodec::decode(bytes);
      if (probe) {
        auto reply =
            holepunch::ProbeCodec::encode_response(probe.value(), {{}, 9999});
        response_.assign(reply.begin(), reply.end());
        following_response_ =
            std::vector<std::byte>{bytes.begin(), bytes.end()};
        pending_ = true;
        ++probe_sends_;
        valid_probe = true;
      }
    }
    if (probe_sends_ > 0 && !valid_probe) {
      auto envelope =
          takion::EnvelopeCodec::decode(bytes, takion::PlainEnvelope{});
      if (envelope) {
        auto profile = takion::resolve_takion_wire_profile(7);
        if (profile) {
          auto plain = takion::PacketCodec::decode_plain_packet(
              envelope.value(), *profile);
          if (plain) {
            if (plain.value().type.family_code() ==
                static_cast<std::uint8_t>(
                    takion::PacketFamily::audio_and_haptics)) {
              ++takion_probe_sends_;
              if (respond_to_rtt_echo_ && rtt_echoes_ < 10 && !pending_) {
                response_.assign(bytes.begin(), bytes.end());
                pending_ = true;
                ++rtt_echoes_;
              } else if (respond_to_outbound_mtu_echo_ && rtt_echoes_ == 10 &&
                         !outbound_mtu_echoed_ && !pending_) {
                response_.assign(bytes.begin(), bytes.end());
                pending_ = true;
                outbound_mtu_echoed_ = true;
              }
            }
            auto chunk =
                takion::ControlChunkCodec::decode_packet(plain.value().payload);
            if (chunk &&
                std::holds_alternative<takion::InitChunk>(chunk.value())) {
              ++takion_init_sends_;
              if (takion_init_sends_ == 1) {
                active_receiver_.emplace(
                    takion::ReliableChannel::create(
                        takion::ReliableChannelConfig{
                            .initial_local_sequence = 0,
                            .initial_remote_sequence = 1,
                            .maximum_message_size = 1024 * 1024,
                            .maximum_fragment_size = 2048,
                            .maximum_fragment_count = 1024,
                            .retransmission_window_size = 256,
                            .receive_reorder_size = 256,
                            .retransmission_timeout_us = 100000,
                            .maximum_retries = 5})
                        .value());
              }
              local_verification_tag_ =
                  std::get<takion::InitChunk>(chunk.value()).values.tag;
              if (respond_to_takion_init_ && !pending_) {
                takion::HandshakeCookie cookie{};
                for (std::size_t i = 0; i < cookie.size(); ++i)
                  cookie[i] = static_cast<std::byte>(i);
                const takion::InitAcknowledgementValues values{
                    {takion::VerificationTag{0xa1b2c3d4},
                     takion::AdvertisedReceiveWindow{
                         takion::default_advertised_receive_window},
                     takion::StreamCount{100}, takion::StreamCount{100},
                     takion::InitialSequenceNumber{0x01020304}},
                    cookie};
                auto payload = takion::ControlChunkCodec::encode_packet(
                    takion::ControlChunkPacket{takion::InitAcknowledgementChunk{
                        takion::ControlHeader{
                            local_verification_tag_, {}, 0, 0},
                        values}});
                if (payload) {
                  takion::PlainPacket packet{
                      takion::PacketType{static_cast<std::uint8_t>(
                          takion::PacketFamily::control)},
                      std::nullopt,
                      {payload.value().begin(), payload.value().end()}};
                  auto encoded = takion::PacketCodec::encode_plain_packet(
                      packet, *profile);
                  if (encoded) {
                    auto wire = takion::EnvelopeCodec::encode(
                        encoded.value().bytes(), takion::PlainEnvelope{});
                    if (wire) {
                      response_.assign(wire.value().begin(),
                                       wire.value().end());
                      pending_ = true;
                    }
                  }
                }
              }
            } else if (chunk && std::holds_alternative<takion::CookieChunk>(
                                    chunk.value())) {
              const auto& cookie_chunk =
                  std::get<takion::CookieChunk>(chunk.value());
              takion::HandshakeCookie expected_cookie{};
              for (std::size_t i = 0; i < expected_cookie.size(); ++i)
                expected_cookie[i] = static_cast<std::byte>(i);
              if (cookie_chunk.header.verification_tag.value == 0xa1b2c3d4 &&
                  cookie_chunk.cookie == expected_cookie) {
                ++takion_cookie_sends_;
                if (respond_to_takion_cookie_ && !pending_) {
                  auto payload = takion::ControlChunkCodec::encode_packet(
                      takion::ControlChunkPacket{
                          takion::CookieAcknowledgementChunk{
                              takion::ControlHeader{
                                  local_verification_tag_, {}, 0, 0}}});
                  if (payload) {
                    takion::PlainPacket packet{
                        takion::PacketType{static_cast<std::uint8_t>(
                            takion::PacketFamily::control)},
                        std::nullopt,
                        {payload.value().begin(), payload.value().end()}};
                    auto encoded = takion::PacketCodec::encode_plain_packet(
                        packet, *profile);
                    if (encoded) {
                      auto wire = takion::EnvelopeCodec::encode(
                          encoded.value().bytes(), takion::PlainEnvelope{});
                      if (wire) {
                        response_.assign(wire.value().begin(),
                                         wire.value().end());
                        pending_ = true;
                      }
                    }
                  }
                }
              }
            } else if (chunk && std::holds_alternative<takion::DataChunk>(
                                    chunk.value())) {
              ++takion_data_sends_;
              const auto& data_payload =
                  std::get<takion::DataChunk>(chunk.value()).payload;
              bool big_sent = false;
              if (!active_receiver_ &&
                  data_payload.size() >= takion::first_fragment_overhead &&
                  data_payload[4] == std::byte{} &&
                  data_payload[5] == std::byte{1}) {
                takion::ControlCodec codec;
                auto decoded = codec.decode(std::span{data_payload}.subspan(
                    takion::first_fragment_overhead));
                big_sent = decoded &&
                           std::holds_alternative<takion::Big>(decoded.value());
                if (big_sent) ++takion_big_sends_;
              }
              if (active_receiver_) {
                const auto flags =
                    std::get<takion::DataChunk>(chunk.value()).header.flags;
                EXPECT_TRUE(flags == 0 || flags == 1);
                const auto received = active_receiver_->receive(
                    data_payload, (flags & 0x01U) != 0, 0);
                if (!received.anomaly) {
                  takion::ControlCodec codec;
                  for (const auto& message : received.messages) {
                    if (message.channel != 1) continue;
                    auto decoded = codec.decode(message.payload);
                    if (decoded &&
                        std::holds_alternative<takion::Big>(decoded.value())) {
                      ++takion_big_sends_;
                      if (!pending_ && !active_big_acked_) {
                        std::uint32_t sequence{};
                        for (std::size_t i = 0; i < 4; ++i)
                          sequence =
                              (sequence << 8U) |
                              std::to_integer<std::uint32_t>(data_payload[i]);
                        active_big_acked_ =
                            queue_takion_ack(sequence, *profile);
                        if (active_big_acked_) {
                          ++active_big_ack_sends_;
                          auto ack_wire = std::move(response_);
                          takion::StreamInfo info;
                          info.audio_header.assign(14, std::byte{});
                          info.audio_header[0] = std::byte{2};
                          info.audio_header[1] = std::byte{16};
                          info.resolutions.push_back(
                              takion::StreamInfoResolution{
                                  1280, 720, {std::byte{1}, std::byte{2}}});
                          if (queue_takion_control(info, 0xa1b2c3d4, 1,
                                                   *profile)) {
                            following_response_ = std::move(response_);
                            ++active_stream_info_sends_;
                            takion::Bang bang;
                            bang.server_version = registration_ps4_ ? 9 : 15;
                            bang.token = 1;
                            bang.encrypted_key_accepted = true;
                            bang.version_accepted = true;
                            bang.session_key = libchiaking::net::protocol::
                                takion::SharedSecretText{std::string(24, 'A')};
                            bang.ecdh_pub_key = std::vector<std::byte>(
                                registration_ps4_ ? 65 : 133);
                            bang.ecdh_pub_key->at(0) = std::byte{0x04};
                            for (std::size_t i = 1;
                                 i < bang.ecdh_pub_key->size(); ++i)
                              bang.ecdh_pub_key->at(i) =
                                  static_cast<std::byte>(i - 1);
                            bang.ecdh_sig = std::vector<std::byte>(32);
                            bang.protocol_ack = takion::ProtocolRequestAck{
                                registration_ps4_ ? 9U : 15U};
                            if (queue_takion_control(std::move(bang),
                                                     0xa1b2c3d5, 1, *profile))
                              third_response_ = std::move(response_);
                          }
                          response_ = std::move(ack_wire);
                          pending_ = true;
                        }
                      }
                    }
                  }
                }
              }
              if (data_payload.size() >= takion::first_fragment_overhead &&
                  data_payload[4] == std::byte{} &&
                  data_payload[5] == std::byte{8})
                ++senkusha_control_sends_;
              if (respond_to_senkusha_echo_ && !senkusha_echo_acked_ &&
                  data_payload.size() >= takion::first_fragment_overhead &&
                  data_payload[4] == std::byte{} &&
                  data_payload[5] == std::byte{8} && !pending_) {
                std::uint32_t sequence{};
                for (std::size_t i = 0; i < 4; ++i)
                  sequence = (sequence << 8U) |
                             std::to_integer<std::uint32_t>(data_payload[i]);
                senkusha_echo_acked_ = queue_takion_ack(sequence, *profile);
              }
              if (respond_to_echo_disable_ && !echo_disable_acked_ &&
                  !pending_ &&
                  data_payload.size() >= takion::first_fragment_overhead &&
                  data_payload[4] == std::byte{} &&
                  data_payload[5] == std::byte{8}) {
                takion::ControlCodec codec;
                auto decoded = codec.decode(std::span{data_payload}.subspan(
                    takion::first_fragment_overhead));
                const auto* senkusha =
                    decoded ? std::get_if<takion::Senkusha>(&decoded.value())
                            : nullptr;
                const auto* echo =
                    senkusha ? std::get_if<takion::SenkushaEchoCommand>(
                                   &senkusha->command)
                             : nullptr;
                if (echo && !echo->state) {
                  std::uint32_t sequence{};
                  for (std::size_t i = 0; i < 4; ++i)
                    sequence = (sequence << 8U) |
                               std::to_integer<std::uint32_t>(data_payload[i]);
                  echo_disable_acked_ = queue_takion_ack(sequence, *profile);
                }
              }
              if (data_payload.size() >= takion::first_fragment_overhead &&
                  data_payload[4] == std::byte{} &&
                  data_payload[5] == std::byte{8}) {
                takion::ControlCodec codec;
                auto decoded = codec.decode(std::span{data_payload}.subspan(
                    takion::first_fragment_overhead));
                const auto* senkusha =
                    decoded ? std::get_if<takion::Senkusha>(&decoded.value())
                            : nullptr;
                if (senkusha) {
                  if (std::holds_alternative<takion::SenkushaClientMtuCommand>(
                          senkusha->command))
                    ++client_mtu_sends_;
                  const auto* client_mtu =
                      std::get_if<takion::SenkushaClientMtuCommand>(
                          &senkusha->command);
                  if (client_mtu && client_mtu->id == 2 &&
                      client_mtu->mtu_request == 1454 && !client_mtu->state &&
                      client_mtu->mtu_down == 1454) {
                    ++client_mtu_final_sends_;
                    if (respond_to_client_mtu_final_ &&
                        !client_mtu_final_acked_ && !pending_) {
                      std::uint32_t sequence{};
                      for (std::size_t i = 0; i < 4; ++i)
                        sequence =
                            (sequence << 8U) |
                            std::to_integer<std::uint32_t>(data_payload[i]);
                      client_mtu_final_acked_ =
                          queue_takion_ack(sequence, *profile);
                    }
                  }
                  if (respond_to_client_mtu_ && !client_mtu_responded_ &&
                      !pending_ && client_mtu && client_mtu->id == 1)
                    client_mtu_responded_ = queue_takion_control(
                        takion::Senkusha{takion::SenkushaClientMtuCommand{
                            1, 1454, true, 1454}},
                        0xa1b2c3d6, 8, *profile);
                  const auto* mtu = std::get_if<takion::SenkushaMtuCommand>(
                      &senkusha->command);
                  if (respond_to_inbound_mtu_ && !inbound_mtu_responded_ &&
                      !pending_ && mtu && mtu->id == 1) {
                    takion::SenkushaProbeValues values;
                    values.video = true;
                    values.frame_index = static_cast<std::uint16_t>(mtu->id);
                    values.units_in_frame_total = 1;
                    values.codec = 0xff;
                    auto probe = takion::PacketCodec::encode_senkusha_probe(
                        values, *profile, takion::PlainEnvelope{});
                    if (probe) {
                      takion::PlainPacket packet{
                          takion::PacketType{static_cast<std::uint8_t>(
                              takion::PacketFamily::video)},
                          std::nullopt,
                          {probe.value().encoded_av_payload.begin(),
                           probe.value().encoded_av_payload.end()}};
                      auto encoded = takion::PacketCodec::encode_plain_packet(
                          packet, *profile);
                      if (encoded) {
                        auto wire = takion::EnvelopeCodec::encode(
                            encoded.value().bytes(), takion::PlainEnvelope{});
                        if (wire) {
                          response_.assign(wire.value().begin(),
                                           wire.value().end());
                          pending_ = true;
                          inbound_mtu_responded_ = true;
                        }
                      }
                    }
                  }
                }
              }
              if (first_takion_data_payload_.empty()) {
                first_takion_data_payload_ = data_payload;
                ++distinct_takion_data_sends_;
              } else if (data_payload != first_takion_data_payload_)
                ++distinct_takion_data_sends_;
              if (!active_receiver_ && respond_to_takion_data_ &&
                  !takion_data_responded_ && !pending_) {
                takion::ReliableChannelConfig config{
                    .initial_local_sequence = 0xa1b2c3d4,
                    .initial_remote_sequence = 1,
                    .maximum_message_size = 1024 * 1024,
                    .maximum_fragment_size = 1200,
                    .maximum_fragment_count = 1024,
                    .retransmission_window_size = 256,
                    .receive_reorder_size = 256,
                    .retransmission_timeout_us = 100000,
                    .maximum_retries = 5};
                takion::ReliableChannel peer =
                    takion::ReliableChannel::create(config).value();
                takion::ControlCodec codec;
                auto message = codec.encode(takion::ProtocolRequestAck{20});
                if (message) {
                  auto sent = peer.send(1, takion::MessageDataType{0},
                                        message.value().bytes(), 0);
                  if (sent.has_value() && sent->fragments.size() == 1) {
                    auto payload = takion::ControlChunkCodec::encode_packet(
                        takion::ControlChunkPacket{takion::DataChunk{
                            takion::ControlHeader{
                                local_verification_tag_, {}, 0, 1},
                            std::vector<std::byte>(
                                sent->fragments[0].payload.begin(),
                                sent->fragments[0].payload.end())}});
                    if (payload) {
                      takion::PlainPacket packet{
                          takion::PacketType{static_cast<std::uint8_t>(
                              takion::PacketFamily::control)},
                          std::nullopt,
                          {payload.value().begin(), payload.value().end()}};
                      auto encoded = takion::PacketCodec::encode_plain_packet(
                          packet, *profile);
                      if (encoded) {
                        auto wire = takion::EnvelopeCodec::encode(
                            encoded.value().bytes(), takion::PlainEnvelope{});
                        if (wire) {
                          following_response_.emplace(wire.value().begin(),
                                                      wire.value().end());
                          auto ack_payload =
                              takion::ControlChunkCodec::encode_packet(
                                  takion::ControlChunkPacket{
                                      takion::DataAcknowledgementChunk{
                                          takion::ControlHeader{
                                              local_verification_tag_,
                                              {},
                                              0,
                                              0},
                                          takion::DataAcknowledgement{
                                              1, 0x19000, {}, {}}}});
                          if (ack_payload) {
                            takion::PlainPacket ack_packet{
                                takion::PacketType{static_cast<std::uint8_t>(
                                    takion::PacketFamily::control)},
                                std::nullopt,
                                {ack_payload.value().begin(),
                                 ack_payload.value().end()}};
                            auto ack_plain =
                                takion::PacketCodec::encode_plain_packet(
                                    ack_packet, *profile);
                            if (ack_plain) {
                              auto ack_wire = takion::EnvelopeCodec::encode(
                                  ack_plain.value().bytes(),
                                  takion::PlainEnvelope{});
                              if (ack_wire) {
                                response_.assign(ack_wire.value().begin(),
                                                 ack_wire.value().end());
                                pending_ = true;
                                takion_data_responded_ = true;
                              }
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
              if (respond_to_takion_big_ && !bang_sent_ && big_sent &&
                  takion_data_responded_ && !pending_) {
                takion::ReliableChannelConfig config{
                    .initial_local_sequence = 0xa1b2c3d5,
                    .initial_remote_sequence = 1,
                    .maximum_message_size = 1024 * 1024,
                    .maximum_fragment_size = 1200,
                    .maximum_fragment_count = 1024,
                    .retransmission_window_size = 256,
                    .receive_reorder_size = 256,
                    .retransmission_timeout_us = 100000,
                    .maximum_retries = 5};
                takion::ReliableChannel peer =
                    takion::ReliableChannel::create(config).value();
                takion::ControlCodec codec;
                auto message = codec.encode(takion::Bang{
                    20, 1, false, false, takion::SharedSecretText{},
                    std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                    takion::ProtocolRequestAck{20}});
                if (message) {
                  auto sent = peer.send(1, takion::MessageDataType{0},
                                        message.value().bytes(), 0);
                  if (sent.has_value() && sent->fragments.size() == 1) {
                    auto payload = takion::ControlChunkCodec::encode_packet(
                        takion::ControlChunkPacket{takion::DataChunk{
                            takion::ControlHeader{
                                local_verification_tag_, {}, 0, 1},
                            std::vector<std::byte>(
                                sent->fragments[0].payload.begin(),
                                sent->fragments[0].payload.end())}});
                    if (payload) {
                      takion::PlainPacket packet{
                          takion::PacketType{static_cast<std::uint8_t>(
                              takion::PacketFamily::control)},
                          std::nullopt,
                          {payload.value().begin(), payload.value().end()}};
                      auto encoded = takion::PacketCodec::encode_plain_packet(
                          packet, *profile);
                      if (encoded) {
                        auto wire = takion::EnvelopeCodec::encode(
                            encoded.value().bytes(), takion::PlainEnvelope{});
                        if (wire) {
                          response_.assign(wire.value().begin(),
                                           wire.value().end());
                          pending_ = true;
                          bang_sent_ = true;
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    if (probe_sends_ > 0 && !valid_probe) {
      auto frames = rudp::FrameCodec::decode(bytes);
      if (frames && frames.value().size() == 2 &&
          frames.value().front().type == 0x2030 &&
          frames.value().back().type == 0x0230) {
        ++registration_sends_;
        if (respond_to_registration_ && init_responded_ &&
            !registration_responded_) {
          const std::string body =
              std::string{registration_ps4_ ? "PS4-RegistKey: "
                                            : "PS5-RegistKey: "} +
              "010102030405060708090a0b0c0d0e0f\r\n"
              "RP-Key: 000102030405060708090a0b0c0d0e0f\r\n" +
              (registration_ps4_ ? "PS4-Mac: " : "PS5-Mac: ") +
              "001122334455\r\n" +
              (registration_ps4_ ? "PS4-Nickname: " : "PS5-Nickname: ") +
              "SyntheticBox\r\n";
          const std::string http = "HTTP/1.1 200 OK\r\nContent-Length: " +
                                   std::to_string(body.size()) + "\r\n\r\n" +
                                   std::string{body};
          rudp::Frame reply_frame{0x0230, {std::byte{0x56}, std::byte{0x7A}}};
          const auto* first = reinterpret_cast<const std::byte*>(http.data());
          reply_frame.payload.insert(reply_frame.payload.end(), first,
                                     first + http.size());
          auto reply = rudp::FrameCodec::encode(
              std::span<const rudp::Frame>{&reply_frame, 1});
          if (reply) {
            response_ = std::move(reply.value());
            pending_ = true;
            registration_responded_ = true;
          }
        } else if (registration_responded_ && !session_responded_) {
          constexpr std::string_view http =
              "HTTP/1.1 200 OK\r\n"
              "RP-Nonce: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n";
          rudp::Frame reply_frame{0x0230, {std::byte{0x56}, std::byte{0x7A}}};
          const auto* first = reinterpret_cast<const std::byte*>(http.data());
          reply_frame.payload.insert(reply_frame.payload.end(), first,
                                     first + http.size());
          auto reply = rudp::FrameCodec::encode(
              std::span<const rudp::Frame>{&reply_frame, 1});
          if (reply) {
            response_ = std::move(reply.value());
            pending_ = true;
            session_responded_ = true;
          }
        } else if (respond_to_ctrl_ && session_responded_ && !ctrl_responded_) {
          ++ctrl_http_sends_;
          ctrl_request.assign(reinterpret_cast<const char*>(
                                  frames.value().back().payload.data()),
                              frames.value().back().payload.size());
          const std::string http =
              "HTTP/1.1 200 OK\r\n" +
              (ctrl_server_type.empty()
                   ? std::string{}
                   : "RP-Server-Type: " + ctrl_server_type + "\r\n") +
              "\r\n";
          rudp::Frame reply_frame{0x0230, {std::byte{0x56}, std::byte{0x7B}}};
          const auto* first = reinterpret_cast<const std::byte*>(http.data());
          reply_frame.payload.insert(reply_frame.payload.end(), first,
                                     first + http.size());
          auto reply = rudp::FrameCodec::encode(
              std::span<const rudp::Frame>{&reply_frame, 1});
          if (reply) {
            response_ = std::move(reply.value());
            pending_ = true;
            ctrl_responded_ = true;
          }
        }
      } else if (frames && frames.value().size() == 1) {
        const auto type = frames.value().front().type;
        if (type == 0x0230) {
          const auto& payload = frames.value().front().payload;
          constexpr std::array fixed{
              std::byte{}, std::byte{},     std::byte{}, std::byte{0x10},
              std::byte{}, std::byte{0x0D}, std::byte{}, std::byte{}};
          if (payload.size() == 26 &&
              std::equal(payload.begin() + 2, payload.begin() + 10,
                         fixed.begin())) {
            ++switch_request_sends_;
            if (fail_switch_send)
              return libchiaking::unexpected(platform::NetError::network_down);
            if (respond_to_switch_request_ && !switch_response_sent_ &&
                !pending_) {
              auto header = libchiaking::net::protocol::remote_play::CtrlCodec::
                  encode_frame_header(0x34, 0);
              if (header) {
                std::vector<std::byte> reply{std::byte{0x56}, std::byte{0x77}};
                reply.insert(reply.end(), header.value().begin(),
                             header.value().end());
                rudp::Frame frame{0x0230, std::move(reply)};
                auto wire = rudp::FrameCodec::encode({&frame, 1});
                if (wire) {
                  if (require_heartbeat_for_switch) {
                    withheld_switch_response_.assign(wire.value().begin(),
                                                     wire.value().end());
                    queue_heartbeat(0x5676);
                  } else {
                    response_.assign(wire.value().begin(), wire.value().end());
                    pending_ = true;
                    switch_response_sent_ = true;
                  }
                }
              }
            }
          }
        }
        if (type == 0x2430) {
          ++ack_sends_;
          const auto& payload = frames.value().front().payload;
          if (switch_response_sent_ && payload.size() == 6 &&
              payload[2] == std::byte{0x56} && payload[3] == std::byte{0x78} &&
              payload[4] == std::byte{} && payload[5] == std::byte{0x92})
            ++switch_ack_sends_;
          if (registration_responded_ && !ctrl_responded_ && ack_sends_ == 2 &&
              !pending_ && !registration_finish_queued_) {
            rudp::Frame finish{0xC000, {}};
            auto reply = rudp::FrameCodec::encode({&finish, 1});
            if (reply) {
              response_.assign(reply.value().begin(), reply.value().end());
              pending_ = true;
              registration_finish_queued_ = true;
            }
          }
          if (queue_session_id_ && ctrl_responded_ && payload.size() == 6 &&
              payload[2] == std::byte{0x56} && payload[3] == std::byte{0x7C} &&
              session_id_queued_ == 0 && !pending_) {
            auto header = libchiaking::net::protocol::remote_play::CtrlCodec::
                encode_frame_header(0x33, 25);
            if (header) {
              const unsigned pin_frames =
                  queue_pin_before_session_id_
                      ? 1 + std::min(extra_pin_requests, 1U)
                      : 0;
              rudp::Frame session_id{
                  0x0230,
                  {std::byte{0x56}, static_cast<std::byte>(0x7C + pin_frames)}};
              session_id.payload.insert(session_id.payload.end(),
                                        header.value().begin(),
                                        header.value().end());
              session_id.payload.push_back(std::byte{0x4A});
              for (int i = 0; i < 24; ++i)
                session_id.payload.push_back(std::byte{'A'});
              auto reply = rudp::FrameCodec::encode(
                  std::span<const rudp::Frame>{&session_id, 1});
              if (reply) {
                if (queue_pin_before_session_id_) {
                  auto pin_header = libchiaking::net::protocol::remote_play::
                      CtrlCodec::encode_frame_header(0x04, 1);
                  if (pin_header) {
                    rudp::Frame pin{0x0230, {std::byte{0x56}, std::byte{0x7C}}};
                    pin.payload.insert(pin.payload.end(),
                                       pin_header.value().begin(),
                                       pin_header.value().end());
                    pin.payload.push_back(std::byte{1});
                    auto pin_reply = rudp::FrameCodec::encode({&pin, 1});
                    if (pin_reply) {
                      response_.assign(pin_reply.value().begin(),
                                       pin_reply.value().end());
                      following_response_.emplace(reply.value().begin(),
                                                  reply.value().end());
                      if (pin_frames > 1) {
                        pin.payload[1] = std::byte{0x7D};
                        auto second = rudp::FrameCodec::encode({&pin, 1});
                        EXPECT_TRUE(second) << "the second pin frame encodes";
                        third_response_ = std::move(following_response_);
                        if (second)
                          following_response_.emplace(second.value().begin(),
                                                      second.value().end());
                      }
                      pending_ = true;
                      ++session_id_queued_;
                    }
                  }
                } else {
                  response_.assign(reply.value().begin(), reply.value().end());
                  pending_ = true;
                  ++session_id_queued_;
                }
              }
            }
          }
        }
        if (type == 0x8030) {
          ++rudp_sends_;
          if (refuse_rudp_init && rudp_sends_ == *refuse_rudp_init)
            pending_refusals = refusals_per_init;
          if (init_requests)
            init_requests->push_back(frames.value().front().payload);
          if (respond_to_init_) {
            rudp::Frame reply_frame{
                0xD000,
                {std::byte{0x56}, std::byte{0x78}, std::byte{}, std::byte{},
                 std::byte{}, std::byte{}, std::byte{}, std::byte{},
                 std::byte{0xC0}, std::byte{0x0E}}};
            auto reply = rudp::FrameCodec::encode(
                std::span<const rudp::Frame>{&reply_frame, 1});
            if (reply) {
              response_ = std::move(reply.value());
              pending_ = true;
              init_responded_ = true;
            }
          }
        } else if (type == 0x9030) {
          ++cookie_sends_;
          if (respond_to_cookie_ && init_responded_) {
            rudp::Frame reply_frame{0xA030, {std::byte{0x56}, std::byte{0x79}}};
            auto reply = rudp::FrameCodec::encode(
                std::span<const rudp::Frame>{&reply_frame, 1});
            if (reply) {
              response_.assign(reply.value().begin(), reply.value().end());
              pending_ = true;
            }
          }
        }
      }
    }
    return bytes.size();
  }
  auto connect(const platform::SocketEndpoint& peer)
      -> libchiaking::Expected<void, platform::NetError> override {
    if (!connect_peer)
      return libchiaking::unexpected(platform::NetError::not_supported);
    connected_peer = peer;
    return {};
  }
  auto send(std::span<const std::byte> bytes)
      -> libchiaking::Expected<std::size_t, platform::NetError> override {
    if (!connected_peer)
      return libchiaking::unexpected(platform::NetError::not_connected);
    ++connected_sends;
    return send_to(bytes, *connected_peer);
  }
  auto receive_from(std::span<std::byte> buffer)
      -> libchiaking::Expected<platform::UdpReceiveResult,
                               platform::NetError> override {
    if (pending_refusals > 0 && connected_peer) {
      --pending_refusals;
      return libchiaking::unexpected(platform::NetError::connection_refused);
    }
    if (fail_receive)
      return libchiaking::unexpected(platform::NetError::system_failure);
    if (!pending_ || buffer.size() < response_.size())
      return libchiaking::unexpected(platform::NetError::would_block);
    if (raw_queued) {
      raw_queued = false;
      raw_consumed_at = send_calls;
    }
    std::copy(response_.begin(), response_.end(), buffer.begin());
    const auto size = response_.size();
    if (following_response_) {
      response_ = std::move(*following_response_);
      following_response_ = std::move(third_response_);
      third_response_.reset();
    } else {
      response_.clear();
      pending_ = false;
    }
    return platform::UdpReceiveResult{
        size, {platform::IpAddress::loopback_ipv4(), 9999}, std::nullopt};
  }
  auto set_boolean_option(platform::SocketOption, bool)
      -> libchiaking::Expected<void, platform::NetError> override {
    return {};
  }
  auto set_receive_buffer_size(std::size_t)
      -> libchiaking::Expected<void, platform::NetError> override {
    return {};
  }
  auto local_endpoint() -> libchiaking::Expected<platform::SocketEndpoint,
                                                 platform::NetError> override {
    if (!bound_)
      return libchiaking::unexpected(platform::NetError::system_failure);
    return platform::SocketEndpoint{platform::IpAddress::loopback_ipv4(),
                                    40000};
  }
  auto close() -> libchiaking::Expected<void, platform::NetError> override {
    if (!closed_) {
      closed_ = true;
      ++closes_;
    }
    return {};
  }
  bool pending_{true};
  unsigned send_calls{};
  unsigned heartbeat_replies{};
  std::vector<std::vector<std::byte>> raw_sends;
  std::vector<std::pair<std::uint16_t, std::vector<std::byte>>> ctrl_sends;
  bool fail_heartbeat_send{}, fail_switch_send{};
  bool require_heartbeat_for_switch{};
  std::vector<std::byte> withheld_switch_response_;
  bool fail_receive{};
  bool connect_peer{};
  std::optional<unsigned> refuse_rudp_init;
  std::optional<platform::SocketEndpoint> connected_peer;
  unsigned connected_sends{};
  unsigned refusals_per_init{1};
  unsigned pending_refusals{};
  void queue_heartbeat(std::uint16_t counter) {
    rudp::Frame frame{
        0x0230,
        {static_cast<std::byte>(counter >> 8), static_cast<std::byte>(counter),
         std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{},
         std::byte{0xfe}, std::byte{}, std::byte{}}};
    const auto wire = test_support::Setup::value(
        rudp::FrameCodec::encode({&frame, 1}), "the frame encodes");
    response_.assign(wire.begin(), wire.end());
    pending_ = true;
  }
  void queue_raw(std::vector<std::byte> datagram) {
    response_ = std::move(datagram);
    pending_ = true;
    raw_queued = true;
  }
  bool raw_queued{};
  unsigned extra_pin_requests{};
  std::optional<unsigned> raw_consumed_at;
  std::string ctrl_request;

 private:
  bool queue_takion_control(const takion::ControlMessage& message,
                            std::uint32_t peer_sequence, std::uint16_t channel,
                            const takion::WireProfile& profile) {
    takion::ReliableChannelConfig config{
        .initial_local_sequence = peer_sequence,
        .initial_remote_sequence = 1,
        .maximum_message_size = 1024 * 1024,
        .maximum_fragment_size = 1200,
        .maximum_fragment_count = 1024,
        .retransmission_window_size = 256,
        .receive_reorder_size = 256,
        .retransmission_timeout_us = 100000,
        .maximum_retries = 5};
    takion::ReliableChannel peer =
        takion::ReliableChannel::create(config).value();
    takion::ControlCodec codec;
    auto encoded_message = codec.encode(message);
    if (!encoded_message) return false;
    auto sent = peer.send(channel, takion::MessageDataType{0},
                          encoded_message.value().bytes(), 0);
    if (!sent.has_value() || sent->fragments.size() != 1) return false;
    auto payload = takion::ControlChunkCodec::encode_packet(
        takion::ControlChunkPacket{takion::DataChunk{
            takion::ControlHeader{local_verification_tag_, {}, 0, 1},
            std::vector<std::byte>(sent->fragments[0].payload.begin(),
                                   sent->fragments[0].payload.end())}});
    if (!payload) return false;
    takion::PlainPacket packet{
        takion::PacketType{
            static_cast<std::uint8_t>(takion::PacketFamily::control)},
        std::nullopt,
        {payload.value().begin(), payload.value().end()}};
    auto encoded = takion::PacketCodec::encode_plain_packet(packet, profile);
    if (!encoded) return false;
    auto wire = takion::EnvelopeCodec::encode(encoded.value().bytes(),
                                              takion::PlainEnvelope{});
    if (!wire) return false;
    response_.assign(wire.value().begin(), wire.value().end());
    pending_ = true;
    return true;
  }

  bool queue_takion_ack(std::uint32_t sequence,
                        const takion::WireProfile& profile) {
    auto payload = takion::ControlChunkCodec::encode_packet(
        takion::ControlChunkPacket{takion::DataAcknowledgementChunk{
            takion::ControlHeader{local_verification_tag_, {}, 0, 0},
            takion::DataAcknowledgement{sequence, 0x19000, {}, {}}}});
    if (!payload) return false;
    takion::PlainPacket packet{
        takion::PacketType{
            static_cast<std::uint8_t>(takion::PacketFamily::control)},
        std::nullopt,
        {payload.value().begin(), payload.value().end()}};
    auto encoded = takion::PacketCodec::encode_plain_packet(packet, profile);
    if (!encoded) return false;
    auto wire = takion::EnvelopeCodec::encode(encoded.value().bytes(),
                                              takion::PlainEnvelope{});
    if (!wire) return false;
    response_.assign(wire.value().begin(), wire.value().end());
    pending_ = true;
    return true;
  }

  platform::NetworkProvider& owner_;
  unsigned& closes_;
  unsigned& probe_sends_;
  unsigned& rudp_sends_;
  unsigned& cookie_sends_;
  unsigned& registration_sends_;
  unsigned& ctrl_http_sends_;
  unsigned& ack_sends_;
  unsigned& session_id_queued_;
  unsigned& takion_init_sends_;
  unsigned& takion_cookie_sends_;
  unsigned& takion_data_sends_;
  unsigned& distinct_takion_data_sends_;
  unsigned& senkusha_control_sends_;
  unsigned& takion_big_sends_;
  unsigned& active_big_ack_sends_;
  unsigned& active_stream_info_sends_;
  unsigned& takion_probe_sends_;
  unsigned& client_mtu_sends_;
  unsigned& client_mtu_final_sends_;
  unsigned& switch_request_sends_;
  unsigned& switch_ack_sends_;
  bool respond_to_init_{};
  bool respond_to_cookie_{};
  bool respond_to_registration_{};
  bool respond_to_ctrl_{};
  bool queue_session_id_{};
  bool queue_pin_before_session_id_{};
  bool respond_to_takion_init_{};
  bool respond_to_takion_cookie_{};
  bool respond_to_takion_data_{};
  bool respond_to_takion_big_{};
  bool respond_to_senkusha_echo_{};
  bool respond_to_rtt_echo_{};
  bool respond_to_outbound_mtu_echo_{};
  bool respond_to_echo_disable_{};
  bool respond_to_inbound_mtu_{};
  bool respond_to_client_mtu_{};
  bool respond_to_client_mtu_final_{};
  bool respond_to_switch_request_{};
  bool registration_ps4_{};
  takion::VerificationTag local_verification_tag_{};
  bool takion_data_responded_{};
  bool bang_sent_{};
  bool senkusha_echo_acked_{};
  bool echo_disable_acked_{};
  bool inbound_mtu_responded_{};
  bool client_mtu_responded_{};
  bool client_mtu_final_acked_{};
  bool switch_response_sent_{};
  bool active_big_acked_{};
  unsigned rtt_echoes_{};
  bool outbound_mtu_echoed_{};
  std::optional<takion::ReliableChannel> active_receiver_;
  std::vector<std::byte> first_takion_data_payload_;
  bool init_responded_{};
  bool registration_responded_{};
  bool session_responded_{};
  bool registration_finish_queued_{};
  bool ctrl_responded_{};
  bool bound_{};
  bool closed_{};
  static constexpr std::array<std::byte, 32> stun_response_{
      std::byte{1},    std::byte{1},    std::byte{0},    std::byte{12},
      std::byte{0x21}, std::byte{0x12}, std::byte{0xa4}, std::byte{0x42},
      std::byte{},     std::byte{},     std::byte{},     std::byte{},
      std::byte{},     std::byte{},     std::byte{},     std::byte{},
      std::byte{},     std::byte{},     std::byte{},     std::byte{},
      std::byte{0},    std::byte{1},    std::byte{0},    std::byte{8},
      std::byte{0},    std::byte{1},    std::byte{0x9c}, std::byte{0x41},
      std::byte{203},  std::byte{0},    std::byte{113},  std::byte{7}};
  std::vector<std::byte> response_;
  std::optional<std::vector<std::byte>> following_response_;
  std::optional<std::vector<std::byte>> third_response_;

 public:
  std::vector<std::vector<std::byte>>* init_requests{};
  std::string ctrl_server_type;
};

class RouteSockets final : public platform::SocketProvider {
 public:
  explicit RouteSockets(platform::NetworkProvider& owner) : owner_(owner) {}
  auto create_udp_socket(const platform::IpAddress& address)
      -> libchiaking::Expected<std::unique_ptr<platform::UdpSocket>,
                               platform::NetError> override {
    if (address.is_ipv6()) ++ipv6_creates;
    auto socket = std::make_unique<RouteSocket>(
        owner_, closes, probe_sends, rudp_sends, cookie_sends,
        registration_http_sends, ctrl_http_sends, ack_sends, session_id_queued,
        takion_init_sends, takion_cookie_sends, takion_data_sends,
        distinct_takion_data_sends, senkusha_control_sends, takion_big_sends,
        active_big_ack_sends, active_stream_info_sends, takion_probe_sends,
        client_mtu_sends, client_mtu_final_sends, switch_request_sends,
        switch_ack_sends, respond_to_init, respond_to_cookie,
        respond_to_registration, respond_to_ctrl, queue_session_id,
        queue_pin_before_session_id, respond_to_takion_init,
        respond_to_takion_cookie, respond_to_takion_data, respond_to_takion_big,
        respond_to_senkusha_echo, respond_to_rtt_echo,
        respond_to_outbound_mtu_echo, respond_to_echo_disable,
        respond_to_inbound_mtu, respond_to_client_mtu,
        respond_to_client_mtu_final, respond_to_switch_request,
        registration_ps4);
    socket->init_requests = &rudp_init_requests;
    socket->ctrl_server_type = ctrl_server_type;
    socket->extra_pin_requests = extra_pin_requests;
    socket->connect_peer = connect_peer;
    socket->refuse_rudp_init = refuse_rudp_init;
    socket->refusals_per_init = refusals_per_init;
    if (!first) first = socket.get();
    last = socket.get();
    ++creates;
    return std::unique_ptr<platform::UdpSocket>{std::move(socket)};
  }
  auto create_tcp_socket(const platform::IpAddress&)
      -> libchiaking::Expected<std::unique_ptr<platform::TcpSocket>,
                               platform::NetError> override {
    return libchiaking::unexpected(platform::NetError::not_supported);
  }
  auto create_tcp_listener(const platform::IpAddress&)
      -> libchiaking::Expected<std::unique_ptr<platform::TcpListener>,
                               platform::NetError> override {
    return libchiaking::unexpected(platform::NetError::not_supported);
  }
  RouteSocket* first{};
  RouteSocket* last{};
  unsigned creates{};
  unsigned ipv6_creates{};
  unsigned closes{};
  unsigned probe_sends{};
  unsigned rudp_sends{};
  std::vector<std::vector<std::byte>> rudp_init_requests;
  std::string ctrl_server_type;
  unsigned extra_pin_requests{};
  bool connect_peer{};
  std::optional<unsigned> refuse_rudp_init;
  unsigned refusals_per_init{1};
  unsigned cookie_sends{};
  unsigned registration_http_sends{};
  unsigned ctrl_http_sends{};
  unsigned ack_sends{};
  unsigned session_id_queued{};
  unsigned takion_init_sends{};
  unsigned takion_cookie_sends{};
  unsigned takion_data_sends{};
  unsigned distinct_takion_data_sends{};
  unsigned senkusha_control_sends{};
  unsigned takion_big_sends{};
  unsigned active_big_ack_sends{};
  unsigned active_stream_info_sends{};
  unsigned takion_probe_sends{};
  unsigned client_mtu_sends{};
  unsigned client_mtu_final_sends{};
  unsigned switch_request_sends{};
  unsigned switch_ack_sends{};
  bool respond_to_init{};
  bool respond_to_cookie{};
  bool respond_to_registration{};
  bool respond_to_ctrl{};
  bool queue_session_id{};
  bool queue_pin_before_session_id{};
  bool respond_to_takion_init{};
  bool respond_to_takion_cookie{};
  bool respond_to_takion_data{};
  bool respond_to_takion_big{};
  bool respond_to_senkusha_echo{};
  bool respond_to_rtt_echo{};
  bool respond_to_outbound_mtu_echo{};
  bool respond_to_echo_disable{};
  bool respond_to_inbound_mtu{};
  bool respond_to_client_mtu{};
  bool respond_to_client_mtu_final{};
  bool respond_to_switch_request{};
  bool registration_ps4{};
  bool allow_idle_poll{};

 private:
  platform::NetworkProvider& owner_;
};

class RoutePoller final : public platform::Poller {
 public:
  RoutePoller(platform::NetworkProvider& owner, RouteSockets& sockets)
      : owner_(owner), sockets_(sockets) {}
  auto owner() const noexcept -> const platform::NetworkProvider& override {
    return owner_;
  }
  auto wake() -> libchiaking::Expected<void, platform::NetError> override {
    return {};
  }
  auto wait(std::span<const platform::PollRequest> requests,
            std::chrono::nanoseconds timeout, const platform::StopToken&)
      -> libchiaking::Expected<platform::PollWaitResult,
                               platform::NetError> override {
    platform::PollWaitResult result{};
    for (const auto& request : requests) {
      if ((request.pollable == sockets_.last && sockets_.last &&
           sockets_.last->pending_) ||
          (request.pollable == sockets_.first && sockets_.first &&
           sockets_.first->pending_))
        result.ready.push_back({request.tag, platform::PollEvent::readable});
    }
    if (!result.ready.empty()) {
      idled_ = false;
      return result;
    }
    if (!sockets_.allow_idle_poll &&
        (timeout != std::chrono::seconds{1} || idled_))
      return libchiaking::unexpected(platform::NetError::system_failure);
    idled_ = timeout == std::chrono::seconds{1};
    return result;
  }

 private:
  platform::NetworkProvider& owner_;
  RouteSockets& sockets_;
  bool idled_{};
};

class RoutePollers final : public platform::PollerProvider {
 public:
  RoutePollers(platform::NetworkProvider& owner, RouteSockets& sockets)
      : owner_(owner), sockets_(sockets) {}
  auto create_poller()
      -> libchiaking::Expected<std::unique_ptr<platform::Poller>,
                               platform::NetError> override {
    return std::unique_ptr<platform::Poller>{new RoutePoller(owner_, sockets_)};
  }

 private:
  platform::NetworkProvider& owner_;
  RouteSockets& sockets_;
};

class RouteDns final : public platform::DnsProvider {
 public:
  std::function<void()> on_stun;
  auto resolve(std::string_view host, std::string_view)
      -> libchiaking::Expected<std::vector<platform::IpAddress>,
                               platform::NetError> override {
    if (host == "preferred.example" && on_stun) on_stun();
    if (host == "preferred.example" || host == "127.0.0.1")
      return std::vector{platform::IpAddress::loopback_ipv4()};
    return libchiaking::unexpected(platform::NetError::host_unreachable);
  }
};

class RouteInterfaces final : public platform::InterfacesProvider {
 public:
  auto enumerate()
      -> libchiaking::Expected<std::vector<platform::InterfaceInfo>,
                               platform::NetError> override {
    platform::InterfaceInfo info{};
    info.address = platform::IpAddress::ipv4({192, 168, 1, 42});
    info.operational = true;
    return std::vector{info};
  }
};

class RouteNetwork final : public platform::NetworkProvider {
 public:
  RouteNetwork() : sockets_(*this), pollers_(*this, sockets_) {}
  auto sockets() noexcept -> platform::SocketProvider& override {
    return sockets_;
  }
  auto pollers() noexcept -> platform::PollerProvider& override {
    return pollers_;
  }
  auto dns() noexcept -> platform::DnsProvider& override { return dns_; }
  auto interfaces() noexcept -> platform::InterfacesProvider& override {
    return interfaces_;
  }
  auto capabilities() const noexcept -> platform::NetworkCapabilities override {
    return {};
  }
  RouteSockets sockets_;
  RouteDns dns_;

 private:
  RoutePollers pollers_;
  RouteInterfaces interfaces_;
};

}  // namespace public_route_fixture
