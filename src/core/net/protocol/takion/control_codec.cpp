#include "src/core/net/protocol/takion/control_codec.hpp"

#include "src/core/net/protocol/takion/nanopb_message.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief Builds and reads the generated nanopb Takion message for
/// ControlCodec.
class ControlCodec::Wire {
 public:
  /// @brief Gives the wire type of a message.
  ///
  /// @param message The message.
  /// @return The payload type.
  static auto wire_type(const ControlMessage& message)
      -> tkproto_TakionMessage_PayloadType;
  /// @brief Tells whether an audio state kind is in the schema.
  ///
  /// @param type The kind.
  /// @return Whether it is.
  static bool is_known_audio_state_type(AudioStateType type) noexcept;
  /// @brief Gives the reason text the wire carries for a play-time reason.
  ///
  /// @param reason The reason.
  /// @return The text; empty for PlayTimeReason::other.
  static auto play_time_reason_text(PlayTimeReason reason) noexcept
      -> std::string_view;
  /// @brief Gives the wire value of an XMB command.
  ///
  /// @param command The command.
  /// @return The wire value.
  static auto xmb_command_wire(XmbCommand::Command command) noexcept
      -> tkproto_XmbCommandPayload_XmbCommand;
  /// @brief Reads an XMB command from its wire value.
  ///
  /// @param command The wire value; unknown values read as a toggle.
  /// @return The command.
  static auto xmb_command_of(std::int32_t command) noexcept
      -> XmbCommand::Command;
  /// @brief Gives the wire value of a deep link type.
  ///
  /// @param type The type.
  /// @return The wire value.
  static auto deep_link_wire(DeepLink::Type type) noexcept
      -> tkproto_DeepLinkPayload_DeepLinkType;
  /// @brief Reads a deep link type from its wire value.
  ///
  /// @param type The wire value; unknown values read as raw.
  /// @return The type.
  static auto deep_link_of(std::int32_t type) noexcept -> DeepLink::Type;
  /// @brief Writes an event code.
  ///
  /// @param wire The protobuf event code.
  /// @param value The value.
  static void write_event_code(tkproto_EventCode& wire,
                               const DisconnectEventCode& value);
  /// @brief Writes a message's payload.
  ///
  /// @tparam Payload A control message type.
  /// @param wire The message.
  /// @param value The payload.
  template <class Payload>
  static void write_payload(tkproto_TakionMessage& wire, const Payload& value);
  /// @brief Reads the message type without decoding the rest.
  ///
  /// @param bytes The TakionMessage bytes.
  /// @return The type; nothing when the field is absent.
  /// @retval NanopbFailure::malformed The bytes are not a protobuf message, or
  ///     the type does not fit 32-bit signed.
  [[nodiscard]] static auto read_type(std::span<const std::byte> bytes)
      -> Expected<std::optional<std::uint32_t>, NanopbFailure>;
  /// @brief Tells whether every required enum the decoder reads holds a
  /// value the schema knows. Protobuf treats an unknown value in a required
  /// enum as an absent field; the Senkusha command is exempt, since an unknown
  /// command is kept as UnknownSenkushaCommand.
  ///
  /// @param message The decoded message.
  /// @return Whether they all do.
  static bool required_enums_known(
      const tkproto_TakionMessage& message) noexcept;
  /// @brief Reads an event code.
  ///
  /// @param wire The event code.
  /// @return The value.
  static auto read_event_code(const tkproto_EventCode& wire)
      -> DisconnectEventCode;
};

auto ControlCodec::Wire::play_time_reason_text(PlayTimeReason reason) noexcept
    -> std::string_view {
  switch (reason) {
    case PlayTimeReason::session_limit:
      return "MaxLengthPlay";
    case PlayTimeReason::maintenance:
      return "Maintenance";
    case PlayTimeReason::other:
      return {};
  }
  return {};
}

auto ControlCodec::Wire::xmb_command_wire(XmbCommand::Command command) noexcept
    -> tkproto_XmbCommandPayload_XmbCommand {
  switch (command) {
    case XmbCommand::Command::off:
      return tkproto_XmbCommandPayload_XmbCommand_OFF;
    case XmbCommand::Command::on:
      return tkproto_XmbCommandPayload_XmbCommand_ON;
    case XmbCommand::Command::toggle:
      return tkproto_XmbCommandPayload_XmbCommand_TOGGLE;
  }
  return tkproto_XmbCommandPayload_XmbCommand_TOGGLE;
}

auto ControlCodec::Wire::xmb_command_of(std::int32_t command) noexcept
    -> XmbCommand::Command {
  switch (command) {
    case tkproto_XmbCommandPayload_XmbCommand_OFF:
      return XmbCommand::Command::off;
    case tkproto_XmbCommandPayload_XmbCommand_ON:
      return XmbCommand::Command::on;
    case tkproto_XmbCommandPayload_XmbCommand_TOGGLE:
      return XmbCommand::Command::toggle;
  }
  return XmbCommand::Command::toggle;
}

auto ControlCodec::Wire::deep_link_wire(DeepLink::Type type) noexcept
    -> tkproto_DeepLinkPayload_DeepLinkType {
  switch (type) {
    case DeepLink::Type::save_data:
      return tkproto_DeepLinkPayload_DeepLinkType_SAVE_DATA;
    case DeepLink::Type::invite:
      return tkproto_DeepLinkPayload_DeepLinkType_INVITE;
    case DeepLink::Type::game_alert:
      return tkproto_DeepLinkPayload_DeepLinkType_GAME_ALERT;
    case DeepLink::Type::system_service_status:
      return tkproto_DeepLinkPayload_DeepLinkType_SYSTEM_SERVICE_STATUS;
    case DeepLink::Type::debug_vsh_menu:
      return tkproto_DeepLinkPayload_DeepLinkType_DEBUG_VSH_MENU;
    case DeepLink::Type::raw:
      return tkproto_DeepLinkPayload_DeepLinkType_RAW;
  }
  return tkproto_DeepLinkPayload_DeepLinkType_RAW;
}

auto ControlCodec::Wire::deep_link_of(std::int32_t type) noexcept
    -> DeepLink::Type {
  switch (type) {
    case tkproto_DeepLinkPayload_DeepLinkType_SAVE_DATA:
      return DeepLink::Type::save_data;
    case tkproto_DeepLinkPayload_DeepLinkType_INVITE:
      return DeepLink::Type::invite;
    case tkproto_DeepLinkPayload_DeepLinkType_GAME_ALERT:
      return DeepLink::Type::game_alert;
    case tkproto_DeepLinkPayload_DeepLinkType_SYSTEM_SERVICE_STATUS:
      return DeepLink::Type::system_service_status;
    case tkproto_DeepLinkPayload_DeepLinkType_DEBUG_VSH_MENU:
      return DeepLink::Type::debug_vsh_menu;
    case tkproto_DeepLinkPayload_DeepLinkType_RAW:
      return DeepLink::Type::raw;
  }
  return DeepLink::Type::raw;
}

