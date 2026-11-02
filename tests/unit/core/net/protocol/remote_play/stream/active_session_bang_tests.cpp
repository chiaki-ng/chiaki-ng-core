#include "tests/unit/core/net/protocol/remote_play/stream/active_session_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;
using remote_play::ActiveHandshakeState;
using remote_play::ActiveSessionError;
using Family = takion::PacketFamily;

class ActiveSessionHandshake : public ::testing::Test {
 protected:
  void begin(std::uint32_t version = 12,
             takion::ControllerConnectionType type =
                 takion::ControllerConnectionType::dualsense,
             bool idr_on_fec_failure = false,
             bool single_slot_mailbox = false) {
    rig.emplace(version);
    controller_type = type;
    takion::ClientConfig config;
    if (single_slot_mailbox) config.limits.mailbox_capacity = 1;
    ASSERT_NO_FATAL_FAILURE(
        rig->start({type,
                    remote_play::AudioAssemblyPolicy::cloud_generation,
                    {},
                    false,
                    idr_on_fec_failure},
                   config));
  }

  auto session() -> remote_play::ActiveSession& { return *rig->owner; }
  auto wire() -> ActiveSessionTransportState& { return *rig->state; }
  void advance(std::uint64_t microseconds) {
    rig->clock.now_us += microseconds;
  }

  void connect() {
    rig->answer_handshake(true);
    ASSERT_TRUE(session().pump_once());
    ASSERT_EQ(session().state(), ActiveHandshakeState::awaiting_bang);
    ASSERT_TRUE(session().pump_once());
  }

  void acknowledge_sent() {
    std::uint32_t sequence = 0;
    for (const auto& sent : wire().sent)
      if (const auto data =
              ActiveSessionWire::data_chunk(sent, rig->profile, false);
          data && data->payload.size() >= 4)
        sequence = ActiveSessionWire::sequence_of(data->payload);
    wire().incoming.push_back(rig->inbound_acknowledgement(sequence));
    ASSERT_TRUE(session().pump_once());
  }

  void expect_handshake_timeout(ActiveHandshakeState stage) {
    ASSERT_NO_FATAL_FAILURE(acknowledge_sent());
    advance(4'999'999);
    ASSERT_TRUE(session().pump_once());
    EXPECT_FALSE(session().failure());
    advance(1);
    const auto timed_out = session().pump_once();
    ASSERT_FALSE(timed_out);
    EXPECT_EQ(timed_out.error(), ActiveSessionError::handshake_timeout);
    ASSERT_TRUE(session().failure());
    EXPECT_EQ(session().failure()->stage, stage);
    EXPECT_TRUE(session().request_stop());
    ASSERT_TRUE(session().failure());
    EXPECT_EQ(session().failure()->stage, stage);
    EXPECT_EQ(wire().close_count, 1U);
  }

  void expect_controller_refused_before_ready() {
    EXPECT_EQ(session().snapshot().queued_commands, 0U);
    remote_play::ControllerState controller;
    const auto state_post = session().post_controller_state(controller);
    ASSERT_FALSE(state_post);
    EXPECT_EQ(state_post.error(), ActiveSessionError::not_ready);
    const auto history_post =
        session().post_controller_history(controller, controller);
    ASSERT_FALSE(history_post);
    EXPECT_EQ(history_post.error(), ActiveSessionError::not_ready);
    EXPECT_EQ(session().snapshot().queued_commands, 0U);
  }

  auto deliver_bang(bool tamper, std::optional<bool> early_info_valid = {}) {
    if (early_info_valid)
      wire().incoming.push_back(ActiveSessionWire::inbound_message(
          ActiveSessionWire::stream_info(*early_info_valid), 0xa1b2c3d4,
          rig->profile));
    wire().incoming.push_back(ActiveSessionWire::inbound_message(
        ActiveSessionWire::signed_bang(tamper),
        early_info_valid ? 0xa1b2c3d5 : 0xa1b2c3d4, rig->profile));
    return session().pump_once();
  }

  void reach_bang(bool tamper, std::optional<bool> early_info_valid = {}) {
    ASSERT_NO_FATAL_FAILURE(connect());
    ASSERT_NO_FATAL_FAILURE(expect_controller_refused_before_ready());
    ASSERT_TRUE(deliver_bang(tamper, early_info_valid));
  }

  void expect_bang_ignored() {
    EXPECT_EQ(rig->pair->derive_calls, 0U);
    EXPECT_EQ(session().state(), ActiveHandshakeState::awaiting_bang);
    EXPECT_FALSE(session().snapshot().crypto_installed);
    EXPECT_EQ(wire().close_count, 0U);
    EXPECT_FALSE(session().failure());
  }

  void expect_bang_accepted() {
    EXPECT_EQ(rig->pair->derive_calls, 1U);
    EXPECT_EQ(session().state(), ActiveHandshakeState::awaiting_crypto_install);
    EXPECT_EQ(session().snapshot().queued_commands, 0U);
    EXPECT_TRUE(session().snapshot().crypto_installed);
  }

  void expect_ready_with_info(bool with_resolutions) {
    EXPECT_EQ(session().state(), ActiveHandshakeState::ready);
    ASSERT_NE(session().ready_info(), nullptr);
    EXPECT_EQ(session().ready_info()->audio.channels, 2U);
    EXPECT_EQ(session().ready_info()->audio.bits, 16U);
    if (with_resolutions) {
      EXPECT_EQ(session().ready_info()->resolutions.size(), 1U);
    }
    EXPECT_EQ(session().snapshot().queued_commands, 0U);
  }

