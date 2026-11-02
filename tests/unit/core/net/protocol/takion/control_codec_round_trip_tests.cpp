#include "src/core/net/protocol/takion/control_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"
#include "tests/unit/core/net/protocol/takion/control_codec_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionControlCodecRoundTrips : public TakionControlCodecTest {};

TEST_F(TakionControlCodecRoundTrips, MatchIndependentlyBuiltWireForEveryField) {
  const auto big = round_trip(full_big(), expected_full_big());
  const auto bang = round_trip(full_bang_with_acknowledgement(),
                               expected_full_bang_with_acknowledgement());
  const auto stream_info =
      round_trip(full_stream_info(), expected_full_stream_info());
  const auto mtu = round_trip(takion::Senkusha{full_mtu_command()},
                              expected_full_mtu_command());
  const auto bandwidth = round_trip(takion::Senkusha{full_bandwidth_command()},
                                    expected_full_bandwidth_command());
  const auto client_mtu =
      round_trip(takion::Senkusha{full_client_mtu_command()},
                 expected_full_client_mtu_command());
  const auto quality =
      round_trip(full_connection_quality(), expected_full_connection_quality());

  takion::ControllerConnection controller;
  controller.controller_id = -1;
  controller.connected = true;
  controller.controller_type = takion::ControllerConnectionType::dualsense;
  const auto controller_wire =
      round_trip(controller, expected_full_controller_connection());

  const auto request = round_trip(takion::ProtocolRequest{{7, 20}},
                                  expected_full_protocol_request());

  const ByteVector full_field_vectors[] = {
      big,        bang,    stream_info,     mtu,     bandwidth,
      client_mtu, quality, controller_wire, request,
  };
  for (const auto& wire : full_field_vectors) {
    EXPECT_TRUE(prefixes_never_decode_partially(wire));
  }
}

TEST_F(TakionControlCodecRoundTrips, EncodeHeartbeatsAndDisconnectsExactly) {
  const auto heartbeat =
      round_trip(takion::Heartbeat{}, TakionCodecRig::bytes({0x08, 0x03}));

  const takion::Disconnect disconnect{"bye", std::nullopt};
  const auto ordinary_disconnect = round_trip(
      disconnect, TakionCodecRig::bytes(
                      {0x08, 0x08, 0x52, 0x05, 0x0a, 0x03, 0x62, 0x79, 0x65}));

  const takion::Disconnect extended_disconnect{
      "bye", takion::DisconnectEventCode{"E", "N", "D"}};
  const auto extended_disconnect_wire = round_trip(
      extended_disconnect,
      TakionCodecRig::bytes({0x08, 0x08, 0x52, 0x10, 0x0a, 0x03, 0x62,
                             0x79, 0x65, 0x12, 0x09, 0x0a, 0x01, 0x45,
                             0x12, 0x01, 0x4e, 0x1a, 0x01, 0x44}));
  EXPECT_FALSE(decoded_as<takion::Disconnect>(ordinary_disconnect)
                   .extended_info.has_value());

  EXPECT_TRUE(no_strict_prefix_decodes(heartbeat));
  EXPECT_TRUE(no_strict_prefix_decodes(ordinary_disconnect));
  EXPECT_TRUE(no_strict_prefix_decodes(extended_disconnect_wire));
}

TEST_F(TakionControlCodecRoundTrips, EncodeBigAndBangExactly) {
  takion::Big minimal_big;
  minimal_big.client_version = 9;
  const auto big = round_trip(
      minimal_big, TakionCodecRig::bytes({0x08, 0x00, 0x12, 0x08, 0x08, 0x09,
                                          0x12, 0x00, 0x1a, 0x00, 0x22, 0x00}));
  round_trip(full_big());

  takion::Bang minimal_bang;
  minimal_bang.server_version = 12;
  minimal_bang.token = 34;
  minimal_bang.encrypted_key_accepted = true;
  minimal_bang.version_accepted = true;
  minimal_bang.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{"s"};
  const auto bang = round_trip(
      minimal_bang,
      TakionCodecRig::bytes({0x08, 0x01, 0x1a, 0x0b, 0x08, 0x0c, 0x10, 0x22,
                             0x18, 0x01, 0x20, 0x01, 0x2a, 0x01, 0x73}));
  round_trip(full_bang());

  EXPECT_TRUE(no_strict_prefix_decodes(big));
  EXPECT_TRUE(no_strict_prefix_decodes(bang));
}