auto ControlCodec::Wire::wire_type(const ControlMessage& message)
    -> tkproto_TakionMessage_PayloadType {
  return std::visit(
      [](const auto& value) -> tkproto_TakionMessage_PayloadType {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Heartbeat>) {
          return tkproto_TakionMessage_PayloadType_HEARTBEAT;
        } else if constexpr (std::is_same_v<T, Big>) {
          return tkproto_TakionMessage_PayloadType_BIG;
        } else if constexpr (std::is_same_v<T, Bang>) {
          return tkproto_TakionMessage_PayloadType_BANG;
        } else if constexpr (std::is_same_v<T, StreamInfo>) {
          return tkproto_TakionMessage_PayloadType_STREAMINFO;
        } else if constexpr (std::is_same_v<T, StreamInfoAcknowledgement>) {
          return tkproto_TakionMessage_PayloadType_STREAMINFOACK;
        } else if constexpr (std::is_same_v<T, Senkusha>) {
          return tkproto_TakionMessage_PayloadType_SENKUSHA;
        } else if constexpr (std::is_same_v<T, ConnectionQuality>) {
          return tkproto_TakionMessage_PayloadType_CONNECTIONQUALITY;
        } else if constexpr (std::is_same_v<T, CorruptFrame>) {
          return tkproto_TakionMessage_PayloadType_CORRUPTFRAME;
        } else if constexpr (std::is_same_v<T, ControllerConnection>) {
          return tkproto_TakionMessage_PayloadType_CONTROLLERCONNECTION;
        } else if constexpr (std::is_same_v<T, IdrRequest>) {
          return tkproto_TakionMessage_PayloadType_IDRREQUEST;
        } else if constexpr (std::is_same_v<T, Disconnect>) {
          return tkproto_TakionMessage_PayloadType_DISCONNECT;
        } else if constexpr (std::is_same_v<T, ProtocolRequest>) {
          return tkproto_TakionMessage_PayloadType_TAKIONPROTOCOLREQUEST;
        } else if constexpr (std::is_same_v<T, ProtocolRequestAck>) {
          return tkproto_TakionMessage_PayloadType_TAKIONPROTOCOLREQUESTACK;
        } else if constexpr (std::is_same_v<T, AudioState>) {
          return tkproto_TakionMessage_PayloadType_AUDIOSTATE;
        } else if constexpr (std::is_same_v<T, PlayTimeLeft>) {
          return tkproto_TakionMessage_PayloadType_PLAYTIMELEFT;
        } else if constexpr (std::is_same_v<T, ServerMessage>) {
          return tkproto_TakionMessage_PayloadType_SERVERMESSAGE;
        } else if constexpr (std::is_same_v<T, XmbCommand>) {
          return tkproto_TakionMessage_PayloadType_XMBCOMMAND;
        } else if constexpr (std::is_same_v<T, DeepLink> ||
                             std::is_same_v<T, MicInfo>) {
          return tkproto_TakionMessage_PayloadType_DIRECTMESSAGE;
        } else if constexpr (std::is_same_v<T, MicConnection>) {
          return tkproto_TakionMessage_PayloadType_MICCONNECTION;
        } else if constexpr (std::is_same_v<T, MetricsOverlay>) {
          return tkproto_TakionMessage_PayloadType_METRICSOVERLAY;
        } else {
          return static_cast<tkproto_TakionMessage_PayloadType>(value.type);
        }
      },
      message);
}

bool ControlCodec::Wire::is_known_audio_state_type(
    AudioStateType type) noexcept {
  return tkproto_AudioStatePayload_AudioStateType_valid(
      static_cast<tkproto_AudioStatePayload_AudioStateType>(type));
}

void ControlCodec::Wire::write_event_code(tkproto_EventCode& wire,
                                          const DisconnectEventCode& value) {
  TakionWire::assign(wire.event_code, value.event_code);
  if (value.name) TakionWire::assign(wire.name, *value.name);
  if (value.description)
    TakionWire::assign(wire.description, *value.description);
}