  void expect_post_info_controls(std::size_t start) {
    std::vector<std::pair<std::uint16_t, takion::ControlMessage>> controls;
    const takion::ControlCodec codec;
    for (std::size_t index = start; index < wire().sent.size(); ++index) {
      const auto plain = takion::PacketCodec::decode_plain_packet(
          wire().sent[index], rig->profile);
      ASSERT_TRUE(plain) << index;
      const auto chunk =
          takion::ControlChunkCodec::decode_packet(plain.value().payload);
      ASSERT_TRUE(chunk) << index;
      if (const auto* data = std::get_if<takion::DataChunk>(&chunk.value())) {
        ASSERT_GE(data->payload.size(), takion::first_fragment_overhead);
        const auto channel = static_cast<std::uint16_t>(
            std::to_integer<std::uint16_t>(data->payload[4]) << 8U |
            std::to_integer<std::uint16_t>(data->payload[5]));
        auto message =
            codec.decode(std::span<const std::byte>{data->payload}.subspan(
                takion::first_fragment_overhead));
        ASSERT_TRUE(message) << index;
        controls.emplace_back(channel, std::move(message.value()));
      }
    }
    ASSERT_EQ(controls.size(), 3U);
    EXPECT_EQ(controls[0].first, 9U);
    EXPECT_TRUE(std::holds_alternative<takion::StreamInfoAcknowledgement>(
        controls[0].second));
    EXPECT_EQ(controls[1].first, 1U);
    const auto* connection =
        std::get_if<takion::ControllerConnection>(&controls[1].second);
    ASSERT_NE(connection, nullptr);
    EXPECT_FALSE(connection->controller_id);
    EXPECT_EQ(connection->connected, true);
    EXPECT_TRUE(connection->controller_type ==
                takion::ControllerConnectionTypeValue{controller_type});
    EXPECT_EQ(controls[2].first, 1U);
    const auto* microphone =
        std::get_if<takion::StreamInfo>(&controls[2].second);
    ASSERT_NE(microphone, nullptr);
    const std::vector<std::byte> expected_header{
        std::byte{0x01}, std::byte{0x10}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xbb}, std::byte{0x80}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x01}, std::byte{0xe0}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x01}};
    EXPECT_EQ(microphone->audio_header, expected_header);
    EXPECT_TRUE(microphone->resolutions.empty());
    EXPECT_TRUE(microphone->audio_channels.empty());
    EXPECT_FALSE(microphone->start_timeout);
    EXPECT_FALSE(microphone->afk_timeout);
    EXPECT_FALSE(microphone->afk_timeout_disconnect);
    EXPECT_FALSE(microphone->congestion_control_interval);
  }

  void reach_ready(std::uint32_t version = 12,
                   takion::ControllerConnectionType type =
                       takion::ControllerConnectionType::dualsense,
                   bool idr_on_fec_failure = false) {
    ASSERT_NO_FATAL_FAILURE(begin(version, type, idr_on_fec_failure));
    ASSERT_NO_FATAL_FAILURE(reach_bang(false, true));
    ASSERT_NO_FATAL_FAILURE(expect_bang_accepted());
    const auto installed_start = wire().sent.size();
    ASSERT_TRUE(session().pump_once());
    EXPECT_TRUE(session().snapshot().crypto_installed);
    ASSERT_NO_FATAL_FAILURE(expect_ready_with_info(true));
    ASSERT_NO_FATAL_FAILURE(expect_post_info_controls(installed_start));
  }

  void expect_stop_closes_once() {
    const auto snapshot = session().snapshot();
    const auto expect_disconnect =
        snapshot.state == takion::ClientState::connected &&
        snapshot.profile_committed && snapshot.crypto_installed;
    const auto sent_before_stop = wire().sent.size();
    EXPECT_TRUE(session().request_stop());
    EXPECT_EQ(wire().sent.size(),
              sent_before_stop + (expect_disconnect ? 1U : 0U));
    EXPECT_EQ(wire().close_count, 1U);
  }

  void expect_stop_after_one_close() {
    EXPECT_TRUE(session().request_stop());
    EXPECT_EQ(wire().close_count, 1U);
  }

  void enqueue_version_nine(Family family, std::vector<std::byte> payload) {
    const bool video = family == Family::video;
    if (video) payload.insert(payload.begin() + 20, 3, std::byte{});
    rig->enqueue_av(family, std::move(payload),
                    static_cast<std::uint8_t>(video ? 0x10U : 0U));
  }

  void enqueue_media(Family family, std::byte marker, std::uint8_t flags = 0,
                     std::uint8_t frame = 1) {
    std::vector<std::byte> payload(32);
    payload[3] = std::byte{frame};
    payload[8] = family == Family::video ? std::byte{1} : std::byte{5};
    if (family == Family::audio_and_haptics && flags) {
      payload[18] = std::byte{2};
      flags = 0;
    }
    payload.back() = marker;
    rig->enqueue_av(family, std::move(payload), flags);
  }

  auto count_data(std::size_t start, std::size_t size,
                  std::span<const std::byte> body) const -> std::size_t {
    std::size_t count = 0;
    for (std::size_t index = start; index < rig->state->sent.size(); ++index) {
      const auto data =
          ActiveSessionWire::data_chunk(rig->state->sent[index], rig->profile);
      if (!data || data->payload.size() != size) continue;
      if (!std::equal(body.begin(), body.end(), data->payload.begin() + 9))
        continue;
      EXPECT_EQ(data->payload[4], std::byte{0}) << index;
      EXPECT_EQ(data->payload[5], std::byte{2}) << index;
      ++count;
    }
    return count;
  }

  void expect_version_nine_media() {
    std::vector<std::byte> video0(23);
    video0[3] = std::byte{1};
    video0[6] = std::byte{4};
    video0[8] = std::byte{1};
    video0[22] = std::byte{0x51};
    auto video1 = video0;
    video1[1] = std::byte{1};
    video1[5] = std::byte{0x20};
    video1[22] = std::byte{0x52};
    std::vector<std::byte> audio0(19);
    audio0[3] = std::byte{10};
    audio0[5] = std::byte{1};
    audio0[8] = std::byte{5};
    audio0[18] = std::byte{0xa1};
    auto audio1 = audio0;
    audio1[3] = std::byte{11};
    audio1[4] = std::byte{1};
    audio1[18] = std::byte{0xa2};
    auto rejected_audio = audio0;
    rejected_audio[8] = std::byte{9};
    ASSERT_NO_FATAL_FAILURE(
        enqueue_version_nine(Family::audio_and_haptics, rejected_audio));
    ASSERT_TRUE(session().pump_once());
    EXPECT_EQ(session().rejected_media_units(), 1U);
    EXPECT_TRUE(session().diagnostics().available);
    EXPECT_EQ(session().diagnostics().assembly_rejections, 1U);
    EXPECT_EQ(session().diagnostics().video_units, 0U);
    EXPECT_EQ(session().state(), ActiveHandshakeState::ready);
    EXPECT_FALSE(session().take_media_event());

    ASSERT_NO_FATAL_FAILURE(enqueue_version_nine(Family::video, video0));
    ASSERT_NO_FATAL_FAILURE(
        enqueue_version_nine(Family::audio_and_haptics, audio0));
    ASSERT_TRUE(session().pump_once());
    const auto profile_header = session().take_media_event();
    ASSERT_TRUE(profile_header);
    EXPECT_TRUE(std::get<takion::client_event::EncodedVideo>(*profile_header)
                    .profile_header);
    EXPECT_FALSE(session().take_media_event());

    ASSERT_NO_FATAL_FAILURE(enqueue_version_nine(Family::video, video1));
    ASSERT_NO_FATAL_FAILURE(
        enqueue_version_nine(Family::audio_and_haptics, audio1));
    ASSERT_TRUE(session().pump_once());
    const auto video = session().take_media_event();
    EXPECT_EQ(session().diagnostics().video_units, 2U);
    EXPECT_EQ(session().diagnostics().assembled_video, 1U);
    ASSERT_TRUE(video);
    ASSERT_TRUE(
        std::holds_alternative<takion::client_event::EncodedVideo>(*video));
    EXPECT_EQ(std::get<takion::client_event::EncodedVideo>(*video).bytes,
              std::vector<std::byte>({std::byte{0x51}, std::byte{0x52}}));
    EXPECT_FALSE(session().take_media_event());

    std::vector<std::byte> fec_video_source(23);
    fec_video_source[1] = std::byte{2};
    fec_video_source[3] = std::byte{2};
    fec_video_source[6] = std::byte{8};
    fec_video_source[7] = std::byte{1};
    fec_video_source[8] = std::byte{1};
    fec_video_source[22] = std::byte{0x51};
    auto fec_video_parity = fec_video_source;
    fec_video_parity[1] = std::byte{4};
    fec_video_parity[5] = std::byte{0x40};
    fec_video_parity[22] = std::byte{0x78};
    ASSERT_NO_FATAL_FAILURE(
        enqueue_version_nine(Family::video, fec_video_source));
    ASSERT_NO_FATAL_FAILURE(
        enqueue_version_nine(Family::video, fec_video_parity));
    ASSERT_TRUE(session().pump_once());
    EXPECT_FALSE(session().take_media_event());
    advance(16'001);
    ASSERT_TRUE(session().pump_once());
    const auto recovered_video = session().take_media_event();
    ASSERT_TRUE(recovered_video);
    ASSERT_TRUE(std::holds_alternative<takion::client_event::EncodedVideo>(
        *recovered_video));
    const auto& recovered_video_value =
        std::get<takion::client_event::EncodedVideo>(*recovered_video);
    EXPECT_EQ(recovered_video_value.bytes,
              std::vector<std::byte>({std::byte{0x51}, std::byte{0x52}}));
    EXPECT_EQ(recovered_video_value.frame_index, 2U);
    EXPECT_EQ(recovered_video_value.codec, 1U);
    EXPECT_EQ(recovered_video_value.adaptive_stream_index, 0U);
    EXPECT_FALSE(session().take_media_event());

    auto incomplete = video0;
    incomplete[1] = std::byte{5};
    incomplete[3] = std::byte{3};
    auto following = video0;
    following[1] = std::byte{9};
    following[3] = std::byte{5};
    following[6] = std::byte{0};
    const auto recovery_start = wire().sent.size();
    ASSERT_NO_FATAL_FAILURE(enqueue_version_nine(Family::video, incomplete));
    ASSERT_NO_FATAL_FAILURE(enqueue_version_nine(Family::video, following));
    ASSERT_TRUE(session().pump_once());
    advance(16'001);
    ASSERT_TRUE(session().pump_once());
    EXPECT_TRUE(session().take_media_event());
    ASSERT_TRUE(session().pump_once());
    const std::array expected_loss{
        std::byte{0x08}, std::byte{0x05}, std::byte{0x32}, std::byte{0x04},
        std::byte{0x08}, std::byte{0x03}, std::byte{0x10}, std::byte{0x04}};
    std::size_t found_recovery = 0;
    for (std::size_t i = recovery_start; i < wire().sent.size(); ++i) {
      const auto data =
          ActiveSessionWire::data_chunk(wire().sent[i], rig->profile, false);
      if (!data || data->payload.size() != 9 + expected_loss.size()) continue;
      if (!std::equal(expected_loss.begin(), expected_loss.end(),
                      data->payload.begin() + 9))
        continue;
      EXPECT_EQ(data->payload[4], std::byte{0}) << i;
      EXPECT_EQ(data->payload[5], std::byte{2}) << i;
      ++found_recovery;
    }
    EXPECT_GT(found_recovery, 0U);
    advance(200'000);
    ASSERT_TRUE(session().pump_once());
    ASSERT_TRUE(session().pump_once());
    const std::array expected_counts{std::byte{0},  std::byte{0}, std::byte{0},
                                     std::byte{21}, std::byte{0}, std::byte{1}};
    const auto congestion = rig->congestion_packets();
    EXPECT_FALSE(congestion.empty());
    for (const auto& packet : congestion)
      EXPECT_TRUE(std::equal(expected_counts.begin(), expected_counts.end(),
                             packet.begin()));

    std::vector<std::byte> fec_audio_source(19);
    fec_audio_source[3] = std::byte{12};
    fec_audio_source[5] = std::byte{2};
    fec_audio_source[7] = std::byte{1};
    fec_audio_source[8] = std::byte{5};
    fec_audio_source[18] = std::byte{0xa1};
    auto fec_audio_parity = fec_audio_source;
    fec_audio_parity[3] = std::byte{14};
    fec_audio_parity[4] = std::byte{2};
    fec_audio_parity[18] = std::byte{0xf0};
    ASSERT_NO_FATAL_FAILURE(
        enqueue_version_nine(Family::audio_and_haptics, fec_audio_source));
    ASSERT_NO_FATAL_FAILURE(
        enqueue_version_nine(Family::audio_and_haptics, fec_audio_parity));
    ASSERT_TRUE(session().pump_once());
    for (const auto marker :
         {std::byte{0xa1}, std::byte{0xa2}, std::byte{0xa1}, std::byte{0xa2}}) {
      const auto recovered_audio = session().take_media_event();
      ASSERT_TRUE(recovered_audio);
      ASSERT_TRUE(std::holds_alternative<takion::client_event::EncodedAudio>(
          *recovered_audio));
      EXPECT_EQ(
          std::get<takion::client_event::EncodedAudio>(*recovered_audio).bytes,
          std::vector<std::byte>({marker}));
    }
    EXPECT_FALSE(session().take_media_event());
    ASSERT_NO_FATAL_FAILURE(enqueue_version_nine(Family::video, video0));
    ASSERT_TRUE(session().pump_once());
    EXPECT_EQ(session().state(), ActiveHandshakeState::ready);
    EXPECT_FALSE(session().take_media_event());
    EXPECT_TRUE(session().request_stop());
    EXPECT_EQ(wire().close_count, 1U);
  }

  void send_late_stream_info(bool corrupt_mac) {
    ASSERT_NO_FATAL_FAILURE(begin());
    ASSERT_NO_FATAL_FAILURE(reach_bang(false));
    ASSERT_NO_FATAL_FAILURE(expect_bang_accepted());
    ASSERT_TRUE(session().pump_once());
    EXPECT_TRUE(session().snapshot().crypto_installed);
    EXPECT_EQ(session().state(), ActiveHandshakeState::awaiting_stream_info);
    EXPECT_FALSE(session().ready_info());
    auto authenticated = ActiveSessionWire::inbound_message(
        ActiveSessionWire::stream_info(true), 0xa1b2c3d5, rig->profile);
    ASSERT_NO_FATAL_FAILURE(rig->install_sender());
    ASSERT_NO_FATAL_FAILURE(rig->protect(authenticated, Family::control));
    if (corrupt_mac) {
      const auto offsets =
          takion::CryptoContext::packet_offsets(Family::control, rig->profile);
      ASSERT_TRUE(offsets);
      authenticated[offsets.value().mac] ^= std::byte{1};
    }
    wire().incoming.push_back(std::move(authenticated));
    control_start = wire().sent.size();
    ASSERT_TRUE(session().pump_once());
  }

  std::optional<ActiveSessionRig> rig;
  takion::ControllerConnectionType controller_type{
      takion::ControllerConnectionType::dualsense};
  std::size_t control_start{};
};

TEST_F(ActiveSessionHandshake, TimesOutWhenTheConnectIsNeverAnswered) {
  ASSERT_NO_FATAL_FAILURE(begin());
  ASSERT_TRUE(session().pump_once());
  advance(6'000'000);
  const auto timed_out = session().pump_once();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), ActiveSessionError::handshake_timeout);
  ASSERT_TRUE(session().failure());
  EXPECT_EQ(session().failure()->stage,
            ActiveHandshakeState::awaiting_connection);
}