TEST_F(TakionControlCodecRoundTrips,
       EncodeStreamInfoAndItsAcknowledgementExactly) {
  takion::StreamInfo minimal_stream;
  minimal_stream.audio_header = TakionCodecRig::bytes({0xaa});
  const auto stream_info = round_trip(
      minimal_stream,
      TakionCodecRig::bytes({0x08, 0x0d, 0x7a, 0x03, 0x12, 0x01, 0xaa}));
  round_trip(full_stream_info());

  const auto stream_info_ack = round_trip(takion::StreamInfoAcknowledgement{},
                                          TakionCodecRig::bytes({0x08, 0x0e}));

  EXPECT_TRUE(no_strict_prefix_decodes(stream_info));
  EXPECT_TRUE(no_strict_prefix_decodes(stream_info_ack));
}

TEST_F(TakionControlCodecRoundTrips,
       EncodeCorruptFramesAndConnectionQualityExactly) {
  const auto corrupt_frame =
      round_trip(takion::CorruptFrame{258, 259},
                 TakionCodecRig::bytes({0x08, 0x05, 0x32, 0x06, 0x08, 0x82,
                                        0x02, 0x10, 0x83, 0x02}));

  takion::ConnectionQuality minimal_quality;
  minimal_quality.target_bitrate = 1000;
  const auto quality =
      round_trip(minimal_quality,
                 TakionCodecRig::bytes(
                     {0x08, 0x10, static_cast<std::uint8_t>(0x8a), 0x01, 0x03,
                      0x08, static_cast<std::uint8_t>(0xe8), 0x07}));
  round_trip(full_connection_quality());

  EXPECT_TRUE(no_strict_prefix_decodes(corrupt_frame));
  EXPECT_TRUE(no_strict_prefix_decodes(quality));
}

TEST_F(TakionControlCodecRoundTrips, CarryPlayTimeAndServerMessages) {
  round_trip(takion::PlayTimeLeft{5, takion::PlayTimeReason::session_limit});
  round_trip(takion::PlayTimeLeft{30, takion::PlayTimeReason::maintenance});
  round_trip(takion::PlayTimeLeft{1, takion::PlayTimeReason::other});
  ProtoWire unknown_reason;
  unknown_reason.varint(1, 3);
  unknown_reason.string(2, "Unlisted");
  const auto unlisted = ProtoWire::control(18, 19, unknown_reason);
  const auto left = decoded_as<takion::PlayTimeLeft>(unlisted);
  EXPECT_EQ(left.minutes, 3);
  EXPECT_EQ(left.reason, takion::PlayTimeReason::other);

  round_trip(takion::ServerMessage{R"({"type":"trophy"})"});
  ProtoWire server_text;
  server_text.string(1, "notice");
  const auto server = decoded_as<takion::ServerMessage>(
      ProtoWire::control(19, 20, server_text));
  EXPECT_EQ(server.text, "notice");
}

TEST_F(TakionControlCodecRoundTrips, EncodeXmbCommandsExactly) {
  round_trip(takion::XmbCommand{takion::XmbCommand::Command::toggle, 0},
             TakionCodecRig::bytes({0x08, 0x0f, static_cast<std::uint8_t>(0x82),
                                    0x01, 0x04, 0x08, 0x02, 0x10, 0x00}));
  round_trip(takion::XmbCommand{takion::XmbCommand::Command::off, std::nullopt},
             TakionCodecRig::bytes({0x08, 0x0f, static_cast<std::uint8_t>(0x82),
                                    0x01, 0x02, 0x08, 0x00}));
  round_trip(takion::XmbCommand{takion::XmbCommand::Command::on, 3});
}