template <class Payload>
void ControlCodec::Wire::write_payload(tkproto_TakionMessage& wire,
                                       const Payload& value) {
  if constexpr (std::is_same_v<Payload, Disconnect>) {
    wire.has_disconnect_payload = true;
    auto& payload = wire.disconnect_payload;
    TakionWire::assign(payload.reason, value.reason);
    if (value.extended_info) {
      payload.has_extended_info = true;
      write_event_code(payload.extended_info, *value.extended_info);
    }
  } else if constexpr (std::is_same_v<Payload, Big>) {
    wire.has_big_payload = true;
    auto& payload = wire.big_payload;
    payload.client_version = value.client_version;
    TakionWire::assign(payload.session_key, value.session_key.view());
    TakionWire::assign(payload.launch_spec, value.launch_spec.view());
    TakionWire::assign(payload.encrypted_key,
                       std::span<const std::byte>{value.encrypted_key});
    if (value.ecdh_pub_key)
      TakionWire::assign(payload.ecdh_pub_key,
                         std::span<const std::byte>{*value.ecdh_pub_key});
    if (value.ecdh_sig)
      TakionWire::assign(payload.ecdh_sig,
                         std::span<const std::byte>{*value.ecdh_sig});
  } else if constexpr (std::is_same_v<Payload, Bang>) {
    wire.has_bang_payload = true;
    auto& payload = wire.bang_payload;
    payload.server_version = value.server_version;
    payload.token = value.token;
    payload.encrypted_key_accepted = value.encrypted_key_accepted;
    payload.version_accepted = value.version_accepted;
    TakionWire::assign(payload.session_key, value.session_key.view());
    if (value.extended_info) {
      payload.has_extended_info = true;
      write_event_code(payload.extended_info, *value.extended_info);
    }
    if (value.server_version_string)
      TakionWire::assign(payload.server_version_string,
                         *value.server_version_string);
    if (value.ecdh_pub_key)
      TakionWire::assign(payload.ecdh_pub_key,
                         std::span<const std::byte>{*value.ecdh_pub_key});
    if (value.ecdh_sig)
      TakionWire::assign(payload.ecdh_sig,
                         std::span<const std::byte>{*value.ecdh_sig});
    if (value.protocol_ack) write_payload(wire, *value.protocol_ack);
  } else if constexpr (std::is_same_v<Payload, StreamInfo>) {
    wire.has_stream_info_payload = true;
    auto& payload = wire.stream_info_payload;
    const auto resolutions = TakionWire::resize(
        payload.resolution, payload.resolution_count, value.resolutions.size());
    for (std::size_t index = 0; index < resolutions.size(); ++index) {
      const auto& resolution = value.resolutions[index];
      auto& wire_resolution = resolutions[index];
      wire_resolution.width = resolution.width;
      wire_resolution.height = resolution.height;
      TakionWire::assign(wire_resolution.video_header,
                         std::span<const std::byte>{resolution.video_header});
    }
    TakionWire::assign(payload.audio_header,
                       std::span<const std::byte>{value.audio_header});
    if (value.start_timeout) {
      payload.has_start_timeout = true;
      payload.start_timeout = *value.start_timeout;
    }
    if (value.afk_timeout) {
      payload.has_afk_timeout = true;
      payload.afk_timeout = *value.afk_timeout;
    }
    if (value.afk_timeout_disconnect) {
      payload.has_afk_timeout_disconnect = true;
      payload.afk_timeout_disconnect = *value.afk_timeout_disconnect;
    }
    if (value.congestion_control_interval) {
      payload.has_congestion_control_interval = true;
      payload.congestion_control_interval = *value.congestion_control_interval;
    }
    const auto channels =
        TakionWire::resize(payload.audio_channel, payload.audio_channel_count,
                           value.audio_channels.size());
    for (std::size_t index = 0; index < channels.size(); ++index) {
      const auto& channel = value.audio_channels[index];
      auto& wire_channel = channels[index];
      wire_channel.audio_channel_type = channel.audio_channel_type;
      TakionWire::assign(wire_channel.audio_header,
                         std::span<const std::byte>{channel.audio_header});
      if (channel.is_raw_pcm) {
        wire_channel.has_is_raw_pcm = true;
        wire_channel.is_raw_pcm = *channel.is_raw_pcm;
      }
      const auto headers = TakionWire::resize(wire_channel.profile_header,
                                              wire_channel.profile_header_count,
                                              channel.profile_headers.size());
      for (std::size_t header = 0; header < headers.size(); ++header)
        TakionWire::assign(
            headers[header],
            std::span<const std::byte>{channel.profile_headers[header]});
      if (channel.fec_mode) {
        wire_channel.has_fec_mode = true;
        wire_channel.fec_mode = *channel.fec_mode;
      }
    }
    if (value.tos_value) {
      payload.has_tos_value = true;
      payload.tos_value = *value.tos_value;
    }
    if (value.tos_report_packet_threshold) {
      payload.has_tos_report_num_pkts_recv = true;
      payload.tos_report_num_pkts_recv = *value.tos_report_packet_threshold;
    }
    if (value.tos_report_interval_ms) {
      payload.has_tos_report_interval_msec = true;
      payload.tos_report_interval_msec = *value.tos_report_interval_ms;
    }
  } else if constexpr (std::is_same_v<Payload, Senkusha>) {
    wire.has_senkusha_payload = true;
    std::visit(
        [&payload = wire.senkusha_payload](const auto& command) {
          using Command = std::decay_t<decltype(command)>;
          if constexpr (std::is_same_v<Command, SenkushaEchoCommand>) {
            payload.command = TakionWire::number(
                tkproto_SenkushaPayload_Command_ECHO_COMMAND);
            payload.has_echo_command = true;
            payload.echo_command.state = command.state;
          } else if constexpr (std::is_same_v<Command, SenkushaMtuCommand>) {
            payload.command =
                TakionWire::number(tkproto_SenkushaPayload_Command_MTU_COMMAND);
            payload.has_mtu_command = true;
            auto& nested = payload.mtu_command;
            nested.id = command.id;
            nested.mtu_req = command.mtu_request;
            if (command.mtu_sent) {
              nested.has_mtu_sent = true;
              nested.mtu_sent = *command.mtu_sent;
            }
            if (command.number) {
              nested.has_num = true;
              nested.num = *command.number;
            }
            if (command.send_delay) {
              nested.has_send_delay = true;
              nested.send_delay = *command.send_delay;
            }
            if (command.delta) {
              nested.has_delta = true;
              nested.delta = *command.delta;
            }
          } else if constexpr (std::is_same_v<Command,
                                              SenkushaBandwidthCommand>) {
            payload.command = TakionWire::number(
                tkproto_SenkushaPayload_Command_BANDWIDTH_COMMAND);
            payload.has_bandwidth_command = true;
            auto& nested = payload.bandwidth_command;
            nested.id = command.id;
            nested.time = command.time;
            if (command.bandwidth) {
              nested.has_bandwidth = true;
              nested.bandwidth = *command.bandwidth;
            }
            if (command.fps) {
              nested.has_fps = true;
              nested.fps = *command.fps;
            }
            if (command.mtu) {
              nested.has_mtu = true;
              nested.mtu = *command.mtu;
            }
            if (command.nalu_count) {
              nested.has_nalu_count = true;
              nested.nalu_count = *command.nalu_count;
            }
            if (command.total_sent) {
              nested.has_total_sent = true;
              nested.total_sent = *command.total_sent;
            }
            if (command.upstream) {
              nested.has_upstream = true;
              nested.upstream = *command.upstream;
            }
            if (command.loss) {
              nested.has_loss = true;
              nested.loss = *command.loss;
            }
          } else if constexpr (std::is_same_v<Command,
                                              SenkushaClientMtuCommand>) {
            payload.command = TakionWire::number(
                tkproto_SenkushaPayload_Command_CLIENT_MTU_COMMAND);
            payload.has_client_mtu_command = true;
            auto& nested = payload.client_mtu_command;
            nested.id = command.id;
            nested.mtu_req = command.mtu_request;
            nested.state = command.state;
            if (command.mtu_down) {
              nested.has_mtu_down = true;
              nested.mtu_down = *command.mtu_down;
            }
          } else {
            payload.command = command.command;
          }
        },
        value.command);
  } else if constexpr (std::is_same_v<Payload, ConnectionQuality>) {
    wire.has_connection_quality_payload = true;
    auto& payload = wire.connection_quality_payload;
    if (value.target_bitrate) {
      payload.has_target_bitrate = true;
      payload.target_bitrate = *value.target_bitrate;
    }
    if (value.upstream_bitrate) {
      payload.has_upstream_bitrate = true;
      payload.upstream_bitrate = *value.upstream_bitrate;
    }
    if (value.upstream_loss) {
      payload.has_upstream_loss = true;
      payload.upstream_loss = *value.upstream_loss;
    }
    if (value.disable_upstream_audio) {
      payload.has_disable_upstream_audio = true;
      payload.disable_upstream_audio = *value.disable_upstream_audio;
    }
    if (value.rtt) {
      payload.has_rtt = true;
      payload.rtt = *value.rtt;
    }
    if (value.loss) {
      payload.has_loss = true;
      payload.loss = *value.loss;
    }
  } else if constexpr (std::is_same_v<Payload, PlayTimeLeft>) {
    wire.has_play_time_left_payload = true;
    auto& payload = wire.play_time_left_payload;
    payload.has_time_left = true;
    payload.time_left = value.minutes;
    if (const auto reason = play_time_reason_text(value.reason);
        !reason.empty())
      TakionWire::assign(payload.reason, reason);
  } else if constexpr (std::is_same_v<Payload, ServerMessage>) {
    wire.has_server_message_payload = true;
    TakionWire::assign(wire.server_message_payload.payload, value.text);
  } else if constexpr (std::is_same_v<Payload, MetricsOverlay>) {
    wire.has_metrics_overlay_payload = true;
    wire.metrics_overlay_payload.has_limited_metrics_overlay_payload = true;
    auto& limited =
        wire.metrics_overlay_payload.limited_metrics_overlay_payload;
    if (value.available_down_kbps) {
      limited.has_available_downstream_bandwidth_kbps = true;
      limited.available_downstream_bandwidth_kbps = *value.available_down_kbps;
    }
    if (value.used_down_kbps) {
      limited.has_used_downstream_bandwidth_kbps = true;
      limited.used_downstream_bandwidth_kbps = *value.used_down_kbps;
    }
    if (value.used_up_kbps) {
      limited.has_used_upstream_kbps = true;
      limited.used_upstream_kbps = *value.used_up_kbps;
    }
    if (value.round_trip_ms) {
      limited.has_latest_round_trip_time_ms = true;
      limited.latest_round_trip_time_ms = *value.round_trip_ms;
    }
    if (value.loss_percent) {
      limited.has_loss_pct = true;
      limited.loss_pct = *value.loss_percent;
    }
    if (value.upstream_loss_percent) {
      limited.has_upstream_loss_pct = true;
      limited.upstream_loss_pct = *value.upstream_loss_percent;
    }
    if (value.gateway_round_trip_us) {
      limited.has_gateway_rtt_us = true;
      limited.gateway_rtt_us = *value.gateway_round_trip_us;
    }
    if (value.audio_channels) {
      limited.has_audio_channels = true;
      limited.audio_channels = *value.audio_channels;
    }
    if (!value.dynamic_range.empty())
      TakionWire::assign(limited.dynamic_range, value.dynamic_range);
    if (!value.video_codec.empty())
      TakionWire::assign(limited.video_codec, value.video_codec);
    if (value.width) {
      limited.has_width = true;
      limited.width = *value.width;
    }
    if (value.height) {
      limited.has_height = true;
      limited.height = *value.height;
    }
    if (value.corrupt_frames) {
      limited.has_corrupt_frame_count = true;
      limited.corrupt_frame_count = *value.corrupt_frames;
    }
  } else if constexpr (std::is_same_v<Payload, DeepLink>) {
    DeepLinkWire link;
    link->deep_link_type = TakionWire::number(deep_link_wire(value.type));
    if (value.request_id) {
      link->has_request_id = true;
      link->request_id = *value.request_id;
    }
    if (!value.raw_data.empty())
      DeepLinkWire::assign(link->raw_data, value.raw_data);
    if (!value.invitation_id.empty())
      DeepLinkWire::assign(link->invitation_id, value.invitation_id);
    if (!value.session_id.empty())
      DeepLinkWire::assign(link->session_id, value.session_id);
    if (!value.item_id.empty())
      DeepLinkWire::assign(link->item_id, value.item_id);
    if (value.pad_unique_id) {
      link->has_pad_unique_id = true;
      link->pad_unique_id = *value.pad_unique_id;
    }
    if (value.result) {
      link->has_result = true;
      link->result = *value.result;
    }
    if (value.should_show) {
      link->has_should_show = true;
      link->should_show = *value.should_show;
    }
    wire.has_direct_message_payload = true;
    auto& payload = wire.direct_message_payload;
    payload.direct_message_type = TakionWire::number(
        tkproto_DirectMessagePayload_DirectMessageType_DEEPLINK);
    payload.destination = TakionWire::number(
        tkproto_DirectMessagePayload_Destination_GAIKAI_CONTROLLER);
    const auto encoded = link.encode<std::vector<std::byte>>(
        std::numeric_limits<std::size_t>::max());
    TakionWire::assign(payload.data,
                       encoded ? std::span<const std::byte>{encoded.value()}
                               : std::span<const std::byte>{});
  } else if constexpr (std::is_same_v<Payload, MicConnection>) {
    wire.has_mic_connection_payload = true;
    auto& payload = wire.mic_connection_payload;
    payload.controller_id = value.controller_id;
    payload.connected = value.connected;
    if (value.result) {
      payload.has_result = true;
      payload.result = *value.result;
    }
  } else if constexpr (std::is_same_v<Payload, MicInfo>) {
    MicInfoWire info;
    info->mic_info_type =
        TakionWire::number(value.type == MicInfo::Type::connect
                               ? tkproto_MicInfoPayload_MicInfoType_MIC_CONNECT
                               : tkproto_MicInfoPayload_MicInfoType_MIC_MUTE);
    if (value.controller_id) {
      info->has_controller_id = true;
      info->controller_id = *value.controller_id;
    }
    if (value.connected) {
      info->has_connected = true;
      info->connected = *value.connected;
    }
    if (value.muted) {
      info->has_muted = true;
      info->muted = *value.muted;
    }
    if (value.result) {
      info->has_result = true;
      info->result = *value.result;
    }
    wire.has_direct_message_payload = true;
    auto& payload = wire.direct_message_payload;
    payload.direct_message_type = TakionWire::number(
        tkproto_DirectMessagePayload_DirectMessageType_MICINFO);
    payload.destination = TakionWire::number(
        tkproto_DirectMessagePayload_Destination_GAIKAI_CONTROLLER);
    const auto encoded = info.encode<std::vector<std::byte>>(
        std::numeric_limits<std::size_t>::max());
    TakionWire::assign(payload.data,
                       encoded ? std::span<const std::byte>{encoded.value()}
                               : std::span<const std::byte>{});
  } else if constexpr (std::is_same_v<Payload, XmbCommand>) {
    wire.has_xmb_command_payload = true;
    auto& payload = wire.xmb_command_payload;
    payload.command = TakionWire::number(xmb_command_wire(value.command));
    if (value.player_id) {
      payload.has_player_id = true;
      payload.player_id = *value.player_id;
    }
  } else if constexpr (std::is_same_v<Payload, CorruptFrame>) {
    wire.has_corrupt_payload = true;
    wire.corrupt_payload.start = value.start;
    wire.corrupt_payload.end = value.end;
  } else if constexpr (std::is_same_v<Payload, ControllerConnection>) {
    wire.has_controller_connection_payload = true;
    auto& payload = wire.controller_connection_payload;
    if (value.controller_id) {
      payload.has_controller_id = true;
      payload.controller_id = *value.controller_id;
    }
    if (value.connected) {
      payload.has_connected = true;
      payload.connected = *value.connected;
    }
    if (value.controller_type) {
      payload.has_controller_type = true;
      payload.controller_type = std::visit(
          [](const auto& kind) -> std::int32_t {
            if constexpr (std::is_same_v<std::decay_t<decltype(kind)>,
                                         ControllerConnectionType>)
              return std::to_underlying(kind);
            else
              return kind.value;
          },
          *value.controller_type);
    }
  } else if constexpr (std::is_same_v<Payload, ProtocolRequest>) {
    wire.has_takion_protocol_request = true;
    auto& payload = wire.takion_protocol_request;
    std::ranges::copy(
        value.supported_versions,
        TakionWire::resize(payload.supported_takion_versions,
                           payload.supported_takion_versions_count,
                           value.supported_versions.size())
            .begin());
  } else if constexpr (std::is_same_v<Payload, ProtocolRequestAck>) {
    wire.has_takion_protocol_request_ack = true;
    if (value.selected_version) {
      wire.takion_protocol_request_ack.has_takion_protocol_version = true;
      wire.takion_protocol_request_ack.takion_protocol_version =
          *value.selected_version;
    }
  } else if constexpr (std::is_same_v<Payload, AudioState>) {
    wire.has_audio_state = true;
    auto& payload = wire.audio_state;
    payload.audio_state_type = static_cast<std::int32_t>(value.type);
    if (value.data)
      TakionWire::assign(payload.audio_state_data,
                         std::span<const std::byte>{*value.data});
  }
}

