/// @file
/// @brief Declares the Takion control messages the library models: the typed
/// form of the protobuf `tkproto::TakionMessage` carried on the reliable
/// control channel (proto/takion.proto).
///
/// Every message is one TakionMessage whose `type` names which payload field
/// it carries. The handshake exchange is BIG (client: version, keys, launch
/// specification) and BANG (console: acceptance, its keys); then STREAMINFO
/// describes the stream and the client acknowledges it, while heartbeats,
/// connection quality, IDR requests and the rest flow during play. Versions
/// are negotiated with TAKIONPROTOCOLREQUEST and its acknowledgement.
#pragma once

#include "libchiaking/detail/secret_string.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief The TakionMessage payload types, with their wire numbers.
enum class ControlPayloadType : std::uint32_t {
  /// @brief Client opens the session.
  big = 0,
  /// @brief Console answers BIG.
  bang = 1,
  /// @brief Not modelled.
  info = 2,
  /// @brief Keep-alive, no payload.
  heartbeat = 3,
  /// @brief Not modelled.
  packet_loss = 4,
  /// @brief A range of frames the client could not decode.
  corrupt_frame = 5,
  /// @brief Not modelled.
  cursor = 6,
  /// @brief Not modelled.
  timer = 7,
  /// @brief Either side ends the session.
  disconnect = 8,
  /// @brief Not modelled.
  log = 9,
  /// @brief Not modelled.
  header_request = 10,
  /// @brief Not modelled.
  debug = 11,
  /// @brief A Senkusha connection test command.
  senkusha = 12,
  /// @brief Console describes the stream's resolutions and audio.
  stream_info = 13,
  /// @brief Client acknowledges STREAMINFO.
  stream_info_acknowledgement = 14,
  /// @brief Opens, closes or toggles the system menu.
  xmb_command = 15,
  /// @brief Bitrate and loss figures.
  connection_quality = 16,
  /// @brief Not modelled.
  client_metric = 17,
  /// @brief Not modelled.
  play_time_left = 18,
  /// @brief Not modelled.
  server_message = 19,
  /// @brief Not modelled.
  fps_change = 20,
  /// @brief A controller was connected or disconnected.
  controller_connection = 21,
  /// @brief Session start information; travels as packet type 0x08, not here.
  client_info = 22,
  /// @brief Not modelled.
  video_capture = 23,
  /// @brief Not modelled.
  audio_capture = 24,
  /// @brief Client asks for a key frame.
  idr_request = 25,
  /// @brief Detached-tag trace; travels as packet type 0x0a, not here.
  gk_trace = 26,
  /// @brief Not modelled.
  periodic_timestamp = 27,
  /// @brief Not modelled.
  server_settings = 28,
  /// @brief A direct message; GENERICDATA ones drive generic-data transfers
  /// and DEEPLINK ones decode as DeepLink.
  direct_message = 29,
  /// @brief The client connects its microphone, or the server answers.
  mic_connection = 30,
  /// @brief The versions one side offers.
  takion_protocol_request = 31,
  /// @brief The version chosen from an offer.
  takion_protocol_request_acknowledgement = 32,
  /// @brief Audio output state.
  audio_state = 33,
  /// @brief Not modelled.
  pause_mode = 34,
  /// @brief A cloud server's own measurements of the stream.
  metrics_overlay = 35,
};

/// @brief HEARTBEAT, sent periodically to keep the session alive.
struct Heartbeat {
  /// @brief Compares two heartbeats.
  ///
  /// @return Always true.
  constexpr bool operator==(const Heartbeat&) const noexcept = default;
};

/// @brief The extended reason of a DISCONNECT or BANG.
struct DisconnectEventCode {
  /// @brief PS's event code.
  std::string event_code;
  /// @brief The event's name, when given.
  std::optional<std::string> name;
  /// @brief Its description, when given.
  std::optional<std::string> description;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const DisconnectEventCode&) const = default;
};

/// @brief DISCONNECT: the sender is ending the session.
struct Disconnect {
  /// @brief Free text; the library sends "Client Disconnecting".
  std::string reason;
  /// @brief An event code, when given.
  std::optional<DisconnectEventCode> extended_info;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const Disconnect&) const = default;
};

/// @brief TAKIONPROTOCOLREQUESTACK, standalone or inside a BANG.
struct ProtocolRequestAck {
  /// @brief The version chosen; empty when no offered version was acceptable.
  std::optional<std::uint32_t> selected_version;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const ProtocolRequestAck&) const = default;
};