TEST_F(TakionControlCodecRoundTrips, CarryDeepLinksAndTheMetricsOverlay) {
  takion::DeepLink overlaid;
  overlaid.request_id = 7;
  overlaid.raw_data = "psaction:systemservicestatus?isSystemUiOverlaid=1";
  const auto overlaid_wire = round_trip(overlaid);
  std::string overlaid_text(reinterpret_cast<const char*>(overlaid_wire.data()),
                            overlaid_wire.size());
  EXPECT_EQ(overlaid_wire[1], std::byte{29});
  EXPECT_NE(
      overlaid_text.find("psaction:systemservicestatus?isSystemUiOverlaid=1"),
      std::string::npos);
  EXPECT_NE(overlaid_text.find(std::string{"\x10\xe8\x07", 3}),
            std::string::npos);
  takion::MetricsOverlay measured;
  measured.available_down_kbps = 48000;
  measured.used_down_kbps = 15200;
  measured.used_up_kbps = 310;
  measured.round_trip_ms = 24;
  measured.loss_percent = 0.5F;
  measured.audio_channels = 2;
  measured.dynamic_range = "SDR";
  measured.video_codec = "HEVC";
  measured.width = 1920;
  measured.height = 1080;
  round_trip(measured);
  const auto empty_overlay = decoded_as<takion::MetricsOverlay>(
      ProtoWire::control(35, 36, ByteVector{}));
  EXPECT_EQ(empty_overlay, takion::MetricsOverlay{});
  EXPECT_FALSE(codec.decode(TakionCodecRig::bytes({0x08, 35})));

  takion::DeepLink invite;
  invite.type = takion::DeepLink::Type::invite;
  invite.invitation_id = "inv";
  invite.session_id = "ses";
  round_trip(invite);
}

TEST_F(TakionControlCodecRoundTrips, CarryMicrophoneConnectionAndInfo) {
  round_trip(takion::MicConnection{0, true, std::nullopt},
             TakionCodecRig::bytes({0x08, 0x1e, static_cast<std::uint8_t>(0xf2),
                                    0x01, 0x04, 0x08, 0x00, 0x10, 0x01}));
  round_trip(takion::MicConnection{0, true, false});
  takion::MicInfo muted;
  muted.controller_id = 0;
  muted.muted = true;
  const auto muted_wire = round_trip(muted);
  std::string muted_text(reinterpret_cast<const char*>(muted_wire.data()),
                         muted_wire.size());
  EXPECT_EQ(muted_wire[1], std::byte{29});
  EXPECT_NE(muted_text.find(std::string{"\x10\xe8\x07", 3}), std::string::npos);
  takion::MicInfo answered;
  answered.muted = false;
  answered.result = false;
  round_trip(answered);
}

TEST_F(TakionControlCodecRoundTrips, EncodeControllerConnectionsExactly) {
  const takion::ControllerConnection explicit_zero_controller{
      0, false, takion::ControllerConnectionType::not_set};
  const auto controller = round_trip(
      explicit_zero_controller,
      TakionCodecRig::bytes({0x08, 0x15, static_cast<std::uint8_t>(0xb2), 0x01,
                             0x06, 0x08, 0x00, 0x10, 0x00, 0x18, 0x00}));
  const takion::ControllerConnection absent_controller{};
  const auto absent_controller_wire = round_trip(
      absent_controller,
      TakionCodecRig::bytes(
          {0x08, 0x15, static_cast<std::uint8_t>(0xb2), 0x01, 0x00}));
  EXPECT_FALSE(decoded_as<takion::ControllerConnection>(absent_controller_wire)
                   .controller_type.has_value());
  round_trip(takion::ControllerConnection{-1, std::nullopt, std::nullopt});

  EXPECT_TRUE(no_strict_prefix_decodes(controller));
}