auto ControlCodec::encode(const ControlMessage& message) const
    -> Expected<SecretBytes, ControlCodecError> {
  if (std::holds_alternative<UnsupportedControlMessage>(message))
    return unexpected(
        ControlCodecError{ControlCodecErrorReason::unsupported_for_encoding});
  if (const auto* state = std::get_if<AudioState>(&message);
      state && !Wire::is_known_audio_state_type(state->type))
    return unexpected(
        ControlCodecError{ControlCodecErrorReason::unsupported_for_encoding});

  TakionWire wire;
  wire->type = TakionWire::number(Wire::wire_type(message));
  std::visit([&wire](const auto& value) { Wire::write_payload(*wire, value); },
             message);

  const auto size = wire.encoded_size();
  if (!size) {
    if (size.error() == NanopbFailure::missing_required_field)
      return unexpected(
          ControlCodecError{ControlCodecErrorReason::missing_required_field});
    return unexpected(
        ControlCodecError{ControlCodecErrorReason::serialization_failed});
  }
  const auto limit = effective_message_size_limit();
  if (*size > limit)
    return unexpected(ControlCodecError{
        ControlCodecErrorReason::message_too_large, *size, limit});
  auto bytes = wire.encode<SecretBytes>(limit);
  if (!bytes)
    return unexpected(ControlCodecError{
        ControlCodecErrorReason::serialization_failed, *size});
  return std::move(bytes).value();
}