/// @brief A secret text field (session key, launch specification) shared
/// between copies of a message without copying the secret.
///
/// Move-only: a second holder of the same immutable wiping string is made
/// only by share(), so every place that keeps the secret is visible.
/// Comparing compares the text. Safe to share between threads once made.
class SharedSecretText {
 public:
  /// @brief Holds no text.
  SharedSecretText() noexcept = default;
  /// @brief Takes a copy of text into a new wiping string.
  ///
  /// @param text The text.
  explicit SharedSecretText(std::string_view text)
      : secret_(std::make_shared<const SecretString>(text)) {}
  /// @brief Shares an existing secret.
  ///
  /// @param secret The secret; may be null for no text.
  explicit SharedSecretText(std::shared_ptr<const SecretString> secret) noexcept
      : secret_(std::move(secret)) {}
  /// @brief Not copyable; use share().
  SharedSecretText(const SharedSecretText&) = delete;
  /// @brief Not copy-assignable; use share().
  auto operator=(const SharedSecretText&) -> SharedSecretText& = delete;
  /// @brief Takes over another holder's text; the other is left empty.
  SharedSecretText(SharedSecretText&&) noexcept = default;
  /// @brief Takes over another holder's text; the other is left empty.
  ///
  /// @return This holder.
  auto operator=(SharedSecretText&&) noexcept -> SharedSecretText& = default;
  /// @brief Releases this holder's share of the text.
  ~SharedSecretText() = default;

  /// @brief Makes a second holder of the same immutable wiping string.
  ///
  /// @return The new holder; the text is wiped when the last holder goes.
  auto share() const noexcept -> SharedSecretText {
    return SharedSecretText{secret_};
  }

  /// @brief Reads the text.
  ///
  /// @return The text; empty when none; valid while any copy lives.
  auto view() const noexcept -> std::string_view {
    return secret_ ? secret_->view() : std::string_view{};
  }
  /// @brief Tells whether there is no text.
  ///
  /// @return Whether the text is empty.
  auto empty() const noexcept -> bool { return view().empty(); }
  /// @brief Compares the texts.
  ///
  /// @param other The other text.
  /// @return Whether both are the same.
  bool operator==(const SharedSecretText& other) const noexcept {
    return view() == other.view();
  }

 private:
  /// @brief The shared secret; null for none.
  std::shared_ptr<const SecretString> secret_;
};

/// @brief BIG, the client's opening message.
///
/// For a console session it carries the client's Takion version, the session
/// key from the session start, the launch specification, the encrypted key
/// and the ECDH public key and its signature. A Senkusha test sends only the
/// version and, for CloudStream, the session key.
struct Big {
  /// @brief The client's Takion version.
  std::uint32_t client_version{};
  /// @brief The session key.
  SharedSecretText session_key;
  /// @brief The launch specification text.
  SharedSecretText launch_spec;
  /// @brief The encrypted key bytes.
  std::vector<std::byte> encrypted_key;
  /// @brief The client's ECDH public key, when keys are exchanged.
  std::optional<std::vector<std::byte>> ecdh_pub_key;
  /// @brief The signature over that key, when keys are exchanged.
  std::optional<std::vector<std::byte>> ecdh_sig;

  /// @brief Makes a copy that shares the session key and launch
  /// specification with this message.
  ///
  /// @return The copy.
  auto clone() const -> Big {
    return Big{client_version, session_key.share(), launch_spec.share(),
               encrypted_key,  ecdh_pub_key,        ecdh_sig};
  }

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const Big&) const = default;
};

/// @brief BANG, the console's answer to BIG.
struct Bang {
  /// @brief The console's Takion version.
  std::uint32_t server_version{};
  /// @brief A token from the console.
  std::uint32_t token{};
  /// @brief Whether the console accepted the encrypted key.
  bool encrypted_key_accepted{};
  /// @brief Whether the console accepted the client's version.
  bool version_accepted{};
  /// @brief The console's session key.
  SharedSecretText session_key;
  /// @brief Why the console refused, when it did.
  std::optional<DisconnectEventCode> extended_info;
  /// @brief The console's version text, when given.
  std::optional<std::string> server_version_string;
  /// @brief The console's ECDH public key, when keys are exchanged.
  std::optional<std::vector<std::byte>> ecdh_pub_key;
  /// @brief The signature over that key.
  std::optional<std::vector<std::byte>> ecdh_sig;
  /// @brief A protocol request acknowledgement carried in the same
  /// TakionMessage.
  std::optional<ProtocolRequestAck> protocol_ack;