TEST_F(ActiveSessionHandshake, TimesOutWhenNoBangFollowsTheAcknowledgedBig) {
  ASSERT_NO_FATAL_FAILURE(begin());
  ASSERT_NO_FATAL_FAILURE(connect());
  expect_handshake_timeout(ActiveHandshakeState::awaiting_bang);
}

TEST_F(ActiveSessionHandshake, KeepsThePeerReasonWhenDisconnectedBeforeBang) {
  ASSERT_NO_FATAL_FAILURE(begin());
  ASSERT_NO_FATAL_FAILURE(connect());
  wire().incoming.push_back(ActiveSessionWire::inbound_message(
      takion::Disconnect{"reference peer closed", std::nullopt}, 0xa1b2c3d4,
      rig->profile));
  const auto disconnected = session().pump_once();
  ASSERT_FALSE(disconnected);
  EXPECT_EQ(disconnected.error(), ActiveSessionError::peer_disconnected);
  ASSERT_TRUE(session().failure());
  EXPECT_EQ(session().failure()->stage, ActiveHandshakeState::awaiting_bang);
  EXPECT_EQ(session().failure()->peer_reason, "reference peer closed");
  EXPECT_TRUE(session().request_stop());
  EXPECT_EQ(wire().close_count, 1U);
}