auto ControlCodec::Wire::read_type(std::span<const std::byte> bytes)
    -> Expected<std::optional<std::uint32_t>, NanopbFailure> {
  auto stream = pb_istream_from_buffer(
      reinterpret_cast<const pb_byte_t*>(bytes.data()), bytes.size());
  std::optional<std::uint32_t> type;
  pb_wire_type_t wire_type{};
  std::uint32_t tag{};
  bool end{};
  while (pb_decode_tag(&stream, &wire_type, &tag, &end)) {
    if (tag == tkproto_TakionMessage_type_tag && wire_type == PB_WT_VARINT) {
      std::uint64_t value{};
      if (!pb_decode_varint(&stream, &value))
        return unexpected(NanopbFailure::malformed);
      const bool fits_int32 =
          value <= std::numeric_limits<std::uint32_t>::max() ||
          value >= static_cast<std::uint64_t>(static_cast<std::int64_t>(
                       std::numeric_limits<std::int32_t>::min()));
      if (!fits_int32) return unexpected(NanopbFailure::malformed);
      type = static_cast<std::uint32_t>(value);
    } else if (!pb_skip_field(&stream, wire_type)) {
      return unexpected(NanopbFailure::malformed);
    }
  }
  if (!end || stream.bytes_left != 0)
    return unexpected(NanopbFailure::malformed);
  return type;
}

bool ControlCodec::Wire::required_enums_known(
    const tkproto_TakionMessage& message) noexcept {
  if (message.has_audio_state &&
      !tkproto_AudioStatePayload_AudioStateType_valid(
          static_cast<tkproto_AudioStatePayload_AudioStateType>(
              message.audio_state.audio_state_type)))
    return false;
  if (message.has_xmb_command_payload &&
      !tkproto_XmbCommandPayload_XmbCommand_valid(
          static_cast<tkproto_XmbCommandPayload_XmbCommand>(
              message.xmb_command_payload.command)))
    return false;
  if (message.has_direct_message_payload &&
      (!tkproto_DirectMessagePayload_DirectMessageType_valid(
           static_cast<tkproto_DirectMessagePayload_DirectMessageType>(
               message.direct_message_payload.direct_message_type)) ||
       !tkproto_DirectMessagePayload_Destination_valid(
           static_cast<tkproto_DirectMessagePayload_Destination>(
               message.direct_message_payload.destination))))
    return false;
  return true;
}