TEST_F(TakionControlCodecRoundTrips, EncodeAnIdrRequestExactly) {
  const auto idr_request =
      round_trip(takion::IdrRequest{}, TakionCodecRig::bytes({0x08, 0x19}));

  EXPECT_TRUE(no_strict_prefix_decodes(idr_request));
}

TEST_F(TakionControlCodecRoundTrips, RoundTripTheLargestFieldValues) {
  const auto max_uint32 = std::numeric_limits<std::uint32_t>::max();
  takion::Big maximum_big;
  maximum_big.client_version = max_uint32;
  round_trip(maximum_big);
  takion::Bang maximum_bang;
  maximum_bang.server_version = max_uint32;
  maximum_bang.token = max_uint32;
  maximum_bang.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{"max"};
  round_trip(maximum_bang);
  round_trip(takion::CorruptFrame{max_uint32, max_uint32});
  round_trip(takion::Senkusha{takion::SenkushaMtuCommand{
      max_uint32, max_uint32, max_uint32, max_uint32, max_uint32, max_uint32}});
  round_trip(takion::Senkusha{takion::SenkushaBandwidthCommand{
      max_uint32, max_uint32, max_uint32, max_uint32, max_uint32, max_uint32,
      max_uint32, std::nullopt, std::nullopt}});
  round_trip(takion::Senkusha{takion::SenkushaClientMtuCommand{
      max_uint32, max_uint32, true, max_uint32}});
  takion::ConnectionQuality maximum_quality;
  maximum_quality.target_bitrate = max_uint32;
  maximum_quality.upstream_bitrate = max_uint32;
  maximum_quality.loss = std::numeric_limits<std::uint64_t>::max();
  round_trip(maximum_quality);
  round_trip(takion::ProtocolRequest{{max_uint32, max_uint32}});
}

TEST_F(TakionControlCodecRoundTrips, EncodeSenkushaCommandsExactly) {
  const auto echo =
      round_trip(takion::Senkusha{takion::SenkushaEchoCommand{false}},
                 TakionCodecRig::bytes({0x08, 0x0c, 0x72, 0x06, 0x08, 0x00,
                                        0x12, 0x02, 0x08, 0x00}));
  round_trip(takion::Senkusha{takion::SenkushaEchoCommand{true}});

  const auto mtu = round_trip(
      takion::Senkusha{takion::SenkushaMtuCommand{
          1, 2, std::nullopt, std::nullopt, std::nullopt, std::nullopt}},
      TakionCodecRig::bytes({0x08, 0x0c, 0x72, 0x08, 0x08, 0x01, 0x1a, 0x04,
                             0x08, 0x01, 0x10, 0x02}));
  round_trip(takion::Senkusha{full_mtu_command()});

  const auto bandwidth =
      round_trip(takion::Senkusha{takion::SenkushaBandwidthCommand{
                     1, 2, std::nullopt, std::nullopt, std::nullopt,
                     std::nullopt, std::nullopt, std::nullopt, std::nullopt}},
                 TakionCodecRig::bytes({0x08, 0x0c, 0x72, 0x08, 0x08, 0x03,
                                        0x22, 0x04, 0x08, 0x01, 0x10, 0x02}));
  round_trip(takion::Senkusha{full_bandwidth_command()});

  const auto client_mtu = round_trip(
      takion::Senkusha{
          takion::SenkushaClientMtuCommand{1, 2, false, std::nullopt}},
      TakionCodecRig::bytes({0x08, 0x0c, 0x72, 0x0a, 0x08, 0x04, 0x2a, 0x06,
                             0x08, 0x01, 0x10, 0x02, 0x18, 0x00}));
  round_trip(takion::Senkusha{full_client_mtu_command()});

  EXPECT_TRUE(no_strict_prefix_decodes(echo));
  EXPECT_TRUE(no_strict_prefix_decodes(mtu));
  EXPECT_TRUE(no_strict_prefix_decodes(bandwidth));
  EXPECT_TRUE(no_strict_prefix_decodes(client_mtu));
}