  /// @brief Makes a copy that shares the session key with this message.
  ///
  /// @return The copy.
  auto clone() const -> Bang {
    return Bang{server_version,
                token,
                encrypted_key_accepted,
                version_accepted,
                session_key.share(),
                extended_info,
                server_version_string,
                ecdh_pub_key,
                ecdh_sig,
                protocol_ack};
  }

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const Bang&) const = default;
};

/// @brief One resolution STREAMINFO offers.
struct StreamInfoResolution {
  /// @brief Width, in pixels.
  std::uint32_t width{};
  /// @brief Height, in pixels.
  std::uint32_t height{};
  /// @brief The codec header (parameter sets) for this resolution.
  std::vector<std::byte> video_header;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const StreamInfoResolution&) const = default;
};

/// @brief One audio channel STREAMINFO describes.
struct StreamInfoAudioChannel {
  /// @brief PS's channel type number.
  std::uint32_t audio_channel_type{};
  /// @brief The channel's codec header.
  std::vector<std::byte> audio_header;
  /// @brief Whether the channel is raw PCM, when said.
  std::optional<bool> is_raw_pcm;
  /// @brief Codec headers of the channel's audio profiles.
  std::vector<std::vector<std::byte>> profile_headers{};
  /// @brief The channel's FEC mode, when said.
  std::optional<std::uint32_t> fec_mode{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const StreamInfoAudioChannel&) const = default;
};

/// @brief STREAMINFO, the console's description of the stream.
///
/// The timeouts are passed on as the console sends them; the library does
/// not interpret their unit.
struct StreamInfo {
  /// @brief Resolutions, one per adaptive stream index.
  std::vector<StreamInfoResolution> resolutions;
  /// @brief The main audio codec header.
  std::vector<std::byte> audio_header;
  /// @brief Start timeout, when given.
  std::optional<std::uint32_t> start_timeout;
  /// @brief Idle (AFK) timeout, when given.
  std::optional<std::uint32_t> afk_timeout;
  /// @brief Idle disconnect timeout, when given.
  std::optional<std::uint32_t> afk_timeout_disconnect;
  /// @brief Congestion control interval, when given.
  std::optional<std::uint32_t> congestion_control_interval;
  /// @brief Audio channels.
  std::vector<StreamInfoAudioChannel> audio_channels;
  /// @brief The traffic class the console marks with, when given.
  std::optional<std::uint32_t> tos_value;
  /// @brief ECN-marked packets that trigger a traffic-class report, when given.
  std::optional<std::uint32_t> tos_report_packet_threshold;
  /// @brief Traffic-class report interval, in milliseconds, when given.
  std::optional<std::uint32_t> tos_report_interval_ms;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const StreamInfo&) const = default;
};

/// @brief STREAMINFOACK, sent on channel 9 once STREAMINFO is accepted.
struct StreamInfoAcknowledgement {
  /// @brief Compares two acknowledgements.
  ///
  /// @return Always true.
  constexpr bool operator==(const StreamInfoAcknowledgement&) const noexcept =
      default;
};

/// @brief Senkusha ECHO_COMMAND: switch the far end's probe echo on or off.
struct SenkushaEchoCommand {
  /// @brief Whether echo is to be on.
  bool state{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const SenkushaEchoCommand&) const noexcept =
      default;
};

/// @brief Senkusha MTU_COMMAND: ask the far end to send a video packet of a
/// size (inbound MTU test).
struct SenkushaMtuCommand {
  /// @brief Request id; the answer's frame index equals it.
  std::uint32_t id{};
  /// @brief Requested IP packet size, in bytes.
  std::uint32_t mtu_request{};
  /// @brief Size the far end sent, when it reports one.
  std::optional<std::uint32_t> mtu_sent;
  /// @brief Packets to send, when given.
  std::optional<std::uint32_t> number;
  /// @brief Delay between them, when given.
  std::optional<std::uint32_t> send_delay;
  /// @brief A delta field, when given; its meaning is not known.
  std::optional<std::uint32_t> delta;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const SenkushaMtuCommand&) const = default;
};