TEST_F(ActiveSessionHandshake, ValidBangInstallsCryptoAndWaitsForStreamInfo) {
  ASSERT_NO_FATAL_FAILURE(begin());
  ASSERT_NO_FATAL_FAILURE(reach_bang(false));
  ASSERT_NO_FATAL_FAILURE(expect_bang_accepted());
  ASSERT_TRUE(session().pump_once());
  EXPECT_TRUE(session().snapshot().crypto_installed);
  expect_stop_closes_once();
  EXPECT_EQ(rig->crypto.hmac_calls, 1U);
}

TEST_F(ActiveSessionHandshake, TimesOutWhenNoStreamInfoFollowsTheBang) {
  ASSERT_NO_FATAL_FAILURE(begin());
  ASSERT_NO_FATAL_FAILURE(reach_bang(false));
  ASSERT_NO_FATAL_FAILURE(expect_bang_accepted());
  ASSERT_TRUE(session().pump_once());
  EXPECT_TRUE(session().snapshot().crypto_installed);
  expect_handshake_timeout(ActiveHandshakeState::awaiting_stream_info);
}

TEST_F(ActiveSessionHandshake, TamperedBangFailsAuthenticationAtTheDeadline) {
  ASSERT_NO_FATAL_FAILURE(begin());
  ASSERT_NO_FATAL_FAILURE(reach_bang(true));
  ASSERT_NO_FATAL_FAILURE(expect_bang_ignored());
  advance(4'999'999);
  ASSERT_TRUE(session().pump_once());
  EXPECT_FALSE(session().failure());
  advance(1);
  const auto timed_out = session().pump_once();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), ActiveSessionError::bang_authentication_failed);
  EXPECT_EQ(session().state(), ActiveHandshakeState::failed);
  EXPECT_EQ(wire().close_count, 1U);
  const auto repeated = session().pump_once();
  ASSERT_FALSE(repeated);
  EXPECT_EQ(repeated.error(), ActiveSessionError::bang_authentication_failed);
  ASSERT_TRUE(session().failure());
  EXPECT_EQ(session().failure()->stage, ActiveHandshakeState::awaiting_bang);
  EXPECT_EQ(rig->crypto.hmac_calls, 1U);
}