TEST_F(TakionControlCodecRoundTrips, EncodeProtocolNegotiationExactly) {
  const auto request = round_trip(
      takion::ProtocolRequest{{7, 20}},
      TakionCodecRig::bytes({0x08, 0x1f, static_cast<std::uint8_t>(0xfa), 0x01,
                             0x04, 0x08, 0x07, 0x08, 0x14}));
  const auto acknowledgement = round_trip(
      takion::ProtocolRequestAck{20},
      TakionCodecRig::bytes({0x08, 0x20, static_cast<std::uint8_t>(0x82), 0x02,
                             0x02, 0x08, 0x14}));
  const auto empty_acknowledgement = round_trip(
      takion::ProtocolRequestAck{std::nullopt},
      TakionCodecRig::bytes(
          {0x08, 0x20, static_cast<std::uint8_t>(0x82), 0x02, 0x00}));
  EXPECT_FALSE(decoded_as<takion::ProtocolRequestAck>(empty_acknowledgement)
                   .selected_version.has_value());

  EXPECT_TRUE(no_strict_prefix_decodes(request));
  EXPECT_TRUE(no_strict_prefix_decodes(acknowledgement));
}

TEST_F(TakionControlCodecRoundTrips, CarryTheStreamInfoTosReportFields) {
  auto value = full_stream_info();
  value.tos_value = 0xb8;
  value.tos_report_packet_threshold = 0;
  value.tos_report_interval_ms = 250;

  auto body = expected_full_stream_info_payload();
  body.varint(8, 0xb8);
  body.varint(9, 0);
  body.varint(10, 250);
  ProtoWire expected;
  expected.varint(1, 13);
  expected.message(15, body);
  const auto wire = round_trip(value, expected);
  EXPECT_TRUE(prefixes_never_decode_partially(wire));

  const auto decoded = codec.decode(wire);
  ASSERT_TRUE(decoded);
  const auto info = std::get<takion::StreamInfo>(decoded.value());
  EXPECT_EQ(info.tos_value, std::optional<std::uint32_t>{0xb8});
  EXPECT_EQ(info.tos_report_packet_threshold, std::optional<std::uint32_t>{0});
  EXPECT_EQ(info.tos_report_interval_ms, std::optional<std::uint32_t>{250});
}

TEST_F(TakionControlCodecRoundTrips, LeaveTheTosReportFieldsAbsentByDefault) {
  const auto absent = codec.decode(expected_full_stream_info());
  ASSERT_TRUE(absent);
  const auto without = std::get<takion::StreamInfo>(absent.value());
  EXPECT_FALSE(without.tos_value);
  EXPECT_FALSE(without.tos_report_packet_threshold);
  EXPECT_FALSE(without.tos_report_interval_ms);
}

TEST_F(TakionControlCodecRoundTrips, EncodeAudioStateExactly) {
  const auto flags = TakionCodecRig::bytes(
      {0x03, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00});
  const auto wire = TakionCodecRig::bytes({0x08, 0x21, 0x8a, 0x02, 0x0d, 0x08,
                                           0x01, 0x12, 0x09, 0x03, 0xff, 0x00,
                                           0x00, 0x00, 0x01, 0x00, 0x00, 0x00});
  const auto encoded = round_trip(
      takion::AudioState{takion::AudioStateType::flags, flags}, wire);
  EXPECT_TRUE(prefixes_never_decode_partially(encoded));

  round_trip(
      takion::AudioState{takion::AudioStateType::channel_count, ByteVector{}},
      TakionCodecRig::bytes(
          {0x08, 0x21, 0x8a, 0x02, 0x04, 0x08, 0x06, 0x12, 0x00}));
  round_trip(takion::AudioState{takion::AudioStateType::full, std::nullopt},
             TakionCodecRig::bytes({0x08, 0x21, 0x8a, 0x02, 0x02, 0x08, 0x00}));
}