/// @brief Senkusha BANDWIDTH_COMMAND: ask for a stream at a rate, and the far
/// end's report of what it sent.
struct SenkushaBandwidthCommand {
  /// @brief Round id; the report echoes it.
  std::uint32_t id{};
  /// @brief Duration, in milliseconds.
  std::uint32_t time{};
  /// @brief Rate, in kilobits per second.
  std::optional<std::uint32_t> bandwidth;
  /// @brief Frames per second to pace at.
  std::optional<std::uint32_t> fps;
  /// @brief MTU to send at, in bytes.
  std::optional<std::uint32_t> mtu;
  /// @brief NALU count, when given.
  std::optional<std::uint32_t> nalu_count;
  /// @brief In a report, packets the far end sent.
  std::optional<std::uint32_t> total_sent;
  /// @brief Whether the test is upstream, when given.
  std::optional<bool> upstream;
  /// @brief A loss figure, when given.
  std::optional<double> loss;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const SenkushaBandwidthCommand&) const = default;
};

/// @brief Senkusha CLIENT_MTU_COMMAND: switch the far end's echo of outbound
/// MTU probes on (id 1) or off (id 2).
struct SenkushaClientMtuCommand {
  /// @brief Command id; 1 to start, 2 to stop.
  std::uint32_t id{};
  /// @brief MTU field: the inbound MTU when starting, the search's upper bound
  /// when stopping.
  std::uint32_t mtu_request{};
  /// @brief Whether echo is to be on.
  bool state{};
  /// @brief The inbound (downstream) MTU, when given.
  std::optional<std::uint32_t> mtu_down;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const SenkushaClientMtuCommand&) const = default;
};

/// @brief A Senkusha command whose number the library does not know.
struct UnknownSenkushaCommand {
  /// @brief The command number.
  std::int32_t command{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const UnknownSenkushaCommand&) const noexcept =
      default;
};

/// @brief Any Senkusha command.
using SenkushaCommand =
    std::variant<SenkushaEchoCommand, SenkushaMtuCommand,
                 SenkushaBandwidthCommand, SenkushaClientMtuCommand,
                 UnknownSenkushaCommand>;

/// @brief SENKUSHA, carried on channel 8.
struct Senkusha {
  /// @brief The command.
  SenkushaCommand command;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const Senkusha&) const = default;
};

/// @brief CONNECTIONQUALITY, bitrate and loss figures, passed through as
/// PS's protobuf carries them.
struct ConnectionQuality {
  /// @brief Target bitrate, when given.
  std::optional<std::uint32_t> target_bitrate;
  /// @brief Upstream bitrate, when given.
  std::optional<std::uint32_t> upstream_bitrate;
  /// @brief Upstream loss, when given.
  std::optional<float> upstream_loss;
  /// @brief Whether upstream audio is to be disabled, when given.
  std::optional<bool> disable_upstream_audio;
  /// @brief Round-trip time, when given.
  std::optional<double> rtt;
  /// @brief Loss, when given.
  std::optional<std::uint64_t> loss;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const ConnectionQuality&) const = default;
};

/// @brief CORRUPTFRAME: frames from `start` to `end` could not be decoded.
/// Sent on channel 2.
struct CorruptFrame {
  /// @brief First bad frame index.
  std::uint32_t start{};
  /// @brief Last bad frame index.
  std::uint32_t end{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const CorruptFrame&) const = default;
};

/// @brief The controller kinds CONTROLLERCONNECTION names.
enum class ControllerConnectionType : std::int32_t {
  /// @brief Not set.
  not_set = 0,
  /// @brief DualShock 3.
  dualshock3 = 1,
  /// @brief DualShock 4.
  dualshock4 = 2,
  /// @brief PS Vita.
  vita = 3,
  /// @brief An XInput pad.
  xinput = 4,
  /// @brief A mobile touch controller.
  mobile = 5,
  /// @brief DualSense.
  dualsense = 6,
  /// @brief PS VR2 Sense controller.
  vr2sense = 7,
};

/// @brief A controller kind number the library does not know.
struct UnknownControllerConnectionType {
  /// @brief The number.
  std::int32_t value{};

  /// @brief Compares two values.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(
      const UnknownControllerConnectionType&) const noexcept = default;
};

/// @brief A controller kind, known or not.
using ControllerConnectionTypeValue =
    std::variant<ControllerConnectionType, UnknownControllerConnectionType>;