TEST_F(ActiveSessionHandshake, TamperedBangIsIgnoredUntilAValidBangArrives) {
  ASSERT_NO_FATAL_FAILURE(begin());
  ASSERT_NO_FATAL_FAILURE(reach_bang(true));
  ASSERT_NO_FATAL_FAILURE(expect_bang_ignored());
  wire().incoming.push_back(ActiveSessionWire::inbound_message(
      ActiveSessionWire::signed_bang(false), 0xa1b2c3d5, rig->profile));
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig->pair->derive_calls, 1U);
  EXPECT_EQ(session().state(), ActiveHandshakeState::awaiting_crypto_install);
  ASSERT_TRUE(session().pump_once());
  EXPECT_TRUE(session().snapshot().crypto_installed);
  EXPECT_FALSE(session().failure());
}

TEST_F(ActiveSessionHandshake, PeerDisconnectAfterATamperedBangKeepsItsReason) {
  ASSERT_NO_FATAL_FAILURE(begin());
  ASSERT_NO_FATAL_FAILURE(reach_bang(true));
  ASSERT_NO_FATAL_FAILURE(expect_bang_ignored());
  wire().incoming.push_back(ActiveSessionWire::inbound_message(
      takion::Disconnect{"Server shutting down", std::nullopt}, 0xa1b2c3d5,
      rig->profile));
  const auto disconnected = session().pump_once();
  ASSERT_FALSE(disconnected);
  EXPECT_EQ(disconnected.error(), ActiveSessionError::peer_disconnected);
  ASSERT_TRUE(session().failure());
  EXPECT_EQ(session().failure()->peer_reason, "Server shutting down");
}

TEST_F(ActiveSessionHandshake,
       InvalidEarlyStreamInfoFailsAtTheStreamInfoDeadline) {
  ASSERT_NO_FATAL_FAILURE(begin());
  ASSERT_NO_FATAL_FAILURE(reach_bang(false, false));
  ASSERT_NO_FATAL_FAILURE(expect_bang_accepted());
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(session().state(), ActiveHandshakeState::awaiting_stream_info);
  EXPECT_EQ(session().ready_info(), nullptr);
  EXPECT_EQ(wire().close_count, 0U);
  advance(4'999'999);
  ASSERT_TRUE(session().pump_once());
  EXPECT_FALSE(session().failure());
  advance(1);
  const auto rejected = session().pump_once();
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), ActiveSessionError::handshake_failed);
  ASSERT_TRUE(session().failure());
  EXPECT_EQ(session().failure()->stage,
            ActiveHandshakeState::awaiting_stream_info);
  EXPECT_EQ(session().state(), ActiveHandshakeState::failed);
  EXPECT_EQ(session().snapshot().queued_commands, 0U);
  EXPECT_EQ(wire().close_count, 1U);
  EXPECT_EQ(rig->crypto.hmac_calls, 1U);
}

TEST_F(ActiveSessionHandshake, InvalidEarlyStreamInfoWaitsForAValidOne) {
  ASSERT_NO_FATAL_FAILURE(begin());
  ASSERT_NO_FATAL_FAILURE(reach_bang(false, false));
  ASSERT_NO_FATAL_FAILURE(expect_bang_accepted());
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(session().state(), ActiveHandshakeState::awaiting_stream_info);
  EXPECT_EQ(session().ready_info(), nullptr);
  EXPECT_EQ(wire().close_count, 0U);
  wire().incoming.push_back(ActiveSessionWire::inbound_message(
      ActiveSessionWire::stream_info(true), 0xa1b2c3d6, rig->profile));
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(session().state(), ActiveHandshakeState::ready);
  EXPECT_TRUE(session().ready_info());
}

TEST_F(ActiveSessionHandshake,
       StreamInfoStaysPendingWhileTheMailboxIsFullUntilThePostFails) {
  ASSERT_NO_FATAL_FAILURE(
      begin(12, takion::ControllerConnectionType::dualsense, false, true));
  ASSERT_NO_FATAL_FAILURE(reach_bang(false, true));
  ASSERT_NO_FATAL_FAILURE(expect_bang_accepted());
  const auto installed_start = wire().sent.size();
  ASSERT_TRUE(session().pump_once());
  EXPECT_TRUE(session().snapshot().crypto_installed);
  const auto count_acks = [&] {
    std::size_t count = 0;
    for (std::size_t index = installed_start; index < wire().sent.size();
         ++index)
      if (const auto control =
              ActiveSessionWire::sent_control(wire().sent[index], rig->profile);
          control && std::holds_alternative<takion::StreamInfoAcknowledgement>(
                         control->second))
        ++count;
    return count;
  };
  EXPECT_EQ(session().state(), ActiveHandshakeState::awaiting_stream_info);
  EXPECT_FALSE(session().ready_info());
  EXPECT_FALSE(session().failure());
  EXPECT_EQ(wire().close_count, 0U);
  EXPECT_EQ(count_acks(), 1U);
  wire().incoming.push_back(ActiveSessionWire::inbound_message(
      ActiveSessionWire::stream_info(true), 0xa1b2c3d6, rig->profile));
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(count_acks(), 2U);
  EXPECT_EQ(session().state(), ActiveHandshakeState::awaiting_stream_info);
  EXPECT_FALSE(session().failure());
  advance(4'999'999);
  ASSERT_TRUE(session().pump_once());
  EXPECT_FALSE(session().failure());
  advance(1);
  const auto timed_out = session().pump_once();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), ActiveSessionError::post_failed);
  ASSERT_TRUE(session().failure());
  EXPECT_EQ(session().failure()->stage,
            ActiveHandshakeState::awaiting_stream_info);
  EXPECT_EQ(wire().close_count, 1U);
}

TEST_F(ActiveSessionHandshake,
       LateAuthenticatedStreamInfoMakesTheSessionReady) {
  ASSERT_NO_FATAL_FAILURE(send_late_stream_info(false));
  ASSERT_NO_FATAL_FAILURE(expect_ready_with_info(false));
  ASSERT_NO_FATAL_FAILURE(expect_post_info_controls(control_start));
  expect_stop_closes_once();
  EXPECT_EQ(rig->crypto.hmac_calls, 1U);
}