TEST_F(TakionControlCodecRoundTrips, DecodeEveryAudioProfileHeaderOfAChannel) {
  ProtoWire channel;
  channel.varint(1, 0);
  channel.length(2, TakionCodecRig::bytes({}));
  for (int index = 0; index < 4; ++index)
    channel.length(4, TakionCodecRig::bytes({}));
  ProtoWire payload;
  payload.length(2, TakionCodecRig::bytes({}));
  payload.message(7, channel);
  const auto wire = ProtoWire::control(13, 15, payload);
  const auto decoded = codec.decode(wire);
  ASSERT_TRUE(decoded);
  const auto info = std::get<takion::StreamInfo>(decoded.value());
  EXPECT_EQ(info.audio_channels.size(), 1);
  EXPECT_EQ(info.audio_channels[0].profile_headers.size(), 4);
  EXPECT_FALSE(info.audio_channels[0].fec_mode);
}

TEST_F(TakionControlCodecRoundTrips, KeepASecretBigOnTheLegacyWire) {
  const auto legacy = codec.encode(takion::ControlMessage{full_big()});
  ASSERT_TRUE(legacy);
  EXPECT_TRUE(std::ranges::equal(legacy.value().bytes(), expected_full_big()));

  libchiaking::SecretString secret;
  secret.assign("session-key");
  libchiaking::SecretString secret_launch;
  secret_launch.assign("launch-spec");
  auto secure_big = full_big();
  secure_big.session_key = takion::SharedSecretText{
      std::make_shared<const libchiaking::SecretString>(std::move(secret))};
  secure_big.launch_spec = takion::SharedSecretText{
      std::make_shared<const libchiaking::SecretString>(
          std::move(secret_launch))};
  const auto encoded = codec.encode(takion::ControlMessage{secure_big.clone()});
  ASSERT_TRUE(encoded);
  EXPECT_TRUE(
      std::ranges::equal(encoded.value().bytes(), legacy.value().bytes()));
  const auto& decoded = decoded_as<takion::Big>(encoded.value());
  EXPECT_EQ(decoded.session_key.view(), "session-key");
  EXPECT_EQ(decoded.launch_spec.view(), "launch-spec");
  EXPECT_EQ(secure_big, decoded);
  EXPECT_EQ(secure_big, full_big());
  auto different = full_big();
  different.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{"other-key"};
  EXPECT_FALSE((secure_big == different));
  different = full_big();
  different.launch_spec =
      libchiaking::net::protocol::takion::SharedSecretText{"other-launch"};
  EXPECT_FALSE((secure_big == different));

  const takion::ControlCodec bounded{legacy.value().size() - 1};
  const auto rejected =
      bounded.encode(takion::ControlMessage{secure_big.clone()});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().reason,
            takion::ControlCodecErrorReason::message_too_large);

  auto oversized_big = secure_big.clone();
  oversized_big.launch_spec =
      libchiaking::net::protocol::takion::SharedSecretText{
          std::string(legacy.value().size(), 'x')};
  const takion::ControlCodec launch_bounded{legacy.value().size()};
  const auto rejected_launch =
      launch_bounded.encode(takion::ControlMessage{std::move(oversized_big)});
  ASSERT_FALSE(rejected_launch);
  EXPECT_EQ(rejected_launch.error().reason,
            takion::ControlCodecErrorReason::message_too_large);
}