/// @brief CONTROLLERCONNECTION: a controller came or went.
struct ControllerConnection {
  /// @brief The controller slot, when given.
  std::optional<std::int32_t> controller_id;
  /// @brief Whether it is connected, when given.
  std::optional<bool> connected;
  /// @brief Its kind, when given.
  std::optional<ControllerConnectionTypeValue> controller_type;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const ControllerConnection&) const = default;
};

/// @brief IDRREQUEST: ask the console for a key frame. Sent on channel 2.
struct IdrRequest {
  /// @brief Compares two requests.
  ///
  /// @return Always true.
  constexpr bool operator==(const IdrRequest&) const noexcept = default;
};

/// @brief TAKIONPROTOCOLREQUEST: the versions the sender can speak.
struct ProtocolRequest {
  /// @brief The offered versions.
  std::vector<std::uint32_t> supported_versions;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const ProtocolRequest&) const = default;
};

/// @brief The AUDIOSTATE kinds.
enum class AudioStateType : std::uint32_t {
  /// @brief A full state.
  full = 0,
  /// @brief Flags.
  flags = 1,
  /// @brief Listening angle.
  angle = 2,
  /// @brief HRTF setting.
  hrtf = 3,
  /// @brief TV configuration.
  tv_config = 4,
  /// @brief Output port states.
  port_states = 5,
  /// @brief Output channel count; for versions with audio_channel_count_state.
  channel_count = 6,
};

/// @brief AUDIOSTATE: the client's audio output state.
struct AudioState {
  /// @brief Which state.
  AudioStateType type{AudioStateType::full};
  /// @brief Its bytes, when any.
  std::optional<std::vector<std::byte>> data;

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const AudioState&) const = default;
};

/// @brief Why a cloud service will close the game.
enum class PlayTimeReason : std::uint8_t {
  /// @brief The session reached its longest allowed length.
  session_limit,
  /// @brief The server is going down for maintenance.
  maintenance,
  /// @brief A reason the library does not know.
  other,
};

/// @brief A cloud service's warning that it will close the game.
struct PlayTimeLeft {
  /// @brief Minutes until the game closes.
  std::uint32_t minutes{};
  /// @brief Why it closes.
  PlayTimeReason reason{PlayTimeReason::other};
  /// @brief The reason exactly as the server wrote it, kept for diagnostics
  /// only when it is not one the client knows; empty otherwise.
  std::string reason_text{};

  /// @brief Compares two values.
  ///
  /// @return Whether both are the same.
  bool operator==(const PlayTimeLeft&) const = default;
};

/// @brief A cloud service's server message, whose text the library does not
/// interpret yet.
struct ServerMessage {
  /// @brief The message text.
  std::string text;

  /// @brief Compares two values.
  ///
  /// @return Whether both are the same.
  bool operator==(const ServerMessage&) const = default;
};

/// @brief XMBCOMMAND: opens, closes or toggles the console's system menu, as a
/// cloud client does for the PS button.
struct XmbCommand {
  /// @brief What to do with the system menu.
  enum class Command : std::uint8_t {
    /// @brief Closes it.
    off,
    /// @brief Opens it.
    on,
    /// @brief Opens it when closed and closes it when open.
    toggle,
  };

  /// @brief What to do.
  Command command{Command::toggle};
  /// @brief The player whose menu it is, when given.
  std::optional<std::uint32_t> player_id;

  /// @brief Compares two values.
  ///
  /// @return Whether both are the same.
  bool operator==(const XmbCommand&) const = default;
};

/// @brief A DIRECTMESSAGE carrying a deep link, as a cloud client sends to
/// report its UI state and a cloud service sends for invitations and alerts.
struct DeepLink {
  /// @brief What the deep link is about.
  enum class Type : std::uint8_t {
    /// @brief Saved data.
    save_data,
    /// @brief A game invitation.
    invite,
    /// @brief A game alert.
    game_alert,
    /// @brief The system service status.
    system_service_status,
    /// @brief The debug system menu.
    debug_vsh_menu,
    /// @brief A raw `psaction:` text in raw_data.
    raw,
  };