TEST_F(ActiveSessionHandshake, LateStreamInfoWithABadMacIsIgnored) {
  ASSERT_NO_FATAL_FAILURE(send_late_stream_info(true));
  EXPECT_EQ(session().state(), ActiveHandshakeState::awaiting_stream_info);
  EXPECT_FALSE(session().ready_info());
  EXPECT_EQ(session().snapshot().queued_commands, 0U);
  expect_stop_closes_once();
  EXPECT_EQ(rig->crypto.hmac_calls, 1U);
}

TEST_F(ActiveSessionHandshake,
       ReadySessionPacesCongestionReportsAndHeartbeats) {
  ASSERT_NO_FATAL_FAILURE(reach_ready());
  ASSERT_NO_FATAL_FAILURE(acknowledge_sent());
  const auto count_heartbeat = [&] {
    std::size_t count = 0;
    for (const auto& sent : wire().sent) {
      const auto data =
          ActiveSessionWire::data_chunk(sent, rig->profile, false);
      if (!data || data->payload.size() != 11) continue;
      if (data->payload[9] == std::byte{0x08} &&
          data->payload[10] == std::byte{0x03}) {
        EXPECT_EQ(data->payload[4], std::byte{0});
        EXPECT_EQ(data->payload[5], std::byte{1});
        ++count;
      }
    }
    return count;
  };
  const auto count_congestion = [&] {
    const auto packets = rig->congestion_packets();
    for (const auto& packet : packets) {
      EXPECT_EQ(packet.size(), 14U);
      for (std::size_t i = 0; i < 6 && i < packet.size(); ++i)
        EXPECT_EQ(packet[i], std::byte{0}) << i;
    }
    return packets.size();
  };
  advance(199'999);
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(count_congestion(), 0U);
  advance(1);
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(count_congestion(), 1U);
  advance(200'000);
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(count_congestion(), 2U);
  advance(599'999);
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(count_heartbeat(), 0U);
  advance(1);
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(count_heartbeat(), 1U);
  ASSERT_NO_FATAL_FAILURE(acknowledge_sent());
  advance(1'000'000);
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(count_heartbeat(), 2U);
  EXPECT_TRUE(session().request_stop());
  const auto sends = wire().sent.size();
  advance(1'000'000);
  EXPECT_FALSE(session().pump_once());
  EXPECT_EQ(wire().sent.size(), sends);
  EXPECT_EQ(wire().close_count, 1U);
}

TEST_F(ActiveSessionHandshake,
       StopClosesOnceEvenWhenTheDisconnectCannotBeSent) {
  ASSERT_NO_FATAL_FAILURE(reach_ready());
  wire().fail_send = true;
  EXPECT_TRUE(session().request_stop());
  EXPECT_FALSE(session().stop_failure());
  EXPECT_EQ(wire().close_count, 1U);
  EXPECT_EQ(session().state(), ActiveHandshakeState::closed);
  EXPECT_TRUE(session().request_stop());
  EXPECT_EQ(wire().close_count, 1U);
}

TEST_F(ActiveSessionHandshake, AuthenticatedPeerDisconnectClosesAfterOneReply) {
  ASSERT_NO_FATAL_FAILURE(reach_ready());
  ASSERT_NO_FATAL_FAILURE(rig->install_sender());
  auto peer_closed = ActiveSessionWire::inbound_message(
      takion::Disconnect{"reference peer closed", std::nullopt}, 0xa1b2c3d6,
      rig->profile);
  ASSERT_NO_FATAL_FAILURE(rig->protect(peer_closed, Family::control));
  wire().incoming.push_back(std::move(peer_closed));
  const auto sends = wire().sent.size();
  const auto disconnected = session().pump_once();
  ASSERT_FALSE(disconnected);
  EXPECT_EQ(disconnected.error(), ActiveSessionError::peer_disconnected);
  EXPECT_EQ(wire().close_count, 1U);
  std::size_t disconnects = 0;
  for (std::size_t index = sends; index < wire().sent.size(); ++index)
    if (ActiveSessionWire::data_chunk(wire().sent[index], rig->profile))
      ++disconnects;
  EXPECT_EQ(disconnects, 1U);
  ASSERT_NO_FATAL_FAILURE(expect_stop_after_one_close());
}

TEST_F(ActiveSessionHandshake,
       VideoBacklogKeepsTheNewestFrameAndReportsTheLoss) {
  ASSERT_NO_FATAL_FAILURE(reach_ready(9));
  ASSERT_NO_FATAL_FAILURE(rig->install_sender());
  for (unsigned index = 1; index <= 65; ++index) {
    std::vector<std::byte> payload(23);
    payload[1] = static_cast<std::byte>(index);
    payload[3] = static_cast<std::byte>(index);
    payload[22] = std::byte{0x51};
    ASSERT_NO_FATAL_FAILURE(
        enqueue_version_nine(Family::video, std::move(payload)));
  }
  const auto before = wire().sent.size();
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  const auto header = session().take_media_event();
  ASSERT_TRUE(header);
  EXPECT_TRUE(
      std::get<takion::client_event::EncodedVideo>(*header).profile_header);
  const auto first = session().take_media_event();
  ASSERT_TRUE(first);
  EXPECT_EQ(std::get<takion::client_event::EncodedVideo>(*first).frame_index,
            65U);
  EXPECT_FALSE(session().take_media_event());
  const std::array expected{std::byte{8},    std::byte{5}, std::byte{0x32},
                            std::byte{4},    std::byte{8}, std::byte{1},
                            std::byte{0x10}, std::byte{64}};
  EXPECT_GT(count_data(before, 9 + expected.size(), expected), 0U);
  ASSERT_NO_FATAL_FAILURE(expect_stop_after_one_close());
}

TEST_F(ActiveSessionHandshake,
       FecFailureRequestsAnIdrOnlyAfterTheReorderWindow) {
  ASSERT_NO_FATAL_FAILURE(
      reach_ready(9, takion::ControllerConnectionType::dualsense, true));
  ASSERT_NO_FATAL_FAILURE(rig->install_sender());
  const auto before = wire().sent.size();
  for (unsigned index = 1; index <= 2; ++index) {
    std::vector<std::byte> payload(23);
    payload[1] = static_cast<std::byte>(index);
    payload[3] = static_cast<std::byte>(index);
    payload[5] = std::byte{0x20};
    payload[6] = std::byte{4};
    payload[22] = std::byte{0x51};
    ASSERT_NO_FATAL_FAILURE(
        enqueue_version_nine(Family::video, std::move(payload)));
  }
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  advance(16'000);
  ASSERT_TRUE(session().pump_once());
  const std::array idr{std::byte{8}, std::byte{25}};
  EXPECT_EQ(count_data(before, 11, idr), 0U);
  advance(1);
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(count_data(before, 11, idr), 1U);
  ASSERT_NO_FATAL_FAILURE(expect_stop_after_one_close());
}

TEST_F(ActiveSessionHandshake, VersionNineAssemblesMediaForADualSense) {
  ASSERT_NO_FATAL_FAILURE(reach_ready(9));
  ASSERT_NO_FATAL_FAILURE(rig->install_sender());
  expect_version_nine_media();
}

TEST_F(ActiveSessionHandshake, VersionNineAssemblesMediaForADualShock4) {
  ASSERT_NO_FATAL_FAILURE(
      reach_ready(9, takion::ControllerConnectionType::dualshock4));
  ASSERT_NO_FATAL_FAILURE(rig->install_sender());
  expect_version_nine_media();
}

TEST_F(ActiveSessionHandshake,
       ReadySessionDeliversMediaAndSendsControllerStateUntilStopped) {
  ASSERT_NO_FATAL_FAILURE(reach_ready());
  ASSERT_NO_FATAL_FAILURE(rig->install_sender());
  ASSERT_NO_FATAL_FAILURE(enqueue_media(Family::video, std::byte{0x51}));
  for (std::uint8_t frame = 1; frame <= 3; ++frame)
    ASSERT_NO_FATAL_FAILURE(
        enqueue_media(Family::audio_and_haptics, std::byte{0x52}, 0, frame));
  ASSERT_NO_FATAL_FAILURE(
      enqueue_media(Family::audio_and_haptics, std::byte{0x53}, 0x10));
  ASSERT_TRUE(session().pump_once());
  const auto profile_header = session().take_media_event();
  ASSERT_TRUE(profile_header);
  EXPECT_TRUE(std::get<takion::client_event::EncodedVideo>(*profile_header)
                  .profile_header);
  const auto video = session().take_media_event();
  const auto audio = session().take_media_event();
  for (int buffered = 0; buffered < 2; ++buffered) {
    const auto later = session().take_media_event();
    ASSERT_TRUE(later) << buffered;
    EXPECT_EQ(std::get<takion::client_event::EncodedAudio>(*later).bytes.back(),
              std::byte{0x52});
  }
  const auto haptics = session().take_media_event();
  ASSERT_TRUE(video);
  ASSERT_TRUE(
      std::holds_alternative<takion::client_event::EncodedVideo>(*video));
  ASSERT_TRUE(audio);
  ASSERT_TRUE(
      std::holds_alternative<takion::client_event::EncodedAudio>(*audio));
  ASSERT_TRUE(haptics);
  ASSERT_TRUE(std::holds_alternative<takion::client_event::Haptics>(*haptics));
  EXPECT_EQ(std::get<takion::client_event::EncodedVideo>(*video).bytes.back(),
            std::byte{0x51});
  EXPECT_EQ(std::get<takion::client_event::EncodedAudio>(*audio).bytes.back(),
            std::byte{0x52});
  EXPECT_EQ(std::get<takion::client_event::Haptics>(*haptics).bytes.back(),
            std::byte{0x53});
  EXPECT_FALSE(session().take_media_event());

  remote_play::ControllerState controller;
  const auto sent_before_feedback = wire().sent.size();
  EXPECT_TRUE(session().post_controller_state(controller));
  EXPECT_TRUE(session().post_controller_state(controller));
  EXPECT_EQ(session().snapshot().queued_commands, 2U);
  controller.controller_id = 4;
  const auto invalid_controller = session().post_controller_state(controller);
  ASSERT_FALSE(invalid_controller);
  EXPECT_EQ(invalid_controller.error(),
            ActiveSessionError::invalid_controller_state);
  EXPECT_EQ(session().snapshot().queued_commands, 2U);
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(session().snapshot().queued_commands, 0U);
  ASSERT_EQ(wire().sent.size(), sent_before_feedback + 2);
  for (std::size_t i = 0; i < 2; ++i) {
    const auto& sent = wire().sent[sent_before_feedback + i];
    ASSERT_GE(sent.size(), 3U) << i;
    EXPECT_EQ(sent[0], std::byte{0x06}) << i;
    EXPECT_EQ(sent[1], std::byte{0x00}) << i;
    EXPECT_EQ(sent[2], static_cast<std::byte>(i)) << i;
  }
  controller.controller_id = 0;
  EXPECT_TRUE(session().post_controller_history(controller, controller));
  EXPECT_EQ(session().snapshot().queued_commands, 0U);
  remote_play::ControllerState invalid_history = controller;
  invalid_history.controller_id = 4;
  const auto bad_history =
      session().post_controller_history(invalid_history, controller);
  ASSERT_FALSE(bad_history);
  EXPECT_EQ(bad_history.error(), ActiveSessionError::invalid_controller_state);
  controller.controller_id = 2;
  remote_play::ControllerState changed = controller;
  changed.buttons = 1;
  const auto sent_before_history = wire().sent.size();
  EXPECT_TRUE(session().post_controller_history(controller, changed));
  EXPECT_TRUE(session().post_controller_history(changed, controller));
  EXPECT_EQ(session().snapshot().queued_commands, 2U);
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(session().snapshot().queued_commands, 0U);
  ASSERT_EQ(wire().sent.size(), sent_before_history + 2);
  for (std::size_t i = 0; i < 2; ++i) {
    const auto& sent = wire().sent[sent_before_history + i];
    ASSERT_GE(sent.size(), 13U) << i;
    EXPECT_EQ(sent[0], std::byte{0x01}) << i;
    EXPECT_EQ(sent[1], std::byte{0x00}) << i;
    EXPECT_EQ(sent[2], static_cast<std::byte>(i)) << i;
    EXPECT_EQ(sent[3], std::byte{0x00}) << i;
    EXPECT_EQ(sent[12], std::byte{0x82}) << i;
  }
  EXPECT_TRUE(session().post_controller_history_events(2, {}));
  EXPECT_EQ(session().snapshot().queued_commands, 0U);
  const auto invalid_events = session().post_controller_history_events(4, {});
  ASSERT_FALSE(invalid_events);
  EXPECT_EQ(invalid_events.error(),
            ActiveSessionError::invalid_controller_state);
  const auto sent_before_direct = wire().sent.size();
  EXPECT_TRUE(session().post_controller_history_events(
      2, {takion::FeedbackDigitalButtonEvent{
             takion::FeedbackDigitalButton::cross, true}}));
  EXPECT_EQ(session().snapshot().queued_commands, 1U);
  ASSERT_TRUE(session().pump_once());
  ASSERT_EQ(wire().sent.size(), sent_before_direct + 1);
  EXPECT_EQ(wire().sent.back()[0], std::byte{0x01});
  EXPECT_EQ(wire().sent.back()[2], std::byte{0x02});
  ASSERT_NO_FATAL_FAILURE(enqueue_media(Family::video, std::byte{0x54}));
  ASSERT_TRUE(session().pump_once());
  const auto sent_before_stop = wire().sent.size();
  EXPECT_TRUE(session().request_stop());
  EXPECT_EQ(wire().sent.size(), sent_before_stop + 1);
  EXPECT_EQ(wire().close_count, 1U);
  EXPECT_TRUE(session().request_stop());
  EXPECT_EQ(wire().sent.size(), sent_before_stop + 1);
  EXPECT_EQ(wire().close_count, 1U);
  const auto after_stop = session().post_controller_state(controller);
  ASSERT_FALSE(after_stop);
  EXPECT_EQ(after_stop.error(), ActiveSessionError::not_ready);
  const auto history_after_stop =
      session().post_controller_history(controller, changed);
  ASSERT_FALSE(history_after_stop);
  EXPECT_EQ(history_after_stop.error(), ActiveSessionError::not_ready);
  EXPECT_FALSE(session().take_media_event());
  expect_stop_closes_once();
  EXPECT_EQ(rig->crypto.hmac_calls, 1U);
}

class ActiveSessionBangSessionKey : public ::testing::Test {
 protected:
  void deliver_bang_with_key(remote_play::BangSessionKeyPolicy policy,
                             std::string bang_session_key) {
    remote_play::ActiveControlProfile control;
    control.bang_session_key = policy;
    ASSERT_NO_FATAL_FAILURE(rig.start(control));
    rig.owner->set_diagnostics(&trace);
    rig.answer_handshake();
    ASSERT_TRUE(rig.owner->pump_once());
    ASSERT_TRUE(rig.owner->pump_once());
    ASSERT_EQ(rig.owner->state(), ActiveHandshakeState::awaiting_bang);
    auto bang = ActiveSessionWire::signed_bang(false);
    bang.session_key = libchiaking::net::protocol::takion::SharedSecretText{
        std::move(bang_session_key)};
    rig.state->incoming.push_back(ActiveSessionWire::inbound_message(
        std::move(bang), 0xa1b2c3d4, rig.profile));
    ASSERT_TRUE(rig.owner->pump_once());
  }

  auto mismatch_reported() const -> bool {
    return std::ranges::any_of(sink.messages, [](const std::string& message) {
      return message.find("session key does not match") != std::string::npos;
    });
  }

  void expect_accepted(bool reported) {
    EXPECT_EQ(rig.pair->derive_calls, 1U);
    EXPECT_EQ(rig.owner->state(),
              ActiveHandshakeState::awaiting_crypto_install);
    EXPECT_EQ(mismatch_reported(), reported);
    EXPECT_FALSE(rig.owner->failure());
    EXPECT_TRUE(rig.owner->request_stop());
  }

  ActiveSessionRig rig{12};
  ActiveSessionLogSink sink;
  const libchiaking::diagnostics::Trace trace{
      &sink, rig.clock, libchiaking::diagnostics::Component::local};
};

TEST_F(ActiveSessionBangSessionKey, RequiredPolicyAcceptsTheMatchingKey) {
  ASSERT_NO_FATAL_FAILURE(deliver_bang_with_key(
      remote_play::BangSessionKeyPolicy::required, "session-id"));
  expect_accepted(false);
}

TEST_F(ActiveSessionBangSessionKey, RequiredPolicyRejectsAForeignKey) {
  ASSERT_NO_FATAL_FAILURE(deliver_bang_with_key(
      remote_play::BangSessionKeyPolicy::required, "console-own-id"));
  EXPECT_EQ(rig.pair->derive_calls, 0U);
  EXPECT_EQ(rig.owner->state(), ActiveHandshakeState::awaiting_bang);
  EXPECT_FALSE(mismatch_reported());
  rig.clock.now_us += 5'000'000;
  const auto timed_out = rig.owner->pump_once();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), ActiveSessionError::bang_authentication_failed);
  ASSERT_TRUE(rig.owner->failure());
  EXPECT_EQ(rig.owner->failure()->stage, ActiveHandshakeState::awaiting_bang);
  EXPECT_TRUE(rig.owner->request_stop());
}

TEST_F(ActiveSessionBangSessionKey,
       ReportingPolicyAcceptsTheMatchingKeyQuietly) {
  ASSERT_NO_FATAL_FAILURE(deliver_bang_with_key(
      remote_play::BangSessionKeyPolicy::report_mismatch, "session-id"));
  expect_accepted(false);
}

TEST_F(ActiveSessionBangSessionKey,
       ReportingPolicyAcceptsAForeignKeyAndLogsIt) {
  ASSERT_NO_FATAL_FAILURE(deliver_bang_with_key(
      remote_play::BangSessionKeyPolicy::report_mismatch, "console-own-id"));
  expect_accepted(true);
}

}  // namespace
}  // namespace remote_play_test