TEST_F(TakionControlCodecRoundTrips, OmitAbsentOptionalQualityFields) {
  takion::ConnectionQuality empty_quality;
  const auto quality = round_trip(
      empty_quality,
      TakionCodecRig::bytes(
          {0x08, 0x10, static_cast<std::uint8_t>(0x8a), 0x01, 0x00}));
  const auto& decoded_quality = decoded_as<takion::ConnectionQuality>(quality);
  EXPECT_FALSE(decoded_quality.target_bitrate.has_value());
  EXPECT_FALSE(decoded_quality.upstream_loss.has_value());
  EXPECT_FALSE(decoded_quality.disable_upstream_audio.has_value());

  takion::ConnectionQuality explicit_defaults;
  explicit_defaults.target_bitrate = 0;
  explicit_defaults.upstream_loss = 0.0f;
  explicit_defaults.disable_upstream_audio = false;
  const auto explicit_quality = round_trip(explicit_defaults);
  EXPECT_TRUE(explicit_quality.size() > quality.size());
}

TEST_F(TakionControlCodecRoundTrips, OmitAbsentOptionalStreamInfoFields) {
  takion::StreamInfo empty_stream;
  empty_stream.audio_header = TakionCodecRig::bytes({});
  const auto stream = round_trip(empty_stream);
  const auto& decoded_stream = decoded_as<takion::StreamInfo>(stream);
  EXPECT_TRUE(decoded_stream.resolutions.empty());
  EXPECT_TRUE(decoded_stream.audio_channels.empty());
  EXPECT_TRUE(decoded_stream.audio_header.empty());
  EXPECT_FALSE(decoded_stream.start_timeout.has_value());
  EXPECT_FALSE(decoded_stream.afk_timeout.has_value());
  EXPECT_FALSE(decoded_stream.afk_timeout_disconnect.has_value());
  EXPECT_FALSE(decoded_stream.congestion_control_interval.has_value());
}

TEST_F(TakionControlCodecRoundTrips, OmitAbsentOptionalMtuFields) {
  takion::SenkushaMtuCommand command{
      1, 2, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
  const auto mtu = round_trip(takion::Senkusha{command});
  const auto decoded_mtu = std::get<takion::SenkushaMtuCommand>(
      decoded_as<takion::Senkusha>(mtu).command);
  EXPECT_FALSE(decoded_mtu.mtu_sent.has_value());
  EXPECT_FALSE(decoded_mtu.number.has_value());
  EXPECT_FALSE(decoded_mtu.send_delay.has_value());
  EXPECT_FALSE(decoded_mtu.delta.has_value());
}

TEST_F(TakionControlCodecRoundTrips, OmitAbsentOptionalBangFields) {
  takion::Bang bang;
  bang.server_version = 12;
  bang.token = 1;
  bang.encrypted_key_accepted = false;
  bang.version_accepted = false;
  const auto bang_wire = round_trip(bang);
  const auto& decoded_bang = decoded_as<takion::Bang>(bang_wire);
  EXPECT_FALSE(decoded_bang.extended_info.has_value());
  EXPECT_FALSE(decoded_bang.server_version_string.has_value());
  EXPECT_FALSE(decoded_bang.ecdh_pub_key.has_value());
  EXPECT_FALSE(decoded_bang.ecdh_sig.has_value());
  EXPECT_FALSE(decoded_bang.protocol_ack.has_value());
}

TEST_F(TakionControlCodecRoundTrips, KeepExplicitZeroAndEmptyOptionals) {
  const takion::SenkushaMtuCommand explicit_zero_mtu{1, 2, 0, 0, 0, 0};
  round_trip(takion::Senkusha{explicit_zero_mtu});

  takion::Big explicit_empty_optional_bytes;
  explicit_empty_optional_bytes.client_version = 9;
  explicit_empty_optional_bytes.ecdh_pub_key = TakionCodecRig::bytes({});
  explicit_empty_optional_bytes.ecdh_sig = TakionCodecRig::bytes({});
  round_trip(explicit_empty_optional_bytes);

  const takion::Disconnect explicit_empty_optional_string{
      "reason", takion::DisconnectEventCode{"E", "", std::nullopt}};
  round_trip(explicit_empty_optional_string);
}

}  // namespace
}  // namespace takion_test