auto ControlCodec::Wire::read_event_code(const tkproto_EventCode& wire)
    -> DisconnectEventCode {
  DisconnectEventCode event;
  event.event_code = std::string{TakionWire::text_of(wire.event_code)};
  if (wire.name != nullptr)
    event.name = std::string{TakionWire::text_of(wire.name)};
  if (wire.description != nullptr)
    event.description = std::string{TakionWire::text_of(wire.description)};
  return event;
}

auto ControlCodec::decode(std::span<const std::byte> bytes) const
    -> Expected<ControlMessage, ControlCodecError> {
  const auto limit = effective_message_size_limit();
  if (bytes.size() > limit)
    return unexpected(ControlCodecError{
        ControlCodecErrorReason::message_too_large, bytes.size(), limit});
  const auto failure = [&bytes](ControlCodecErrorReason reason) {
    return unexpected(ControlCodecError{reason, bytes.size()});
  };

  const auto type = Wire::read_type(bytes);
  if (!type) return failure(ControlCodecErrorReason::malformed_message);
  if (!*type) return failure(ControlCodecErrorReason::missing_required_field);
  if (**type > static_cast<std::uint32_t>(
                   tkproto_TakionMessage_PayloadType_METRICSOVERLAY))
    return ControlMessage{
        UnsupportedControlMessage{static_cast<ControlPayloadType>(**type)}};

  TakionWire wire_message;
  if (const auto decoded = wire_message.decode(bytes); !decoded)
    return failure(decoded.error() == NanopbFailure::missing_required_field
                       ? ControlCodecErrorReason::missing_required_field
                       : ControlCodecErrorReason::malformed_message);
  const auto& message = *wire_message;
  if (!Wire::required_enums_known(message))
    return failure(ControlCodecErrorReason::missing_required_field);
  switch (message.type) {
    case tkproto_TakionMessage_PayloadType_HEARTBEAT:
      return ControlMessage{Heartbeat{}};
    case tkproto_TakionMessage_PayloadType_BIG: {
      if (!message.has_big_payload)
        return failure(ControlCodecErrorReason::missing_payload);
      const auto& payload = message.big_payload;
      Big result;
      result.client_version = payload.client_version;
      result.session_key =
          SharedSecretText{TakionWire::text_of(payload.session_key)};
      result.launch_spec =
          SharedSecretText{TakionWire::text_of(payload.launch_spec)};
      result.encrypted_key = std::vector<std::byte>{
          std::from_range, TakionWire::bytes_of(payload.encrypted_key)};
      if (payload.ecdh_pub_key != nullptr)
        result.ecdh_pub_key = std::vector<std::byte>{
            std::from_range, TakionWire::bytes_of(payload.ecdh_pub_key)};
      if (payload.ecdh_sig != nullptr)
        result.ecdh_sig = std::vector<std::byte>{
            std::from_range, TakionWire::bytes_of(payload.ecdh_sig)};
      return ControlMessage{std::move(result)};
    }
    case tkproto_TakionMessage_PayloadType_BANG: {
      if (!message.has_bang_payload)
        return failure(ControlCodecErrorReason::missing_payload);
      const auto& payload = message.bang_payload;
      Bang result;
      result.server_version = payload.server_version;
      result.token = payload.token;
      result.encrypted_key_accepted = payload.encrypted_key_accepted;
      result.version_accepted = payload.version_accepted;
      result.session_key =
          SharedSecretText{TakionWire::text_of(payload.session_key)};
      if (payload.has_extended_info)
        result.extended_info = Wire::read_event_code(payload.extended_info);
      if (payload.server_version_string != nullptr)
        result.server_version_string =
            std::string{TakionWire::text_of(payload.server_version_string)};
      if (payload.ecdh_pub_key != nullptr)
        result.ecdh_pub_key = std::vector<std::byte>{
            std::from_range, TakionWire::bytes_of(payload.ecdh_pub_key)};
      if (payload.ecdh_sig != nullptr)
        result.ecdh_sig = std::vector<std::byte>{
            std::from_range, TakionWire::bytes_of(payload.ecdh_sig)};
      if (message.has_takion_protocol_request_ack) {
        ProtocolRequestAck acknowledgement;
        if (message.takion_protocol_request_ack.has_takion_protocol_version)
          acknowledgement.selected_version =
              message.takion_protocol_request_ack.takion_protocol_version;
        result.protocol_ack = acknowledgement;
      }
      return ControlMessage{std::move(result)};
    }
    case tkproto_TakionMessage_PayloadType_DISCONNECT: {
      if (!message.has_disconnect_payload)
        return failure(ControlCodecErrorReason::missing_payload);
      const auto& payload = message.disconnect_payload;
      Disconnect result;
      result.reason = std::string{TakionWire::text_of(payload.reason)};
      if (payload.has_extended_info)
        result.extended_info = Wire::read_event_code(payload.extended_info);
      return ControlMessage{std::move(result)};
    }
    case tkproto_TakionMessage_PayloadType_STREAMINFO: {
      if (!message.has_stream_info_payload)
        return failure(ControlCodecErrorReason::missing_payload);
      const auto& payload = message.stream_info_payload;
      StreamInfo result;
      result.resolutions.reserve(payload.resolution_count);
      result.audio_channels.reserve(payload.audio_channel_count);
      for (const auto& resolution :
           std::span{payload.resolution, payload.resolution_count})
        result.resolutions.push_back(StreamInfoResolution{
            resolution.width, resolution.height,
            std::vector<std::byte>{
                std::from_range,
                TakionWire::bytes_of(resolution.video_header)}});
      result.audio_header = std::vector<std::byte>{
          std::from_range, TakionWire::bytes_of(payload.audio_header)};
      if (payload.has_start_timeout)
        result.start_timeout = payload.start_timeout;
      if (payload.has_afk_timeout) result.afk_timeout = payload.afk_timeout;
      if (payload.has_afk_timeout_disconnect)
        result.afk_timeout_disconnect = payload.afk_timeout_disconnect;
      if (payload.has_congestion_control_interval)
        result.congestion_control_interval =
            payload.congestion_control_interval;
      for (const auto& channel :
           std::span{payload.audio_channel, payload.audio_channel_count}) {
        StreamInfoAudioChannel typed;
        typed.audio_channel_type = channel.audio_channel_type;
        typed.audio_header = std::vector<std::byte>{
            std::from_range, TakionWire::bytes_of(channel.audio_header)};
        if (channel.has_is_raw_pcm) typed.is_raw_pcm = channel.is_raw_pcm;
        typed.profile_headers.reserve(channel.profile_header_count);
        for (const auto* header :
             std::span{channel.profile_header, channel.profile_header_count})
          typed.profile_headers.push_back(std::vector<std::byte>{
              std::from_range, TakionWire::bytes_of(header)});
        if (channel.has_fec_mode) typed.fec_mode = channel.fec_mode;
        result.audio_channels.push_back(std::move(typed));
      }
      if (payload.has_tos_value) result.tos_value = payload.tos_value;
      if (payload.has_tos_report_num_pkts_recv)
        result.tos_report_packet_threshold = payload.tos_report_num_pkts_recv;
      if (payload.has_tos_report_interval_msec)
        result.tos_report_interval_ms = payload.tos_report_interval_msec;
      return ControlMessage{std::move(result)};
    }
    case tkproto_TakionMessage_PayloadType_STREAMINFOACK:
      return ControlMessage{StreamInfoAcknowledgement{}};
    case tkproto_TakionMessage_PayloadType_AUDIOSTATE: {
      if (!message.has_audio_state)
        return failure(ControlCodecErrorReason::missing_payload);
      const auto& payload = message.audio_state;
      AudioState result;
      result.type = static_cast<AudioStateType>(payload.audio_state_type);
      if (payload.audio_state_data != nullptr)
        result.data = std::vector<std::byte>{
            std::from_range, TakionWire::bytes_of(payload.audio_state_data)};
      return ControlMessage{std::move(result)};
    }
    case tkproto_TakionMessage_PayloadType_SENKUSHA: {
      if (!message.has_senkusha_payload)
        return failure(ControlCodecErrorReason::missing_payload);
      const auto& payload = message.senkusha_payload;
      switch (payload.command) {
        case tkproto_SenkushaPayload_Command_ECHO_COMMAND:
          if (!payload.has_echo_command)
            return failure(ControlCodecErrorReason::missing_payload);
          return ControlMessage{
              Senkusha{SenkushaEchoCommand{payload.echo_command.state}}};
        case tkproto_SenkushaPayload_Command_MTU_COMMAND: {
          if (!payload.has_mtu_command)
            return failure(ControlCodecErrorReason::missing_payload);
          const auto& nested = payload.mtu_command;
          SenkushaMtuCommand result;
          result.id = nested.id;
          result.mtu_request = nested.mtu_req;
          if (nested.has_mtu_sent) result.mtu_sent = nested.mtu_sent;
          if (nested.has_num) result.number = nested.num;
          if (nested.has_send_delay) result.send_delay = nested.send_delay;
          if (nested.has_delta) result.delta = nested.delta;
          return ControlMessage{Senkusha{result}};
        }
        case tkproto_SenkushaPayload_Command_BANDWIDTH_COMMAND: {
          if (!payload.has_bandwidth_command)
            return failure(ControlCodecErrorReason::missing_payload);
          const auto& nested = payload.bandwidth_command;
          SenkushaBandwidthCommand result;
          result.id = nested.id;
          result.time = nested.time;
          if (nested.has_bandwidth) result.bandwidth = nested.bandwidth;
          if (nested.has_fps) result.fps = nested.fps;
          if (nested.has_mtu) result.mtu = nested.mtu;
          if (nested.has_nalu_count) result.nalu_count = nested.nalu_count;
          if (nested.has_total_sent) result.total_sent = nested.total_sent;
          if (nested.has_upstream) result.upstream = nested.upstream;
          if (nested.has_loss) result.loss = nested.loss;
          return ControlMessage{Senkusha{result}};
        }
        case tkproto_SenkushaPayload_Command_CLIENT_MTU_COMMAND: {
          if (!payload.has_client_mtu_command)
            return failure(ControlCodecErrorReason::missing_payload);
          const auto& nested = payload.client_mtu_command;
          SenkushaClientMtuCommand result;
          result.id = nested.id;
          result.mtu_request = nested.mtu_req;
          result.state = nested.state;
          if (nested.has_mtu_down) result.mtu_down = nested.mtu_down;
          return ControlMessage{Senkusha{result}};
        }
      }
      return ControlMessage{Senkusha{
          UnknownSenkushaCommand{static_cast<std::int32_t>(payload.command)}}};
    }
    case tkproto_TakionMessage_PayloadType_CONNECTIONQUALITY: {
      if (!message.has_connection_quality_payload)
        return failure(ControlCodecErrorReason::missing_payload);
      const auto& payload = message.connection_quality_payload;
      ConnectionQuality result;
      if (payload.has_target_bitrate)
        result.target_bitrate = payload.target_bitrate;
      if (payload.has_upstream_bitrate)
        result.upstream_bitrate = payload.upstream_bitrate;
      if (payload.has_upstream_loss)
        result.upstream_loss = payload.upstream_loss;
      if (payload.has_disable_upstream_audio)
        result.disable_upstream_audio = payload.disable_upstream_audio;
      if (payload.has_rtt) result.rtt = payload.rtt;
      if (payload.has_loss) result.loss = payload.loss;
      return ControlMessage{result};
    }
    case tkproto_TakionMessage_PayloadType_PLAYTIMELEFT: {
      if (!message.has_play_time_left_payload)
        return failure(ControlCodecErrorReason::missing_payload);
      const auto& payload = message.play_time_left_payload;
      PlayTimeLeft result;
      result.minutes = payload.time_left;
      const auto reason = TakionWire::text_of(payload.reason);
      if (reason == "MaxLengthPlay")
        result.reason = PlayTimeReason::session_limit;
      else if (reason == "Maintenance")
        result.reason = PlayTimeReason::maintenance;
      else
        result.reason_text = std::string{reason};
      return ControlMessage{result};
    }
    case tkproto_TakionMessage_PayloadType_SERVERMESSAGE:
      if (!message.has_server_message_payload)
        return failure(ControlCodecErrorReason::missing_payload);
      return ControlMessage{ServerMessage{std::string{
          TakionWire::text_of(message.server_message_payload.payload)}}};
    case tkproto_TakionMessage_PayloadType_METRICSOVERLAY: {
      if (!message.has_metrics_overlay_payload)
        return failure(ControlCodecErrorReason::missing_payload);
      MetricsOverlay result;
      const auto& overlay = message.metrics_overlay_payload;
      if (!overlay.has_limited_metrics_overlay_payload)
        return ControlMessage{result};
      const auto& limited = overlay.limited_metrics_overlay_payload;
      if (limited.has_available_downstream_bandwidth_kbps)
        result.available_down_kbps =
            limited.available_downstream_bandwidth_kbps;
      if (limited.has_used_downstream_bandwidth_kbps)
        result.used_down_kbps = limited.used_downstream_bandwidth_kbps;
      if (limited.has_used_upstream_kbps)
        result.used_up_kbps = limited.used_upstream_kbps;
      if (limited.has_latest_round_trip_time_ms)
        result.round_trip_ms = limited.latest_round_trip_time_ms;
      if (limited.has_loss_pct) result.loss_percent = limited.loss_pct;
      if (limited.has_upstream_loss_pct)
        result.upstream_loss_percent = limited.upstream_loss_pct;
      if (limited.has_gateway_rtt_us)
        result.gateway_round_trip_us = limited.gateway_rtt_us;
      if (limited.has_audio_channels)
        result.audio_channels = limited.audio_channels;
      result.dynamic_range =
          std::string{TakionWire::text_of(limited.dynamic_range)};
      result.video_codec =
          std::string{TakionWire::text_of(limited.video_codec)};
      if (limited.has_width) result.width = limited.width;
      if (limited.has_height) result.height = limited.height;
      if (limited.has_corrupt_frame_count)
        result.corrupt_frames = limited.corrupt_frame_count;
      return ControlMessage{result};
    }
    case tkproto_TakionMessage_PayloadType_MICCONNECTION: {
      if (!message.has_mic_connection_payload)
        return failure(ControlCodecErrorReason::missing_payload);
      const auto& payload = message.mic_connection_payload;
      MicConnection result{payload.controller_id, payload.connected,
                           std::nullopt};
      if (payload.has_result) result.result = payload.result;
      return ControlMessage{result};
    }
    case tkproto_TakionMessage_PayloadType_DIRECTMESSAGE: {
      if (message.has_direct_message_payload &&
          message.direct_message_payload.direct_message_type ==
              tkproto_DirectMessagePayload_DirectMessageType_MICINFO) {
        MicInfoWire info;
        if (!info.decode(
                TakionWire::bytes_of(message.direct_message_payload.data)))
          return failure(ControlCodecErrorReason::missing_payload);
        MicInfo result;
        result.type = info->mic_info_type ==
                              tkproto_MicInfoPayload_MicInfoType_MIC_CONNECT
                          ? MicInfo::Type::connect
                          : MicInfo::Type::mute;
        if (info->has_controller_id) result.controller_id = info->controller_id;
        if (info->has_connected) result.connected = info->connected;
        if (info->has_muted) result.muted = info->muted;
        if (info->has_result) result.result = info->result;
        return ControlMessage{result};
      }
      if (!message.has_direct_message_payload ||
          message.direct_message_payload.direct_message_type !=
              tkproto_DirectMessagePayload_DirectMessageType_DEEPLINK)
        return ControlMessage{
            UnsupportedControlMessage{ControlPayloadType::direct_message}};
      DeepLinkWire link;
      if (!link.decode(
              TakionWire::bytes_of(message.direct_message_payload.data)) ||
          !tkproto_DeepLinkPayload_DeepLinkType_valid(
              static_cast<tkproto_DeepLinkPayload_DeepLinkType>(
                  link->deep_link_type)))
        return failure(ControlCodecErrorReason::missing_payload);
      DeepLink result{Wire::deep_link_of(link->deep_link_type),
                      std::nullopt,
                      std::string{DeepLinkWire::text_of(link->raw_data)},
                      std::string{DeepLinkWire::text_of(link->invitation_id)},
                      std::string{DeepLinkWire::text_of(link->session_id)},
                      std::string{DeepLinkWire::text_of(link->item_id)}};
      if (link->has_request_id) result.request_id = link->request_id;
      if (link->has_pad_unique_id) result.pad_unique_id = link->pad_unique_id;
      if (link->has_result) result.result = link->result;
      if (link->has_should_show) result.should_show = link->should_show;
      return ControlMessage{result};
    }
    case tkproto_TakionMessage_PayloadType_XMBCOMMAND: {
      if (!message.has_xmb_command_payload)
        return failure(ControlCodecErrorReason::missing_payload);
      const auto& payload = message.xmb_command_payload;
      XmbCommand result{Wire::xmb_command_of(payload.command), std::nullopt};
      if (payload.has_player_id) result.player_id = payload.player_id;
      return ControlMessage{result};
    }
    case tkproto_TakionMessage_PayloadType_CORRUPTFRAME:
      if (!message.has_corrupt_payload)
        return failure(ControlCodecErrorReason::missing_payload);
      return ControlMessage{CorruptFrame{message.corrupt_payload.start,
                                         message.corrupt_payload.end}};
    case tkproto_TakionMessage_PayloadType_CONTROLLERCONNECTION: {
      if (!message.has_controller_connection_payload)
        return failure(ControlCodecErrorReason::missing_payload);
      const auto& payload = message.controller_connection_payload;
      ControllerConnection result;
      if (payload.has_controller_id)
        result.controller_id = payload.controller_id;
      if (payload.has_connected) result.connected = payload.connected;
      if (payload.has_controller_type) {
        if (tkproto_ControllerConnectionPayload_ControllerType_valid(
                static_cast<tkproto_ControllerConnectionPayload_ControllerType>(
                    payload.controller_type)))
          result.controller_type =
              static_cast<ControllerConnectionType>(payload.controller_type);
        else
          result.controller_type = UnknownControllerConnectionType{
              static_cast<std::int32_t>(payload.controller_type)};
      }
      return ControlMessage{result};
    }
    case tkproto_TakionMessage_PayloadType_IDRREQUEST:
      return ControlMessage{IdrRequest{}};
    case tkproto_TakionMessage_PayloadType_TAKIONPROTOCOLREQUEST: {
      if (!message.has_takion_protocol_request)
        return failure(ControlCodecErrorReason::missing_payload);
      const auto& payload = message.takion_protocol_request;
      return ControlMessage{ProtocolRequest{std::vector<std::uint32_t>{
          payload.supported_takion_versions,
          payload.supported_takion_versions +
              payload.supported_takion_versions_count}}};
    }
    case tkproto_TakionMessage_PayloadType_TAKIONPROTOCOLREQUESTACK: {
      if (!message.has_takion_protocol_request_ack)
        return failure(ControlCodecErrorReason::missing_payload);
      ProtocolRequestAck acknowledgement;
      if (message.takion_protocol_request_ack.has_takion_protocol_version)
        acknowledgement.selected_version =
            message.takion_protocol_request_ack.takion_protocol_version;
      return ControlMessage{acknowledgement};
    }
    default:
      return ControlMessage{UnsupportedControlMessage{
          static_cast<ControlPayloadType>(message.type)}};
  }
}

}  // namespace libchiaking::net::protocol::takion