  /// @brief What the deep link is about.
  Type type{Type::raw};
  /// @brief The sender's request number, when given.
  std::optional<std::uint32_t> request_id;
  /// @brief The raw text, such as
  /// `psaction:systemservicestatus?isSystemUiOverlaid=1`.
  std::string raw_data;
  /// @brief The invitation id, for an invitation.
  std::string invitation_id;
  /// @brief The session id, for an invitation.
  std::string session_id;
  /// @brief The item id, for an alert.
  std::string item_id;
  /// @brief The controller the deep link acts for, when given.
  std::optional<std::uint32_t> pad_unique_id{};
  /// @brief The service's result code, in a reply.
  std::optional<std::uint32_t> result{};
  /// @brief Whether the service shows the deep link's target, in a reply.
  std::optional<bool> should_show{};

  /// @brief Compares two values.
  ///
  /// @return Whether both are the same.
  bool operator==(const DeepLink&) const = default;
};

/// @brief METRICSOVERLAY: a cloud server's own measurements of the stream,
/// sent about once a second.
struct MetricsOverlay {
  /// @brief Downstream bandwidth available, in kilobits per second.
  std::optional<std::uint32_t> available_down_kbps;
  /// @brief Downstream bandwidth used, in kilobits per second.
  std::optional<std::uint32_t> used_down_kbps;
  /// @brief Upstream bandwidth used, in kilobits per second.
  std::optional<std::uint32_t> used_up_kbps;
  /// @brief Latest round trip, in milliseconds.
  std::optional<std::uint32_t> round_trip_ms;
  /// @brief Downstream loss, in percent.
  std::optional<float> loss_percent;
  /// @brief Upstream loss, in percent.
  std::optional<float> upstream_loss_percent;
  /// @brief Round trip to the gateway, in microseconds.
  std::optional<std::uint32_t> gateway_round_trip_us;
  /// @brief Audio channels.
  std::optional<std::uint32_t> audio_channels;
  /// @brief The dynamic range name the server gives, such as SDR or HDR.
  std::string dynamic_range;
  /// @brief The video codec name the server gives.
  std::string video_codec;
  /// @brief Video width in pixels.
  std::optional<std::uint32_t> width;
  /// @brief Video height in pixels.
  std::optional<std::uint32_t> height;
  /// @brief Corrupt frames the server counted.
  std::optional<std::uint32_t> corrupt_frames;

  /// @brief Compares two values.
  ///
  /// @return Whether both are the same.
  bool operator==(const MetricsOverlay&) const = default;
};

/// @brief MICCONNECTION: the client connects its microphone, or the
/// server answers whether it accepted the connection.
struct MicConnection {
  /// @brief The controller slot the microphone belongs to, from 0.
  std::int32_t controller_id{};
  /// @brief Whether the microphone is connected.
  bool connected{};
  /// @brief The server's answer, absent in the client's request.
  std::optional<bool> result;

  /// @brief Compares two values.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const MicConnection&) const noexcept = default;
};

/// @brief A MICINFO direct message: the client mutes or unmutes its
/// microphone, or the server answers whether it applied the change.
struct MicInfo {
  /// @brief What the message reports.
  enum class Type : std::uint8_t {
    /// @brief The microphone connection.
    connect,
    /// @brief The microphone mute.
    mute,
  };

  /// @brief What the message reports.
  Type type{Type::mute};
  /// @brief The controller slot the microphone belongs to, when given.
  std::optional<std::uint32_t> controller_id;
  /// @brief Whether the microphone is connected, when given.
  std::optional<bool> connected;
  /// @brief Whether the microphone is muted, when given.
  std::optional<bool> muted;
  /// @brief The server's answer, absent in the client's request.
  std::optional<bool> result;

  /// @brief Compares two values.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const MicInfo&) const noexcept = default;
};

/// @brief A control message the library receives but does not model.
struct UnsupportedControlMessage {
  /// @brief Its type number.
  ControlPayloadType type;

  /// @brief Compares two values.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const UnsupportedControlMessage&) const noexcept =
      default;
};

/// @brief Any control message.
using ControlMessage =
    std::variant<Heartbeat, Disconnect, Big, Bang, StreamInfo,
                 StreamInfoAcknowledgement, Senkusha, ConnectionQuality,
                 CorruptFrame, ControllerConnection, IdrRequest,
                 ProtocolRequest, ProtocolRequestAck, AudioState, PlayTimeLeft,
                 ServerMessage, XmbCommand, DeepLink, MetricsOverlay,
                 MicConnection, MicInfo, UnsupportedControlMessage>;

}  // namespace libchiaking::net::protocol::takion
