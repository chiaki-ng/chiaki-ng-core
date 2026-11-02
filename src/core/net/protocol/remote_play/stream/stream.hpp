/// @file
/// @brief The vocabulary a stream speaks to the play context: its phases, its
/// errors, the stage a failure happened in, why the console ended it, the
/// tuning a caller may apply, every event the stream reports, the audio state
/// kinds, and the commands a caller sends into it.
///
/// Home, internet and cloud links all report through StreamEvent; the play
/// context turns the events into the public session's callbacks.
#pragma once
#include "src/core/net/protocol/remote_play/stream/diagnostics.hpp"

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/internet_route.hpp"
#include "libchiaking/detail/play/access_code.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"
#include "src/core/net/protocol/remote_play/rp_reason.hpp"
#include "src/core/net/protocol/remote_play/stream/audio_layout.hpp"
#include "src/core/net/protocol/remote_play/stream/controller.hpp"
#include "src/core/net/protocol/remote_play/stream/datacenter_ping.hpp"
#include "src/core/net/protocol/remote_play/video_codec.hpp"
#include "src/core/net/protocol/takion/detail/control_message.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::takion {
/// @brief The Takion tap that may observe a stream's packets, declared here
/// to keep the include light.
class SourceTap;
}  // namespace libchiaking::net::protocol::takion

namespace libchiaking::net::protocol::remote_play {

/// @brief The default ceiling on the share of lost packets the stream
/// reports as acceptable to the console (5 %).
inline constexpr double default_packet_loss_max = 0.05;

/// @brief The phase of a stream, reported in event::StateChanged.
enum class StreamState : std::uint8_t {
  /// @brief Not started.
  idle,
  /// @brief Reaching the console: session request, control channel and, over
  /// PSN, the routes.
  establishing_route,
  /// @brief Calibrating and handshaking the Takion stream.
  negotiating_stream,
  /// @brief Media is flowing.
  streaming,
  /// @brief Stopping on request.
  stopping,
  /// @brief Stopped; nothing more will arrive.
  stopped,
  /// @brief Ended by a failure; see event::Failed.
  failed,
};

/// @brief Which part of starting or running the stream failed.
enum class StreamFailure : std::uint8_t {
  /// @brief The console could not be reached or its routes not set up.
  route_establishment_failed,
  /// @brief The session or control requests, or the Takion handshake, failed.
  negotiation_failed,
  /// @brief The running stream failed.
  pipeline_failed,
  /// @brief The stream was stopped before it finished starting.
  cancelled,
  /// @brief Stopping did not complete cleanly.
  shutdown_failed,
};

/// @brief What an audio state message body describes; the value is the Takion
/// audio state type.
enum class AudioStateKind : std::uint8_t {
  /// @brief The whole audio state.
  full = 0,
  /// @brief A flags change: a selector byte, little-endian 32-bit available
  /// bits and little-endian 32-bit flags (see audio_state_flags()). Used to
  /// toggle 3D audio on PS5 cloud.
  flags = 1,
  /// @brief The listener's angle.
  angle = 2,
  /// @brief The head-related transfer function.
  hrtf = 3,
  /// @brief The TV's audio configuration.
  tv_config = 4,
  /// @brief The audio port states.
  port_states = 5,
  /// @brief The audio profile, one byte; used to pick stereo or 3D audio from a
  /// PS5.
  channel_count = 6,
};

/// @brief The size of a flags body, in bytes.
inline constexpr std::size_t audio_state_flags_size = 9;

/// @brief Encodes a flags body.
///
/// @param selector Which flag group changes.
/// @param available_bits Which flag bits the client knows, little-endian.
/// @param flags The flag values, little-endian.
/// @return The nine bytes.
constexpr auto audio_state_flags(std::uint8_t selector,
                                 std::uint32_t available_bits,
                                 std::uint32_t flags) noexcept
    -> std::array<std::byte, audio_state_flags_size> {
  std::array<std::byte, audio_state_flags_size> result{};
  const std::span<std::byte> bytes{result};
  result[0] = std::byte{selector};
  libchiaking::bytes::store_le(bytes.subspan(1, sizeof available_bits),
                               available_bits);
  libchiaking::bytes::store_le(bytes.subspan(5, sizeof flags), flags);
  return result;
}

/// @brief The size of a full audio state body, in bytes.
inline constexpr std::size_t audio_state_full_size = 73;
/// @brief The size of a port states body, in bytes.
inline constexpr std::size_t audio_state_port_states_size = 33;
/// @brief The controller slots a port states body describes.
inline constexpr std::size_t audio_state_port_slots = 4;

/// @brief The group of audio ports a port states body describes.
enum class AudioPortGroup : std::uint8_t {
  /// @brief The ports of each controller.
  controller = 0,
  /// @brief The main output.
  main = 1,
};

/// @brief Where an audio port plays; the values are output bits.
enum class AudioPortOutput : std::uint16_t {
  /// @brief Nowhere.
  none = 0x0000,
  /// @brief The main output.
  main = 0x0001,
  /// @brief The speaker built into the controller.
  controller_speaker = 0x0004,
  /// @brief A headset plugged into the controller.
  controller_headset = 0x0040,
};

/// @brief The volume of a port playing at full level.
inline constexpr std::int16_t audio_port_full_volume = 127;

/// @brief Encodes a full audio state describing a 7.1 speaker layout.
///
/// @return The 73 bytes: layout type 3, little-endian 32-bit present speaker
///     bits and layout flags, then a little-endian azimuth and elevation per
///     speaker; the unused speaker slots stay zero.
constexpr auto audio_state_full_layout() noexcept
    -> std::array<std::byte, audio_state_full_size> {
  constexpr std::uint8_t layout_type = 3;
  constexpr std::uint32_t speakers_present = 0xffU;
  constexpr std::uint32_t layout_flags = 1;
  constexpr std::array<std::array<std::int16_t, 2>, 8> positions{{{30, 0},
                                                                  {-30, 0},
                                                                  {0, 0},
                                                                  {-360, -360},
                                                                  {90, 0},
                                                                  {-90, 0},
                                                                  {135, 0},
                                                                  {-135, 0}}};
  std::array<std::byte, audio_state_full_size> result{};
  const std::span<std::byte> bytes{result};
  result[0] = std::byte{layout_type};
  libchiaking::bytes::store_le(bytes.subspan(1, sizeof speakers_present),
                               speakers_present);
  libchiaking::bytes::store_le(bytes.subspan(5, sizeof layout_flags),
                               layout_flags);
  for (std::size_t speaker = 0; speaker < positions.size(); ++speaker)
    for (std::size_t axis = 0; axis < 2; ++axis)
      libchiaking::bytes::store_le(
          bytes.subspan(9 + speaker * 4 + axis * 2, sizeof(std::uint16_t)),
          static_cast<std::uint16_t>(positions[speaker][axis]));
  return result;
}

/// @brief Encodes a port states body that routes one controller slot.
///
/// @param group Which ports the body describes.
/// @param slot The controller slot, below audio_state_port_slots; any other
///     slot leaves every port unset.
/// @param output Where the slot's port plays.
/// @param volume The port's volume.
/// @return The 33 bytes: the group, then little-endian 16-bit outputs, 16-bit
///     volumes and 32-bit flags for each slot.
constexpr auto audio_state_port_states(AudioPortGroup group, std::size_t slot,
                                       AudioPortOutput output,
                                       std::int16_t volume) noexcept
    -> std::array<std::byte, audio_state_port_states_size> {
  std::array<std::byte, audio_state_port_states_size> result{};
  const std::span<std::byte> bytes{result};
  result[0] = std::byte{static_cast<std::uint8_t>(group)};
  if (slot >= audio_state_port_slots) return result;
  libchiaking::bytes::store_le(bytes.subspan(1 + slot * 2, 2),
                               static_cast<std::uint16_t>(output));
  libchiaking::bytes::store_le(
      bytes.subspan(1 + audio_state_port_slots * 2 + slot * 2, 2),
      static_cast<std::uint16_t>(volume));
  return result;
}

/// @brief Why a stream call or start failed.
enum class StreamError : std::uint8_t {
  /// @brief The call does not fit the stream's phase.
  invalid_state,
  /// @brief The start configuration is unusable.
  invalid_configuration,
  /// @brief A platform provider the stream needs is missing.
  provider_unavailable,
  /// @brief The control channel failed.
  ctrl_failed,
  /// @brief The route to the console failed or was cancelled.
  route_failed,
  /// @brief The media pipeline failed.
  pipeline_failed,
  /// @brief A controller state or seat change was refused.
  controller_rejected,
  /// @brief A login passcode was refused or could not be sent.
  pin_rejected,
  /// @brief Stopping did not complete cleanly.
  shutdown_failed,
  /// @brief The control request's socket failed.
  ctrl_network_failure,
  /// @brief The control request timed out.
  ctrl_timeout,
  /// @brief The console closed the control request early.
  ctrl_truncated,
  /// @brief The control reply could not be read.
  ctrl_invalid_response,
  /// @brief The control reply head was too large.
  ctrl_too_large,
  /// @brief The console refused the control request.
  ctrl_refused,
  /// @brief Reserved; nothing produces it today.
  ctrl_handoff_failure,
  /// @brief The console closed the session request early.
  session_init_truncated,
  /// @brief The session request's socket failed.
  session_init_network_failure,
  /// @brief The session request timed out.
  session_init_timeout,
  /// @brief The session reply could not be read.
  session_init_invalid_response,
  /// @brief The session reply head was too large.
  session_init_too_large,
  /// @brief The console refused the session request.
  session_init_refused,
  /// @brief The console wants an RP version the client could not settle on.
  session_init_version_mismatch,
  /// @brief The console is streaming to another client.
  session_init_in_use,
  /// @brief An argument is out of range.
  invalid_argument,
  /// @brief Nothing listens on the session port; the console is likely asleep.
  session_init_connection_refused,
  /// @brief Nothing listens on the control port.
  ctrl_connection_refused,
  /// @brief The platform ran out of sockets.
  resource_exhausted,
};

/// @brief What an event::EncodedMedia carries; the payload layout is fixed
/// per kind (see PublicMediaEvent).
enum class EncodedMediaKind : std::uint8_t {
  /// @brief One encoded audio frame (Opus, or raw PCM when the format says so);
  /// an empty payload marks a lost frame for concealment.
  audio = 0,
  /// @brief One encoded video frame, or a codec header when profile_header is
  /// set.
  video = 1,
  /// @brief DualSense haptic audio for pad_index.
  haptics = 2,
  /// @brief A rumble: player, left motor, right motor and a fourth byte.
  rumble = 3,
  /// @brief Adaptive trigger effects, 25 bytes.
  trigger_effects = 4,
  /// @brief The console reset the motion sensors; empty.
  motion_reset = 5,
  /// @brief The haptics intensity, one byte.
  haptic_intensity = 6,
  /// @brief The trigger effect intensity, one byte.
  trigger_intensity = 7,
  /// @brief The light bar colour, three bytes.
  led_color = 8,
  /// @brief The player indicator, one byte.
  player_index = 9,
  /// @brief A pad the console confirmed: index and light bar colour.
  pad_confirmed = 10,
  /// @brief A video frame lost to FEC; one byte, 1 when a keyframe was asked
  /// for.
  video_fec_failure = 11,
  /// @brief An audio codec header for a profile switch, with the parsed
  /// format.
  audio_profile = 12,
  /// @brief Motion sensor settings, two bytes.
  motion_settings = 13,
  /// @brief One encoded frame of the party voice chat a PS4 host sends beside
  /// the game audio; an empty payload marks a lost frame for concealment.
  voice_audio = 14,
  /// @brief The party voice chat's codec header, with the parsed format.
  voice_profile = 15,
  /// @brief One mono Opus frame for the speaker built into the controller at
  /// pad_index; an empty payload marks a lost frame for concealment.
  controller_speaker = 16
};

/// @brief The stage a start or a running stream was in when it failed, used
/// to explain failures and time each step.
enum class FailureStage : std::uint8_t {
  /// @brief Not known.
  unknown,
  /// @brief Finding the console's address.
  resolve,
  /// @brief Registering with the console.
  registration,
  /// @brief The session request.
  session_init,
  /// @brief The control request.
  ctrl_startup,
  /// @brief Waiting for the session id.
  ctrl_session_id,
  /// @brief Sending the post-id announcements.
  ctrl_features,
  /// @brief Receiving on the control channel.
  ctrl_receive,
  /// @brief Senkusha calibration of the stream route.
  calibration,
  /// @brief Moving a PSN stream onto its data route.
  route_switch,
  /// @brief Connecting the Takion stream.
  active_connect,
  /// @brief The BIG/BANG key exchange.
  active_bang,
  /// @brief Installing the stream ciphers.
  active_crypto,
  /// @brief Receiving and checking the stream info.
  stream_info,
  /// @brief Receiving media.
  media,
  /// @brief Sending controller input.
  controller,
  /// @brief Opening the PSN session.
  psn_open,
  /// @brief Starting Remote Play through PSN.
  psn_start,
  /// @brief Hole punching the PSN control route.
  control_holepunch,
  /// @brief Hole punching the PSN data route.
  data_holepunch,
  /// @brief Provisioning a cloud stream.
  cloud_provision,
  /// @brief Preparing the active Takion session.
  active_prepare,
  /// @brief Stopping.
  shutdown,
  /// @brief Establishing the cloud identity.
  cloud_identity,
  /// @brief Getting the cloud client ids.
  cloud_client_ids,
  /// @brief Reading the cloud configuration.
  cloud_config,
  /// @brief Starting the cloud session.
  cloud_start,
  /// @brief The cloud OAuth step.
  cloud_oauth,
  /// @brief Authorising the cloud stream.
  cloud_authorize,
  /// @brief Taking the cloud session lock.
  cloud_lock,
  /// @brief Measuring and choosing the datacenter.
  cloud_datacenter,
  /// @brief Allocating a cloud server.
  cloud_allocate,
  /// @brief Resolving the cloud title.
  cloud_title,
  /// @brief Checking the title may be launched in the cloud.
  cloud_launch_check,
};
/// @brief Why a stream ended, in the terms of the reference client's quit
/// reasons.
enum class QuitReason : std::uint8_t {
  /// @brief The stream has not ended, or ended for no stated reason.
  none,
  /// @brief The caller stopped it.
  stopped,
  /// @brief The console refused the session request for another reason.
  session_request_unknown,
  /// @brief Nothing listened on the session port.
  session_request_connection_refused,
  /// @brief The console is streaming to another client.
  session_request_rp_in_use,
  /// @brief The console's Remote Play service failed.
  session_request_rp_crash,
  /// @brief The console wants another RP version.
  session_request_rp_version_mismatch,
  /// @brief The control channel failed after it started.
  ctrl_unknown,
  /// @brief The control request failed.
  ctrl_connect_failed,
  /// @brief Nothing listened on the control port.
  ctrl_connection_refused,
  /// @brief The stream failed for a reason the console did not state.
  stream_connection_unknown,
  /// @brief The console disconnected the stream.
  stream_connection_remote_disconnected,
  /// @brief The console disconnected because it is shutting down or going to
  /// rest.
  stream_connection_remote_shutdown,
  /// @brief The cloud server disconnected because the streaming time limit
  /// was reached.
  stream_connection_time_limit_reached,
  /// @brief The cloud server ended a game trial whose play time ran out,
  /// after warning that it was running out.
  stream_connection_trial_ended,
  /// @brief PSN registration failed.
  psn_regist_failed,
};

/// @brief Whether a quit reason means something went wrong.
///
/// @param reason The reason.
/// @return False for a stop by the caller, a console shutdown, a reached
///     time limit or an ended trial, true otherwise.
[[nodiscard]] constexpr auto quit_reason_is_error(QuitReason reason) noexcept
    -> bool {
  return reason != QuitReason::stopped &&
         reason != QuitReason::stream_connection_remote_shutdown &&
         reason != QuitReason::stream_connection_time_limit_reached &&
         reason != QuitReason::stream_connection_trial_ended;
}

/// @brief The longest quit text kept, in characters.
inline constexpr std::size_t quit_text_limit = 255;

/// @brief The caller's adjustments to how a stream runs.
///
/// Plain data.
struct SessionTuning {
  /// @brief Drop received audio instead of passing it on.
  bool disable_audio{};
  /// @brief Drop received video, and ask the console for the smallest picture.
  bool disable_video{};
  /// @brief A Takion version to use instead of the target's own; 0 for none.
  std::uint32_t takion_version_override{};
  /// @brief The share of lost packets reported acceptable, 0 to 1.
  double packet_loss_max{default_packet_loss_max};
  /// @brief A tap that observes the stream's packets, or null.
  takion::SourceTap* source_tap{};
  /// @brief Whether to negotiate the Takion version with the console.
  bool negotiate_takion_version{};
  /// @brief The audio channels to ask for (0, 2, 6, 8 or 12; 0 for the
  /// default).
  std::uint64_t requested_audio_channels{};
  /// @brief The widest audio layout a cloud service may send (0, 2, 6, 8 or
  /// 12; 0 for the service default).
  std::uint64_t max_audio_channels{};
  /// @brief Whether to pass on the audio of the speaker built into each
  /// controller instead of dropping it.
  bool controller_speaker{};

  /// @brief Whether the tuning is usable.
  ///
  /// @return True when the loss ceiling is in 0..1 and the channel request is
  ///     valid.
  constexpr auto valid() const noexcept -> bool {
    return packet_loss_max >= 0.0 && packet_loss_max <= 1.0 &&
           valid_requested_audio_channels(requested_audio_channels) &&
           valid_requested_audio_channels(max_audio_channels);
  }
};

/// @brief Why a stream ended and what the console said.
struct QuitDetail {
  /// @brief The reason.
  QuitReason reason{QuitReason::none};
  /// @brief The console's own text, at most quit_text_limit characters.
  std::string text;
};

/// @brief The longest setup progress text kept, in characters.
inline constexpr std::size_t setup_progress_text_limit = 159;

/// @brief The events a stream reports.
namespace event {
/// @brief A start reached a stage.
struct SetupProgress {
  /// @brief The stage.
  FailureStage stage;
  /// @brief Microseconds since the start began.
  std::uint64_t elapsed_us{};
  /// @brief A note about the stage, at most setup_progress_text_limit
  /// characters.
  std::string text{};
  /// @brief The PSN route step, for internet starts.
  std::optional<progress::InternetRoute> route{};
};
/// @brief The stream entered a phase.
struct StateChanged {
  /// @brief The phase.
  StreamState state;
  /// @brief Why it ended, when the phase is an end.
  QuitReason quit_reason{QuitReason::none};
  /// @brief The console's text about the end.
  std::string quit_text{};
};
/// @brief The console wants the user's login passcode.
struct PinRequired {
  /// @brief Whether the previous passcode was refused.
  bool incorrect{};
};
/// @brief The audio format a stream sends.
struct AudioFormat {
  /// @brief The channel count.
  std::uint8_t channels{};
  /// @brief Bits per sample.
  std::uint8_t bits{};
  /// @brief Samples per second.
  std::uint32_t rate{};
  /// @brief Samples per channel in one frame.
  std::uint32_t frame_size{};
  /// @brief The Opus multistream stream count.
  std::uint8_t streams{};
  /// @brief How many of the streams are coupled.
  std::uint8_t coupled_streams{};
  /// @brief The Opus channel mapping.
  std::array<std::uint8_t, audio_max_channels> mapping{};
  /// @brief The speaker of each channel, when known.
  std::array<AudioChannelPosition, audio_max_channels> positions{};

  /// @brief Compares every field.
  ///
  /// @return True when equal.
  constexpr bool operator==(const AudioFormat&) const noexcept = default;
};
/// @brief One picture size the console may stream.
struct VideoProfile {
  /// @brief Width in pixels.
  std::uint32_t width{};
  /// @brief Height in pixels.
  std::uint32_t height{};
  /// @brief The codec header (parameter sets) for this size.
  std::vector<std::byte> video_header;

  /// @brief Compares every field.
  ///
  /// @return True when equal.
  bool operator==(const VideoProfile&) const = default;
};
/// @brief What a home start actually asked the console for after the
/// console's server type was known.
struct EffectiveLaunch {
  /// @brief The codec.
  VideoCodec codec{VideoCodec::h264};
  /// @brief Width in pixels.
  std::uint32_t width{};
  /// @brief Height in pixels.
  std::uint32_t height{};
  /// @brief Highest frame rate.
  std::uint32_t max_fps{};
  /// @brief Bitrate in kilobits per second.
  std::uint32_t bitrate_kbps{};

  /// @brief Compares every field.
  ///
  /// @return True when equal.
  constexpr bool operator==(const EffectiveLaunch&) const noexcept = default;
};
/// @brief The size of an audio header in the stream info, in bytes.
inline constexpr std::size_t audio_header_size = 14;
/// @brief The console's stream info was accepted: media follows.
struct StreamInfoReady {
  /// @brief The audio format.
  AudioFormat audio;
  /// @brief The picture sizes, indexed by adaptive stream index.
  std::vector<VideoProfile> profiles;
  /// @brief What a home start asked for, when known.
  std::optional<EffectiveLaunch> launch{};
  /// @brief The raw audio header.
  std::array<std::byte, audio_header_size> audio_header{};
};
/// @brief The console started or stopped hiding the picture.
struct DisplayProhibited {
  /// @brief Whether the picture is hidden.
  bool prohibited{};
};
/// @brief A cloud service warned that it will close the game.
struct PlayTimeLeft {
  /// @brief The warning.
  takion::PlayTimeLeft left;
};
/// @brief A PS5 cloud service sent an addressed message for the frontend.
struct CloudMessage {
  /// @brief The address it was sent to.
  std::uint32_t address{};
  /// @brief Its body.
  std::vector<std::byte> body;
};
/// @brief A game opened the on-screen keyboard.
struct KeyboardOpen {
  /// @brief Its text.
  std::string text;
};
/// @brief The on-screen keyboard's text changed on the console.
struct KeyboardTextChange {
  /// @brief The new text.
  std::string text;
};
/// @brief The console closed the on-screen keyboard.
struct KeyboardRemoteClose {};
/// @brief The console wants a couch user's passcode.
struct PadPasscodeRequest {
  /// @brief The seat.
  std::uint8_t index{};
  /// @brief Whether the previous passcode was refused.
  bool retry{};
};
/// @brief A couch seat could not join.
struct PadJoinFailed {
  /// @brief The seat.
  std::uint8_t index{};
  /// @brief The console's status or result byte.
  std::uint8_t status{};
};
/// @brief The console confirmed a couch seat's pad.
struct PadAnnounced {
  /// @brief The seat.
  std::uint8_t index{};
  /// @brief The pad's light bar colour.
  std::array<std::uint8_t, 3> led{};
};
/// @brief The console dropped a couch seat.
struct PadDropped {
  /// @brief The seat.
  std::uint8_t index{};
};
/// @brief The console could not resolve a couch seat's account.
struct PadAccountUnresolvable {
  /// @brief The seat.
  std::uint8_t index{};
};
/// @brief Media or controller feedback from the console.
struct EncodedMedia {
  /// @brief What it carries.
  EncodedMediaKind kind;
  /// @brief The payload.
  std::vector<std::byte> bytes;
  /// @brief The video frame index (video, FEC failures).
  std::uint16_t frame_index{};
  /// @brief The Takion video codec byte (3 H.264, 6 H.265).
  std::uint8_t codec{};
  /// @brief The picture size index the frame uses.
  std::uint8_t adaptive_stream_index{};
  /// @brief Frames lost since the last delivered one.
  std::int32_t frames_lost{};
  /// @brief Whether the frame was pointed at an older reference picture
  /// because its own was lost.
  bool frame_recovered{};
  /// @brief Whether the payload is a codec header rather than a frame.
  bool profile_header{};
  /// @brief The pad haptics are for.
  std::uint8_t pad_index{};
  /// @brief The audio format, for audio profile headers.
  AudioFormat audio{};
  /// @brief Whether the audio is raw PCM rather than Opus.
  bool audio_raw_pcm{};
  /// @brief The frame's dynamic range flag bit.
  bool dynamic_range_flag{};
  /// @brief Whether the console asked that the frame not be shown.
  bool suppress_presentation{};
};
/// @brief The stream failed.
struct Failed {
  /// @brief Which part failed.
  StreamFailure reason;
};
/// @brief A PSN start registered the console.
struct Registered {
  /// @brief The registration.
  RegisteredConsole console;
};
/// @brief A cloud start measured its datacenters.
struct CloudDatacenters {
  /// @brief The measurements.
  std::vector<cloud::DatacenterPing> datacenters;
};
/// @brief A cloud start chose a datacenter.
struct CloudDatacenterSelected {
  /// @brief Its name.
  std::string name;
  /// @brief Its round trip, in milliseconds.
  int rtt_ms{};
  /// @brief Whether the caller forced the choice.
  bool forced{};
};
/// @brief The console stated its nickname.
struct NicknameReceived {
  /// @brief The nickname.
  std::string nickname;
};
/// @brief The console started or stopped sending nothing for a while as the
/// stream plays; the link that emits it decides how long.
struct ConnectionWaiting {
  /// @brief Whether nothing is arriving.
  bool waiting{};
};
/// @brief A PSN start began or finished hole punching.
struct Holepunch {
  /// @brief Whether it finished.
  bool finished{};
};
}  // namespace event

/// @brief Anything a stream reports.
using StreamEvent = std::variant<
    event::StateChanged, event::PinRequired, event::StreamInfoReady,
    event::EncodedMedia, event::Failed, event::SetupProgress,
    event::DisplayProhibited, event::PlayTimeLeft, event::KeyboardOpen,
    event::KeyboardTextChange, event::KeyboardRemoteClose,
    event::PadPasscodeRequest, event::PadJoinFailed, event::PadAnnounced,
    event::PadDropped, event::PadAccountUnresolvable, event::Registered,
    event::CloudDatacenters, event::NicknameReceived, event::Holepunch,
    event::CloudDatacenterSelected, event::CloudMessage,
    event::ConnectionWaiting>;

/// @brief The stream command types.
namespace command {
/// @brief Put the console into rest mode (control message 0x50).
struct GotoBed {};
/// @brief Send the console to its home screen (control message 0x14).
struct GoHome {};
/// @brief Replace the text of the console's on-screen keyboard.
struct KeyboardSetText {
  /// @brief The new text (UTF-8), kept secret because it may be a password.
  SecretString text;

  /// @brief Copies the command, text included.
  ///
  /// @return The copy.
  auto clone() const -> KeyboardSetText { return {text.clone()}; }
};
/// @brief Close the on-screen keyboard, accepting its text.
struct KeyboardAccept {};
/// @brief Close the on-screen keyboard, rejecting its text.
struct KeyboardReject {};
/// @brief Tell the stream how many local controllers take part.
struct SetPadCount {
  /// @brief The number of controllers, 1 to 4.
  std::uint8_t pad_count{1};
};
/// @brief Remember the PSN account to sign into a couch seat.
struct CouchSetAccountId {
  /// @brief The seat, 1 to 3.
  std::uint8_t pad{};
  /// @brief The account.
  AccountId account_id;
};
/// @brief Sign the remembered account into a couch seat on a PS5 (control
/// message 0x68).
struct CouchUserJoin {
  /// @brief The seat, 1 to 3.
  std::uint8_t pad{};
};
/// @brief Attach a controller to a couch seat on a PS4 (control message
/// 0x8).
struct CouchPadJoin {
  /// @brief The seat, 1 to 3.
  std::uint8_t pad{};
  /// @brief The controller type, or 0 for the default.
  std::uint8_t controller_type{};
};
/// @brief Detach a couch seat (control message 0x9).
struct CouchLeave {
  /// @brief The seat, 1 to 3.
  std::uint8_t pad{};
};
/// @brief Answer a PS5's request for a couch user's login passcode.
struct CouchPasscode {
  /// @brief The seat, 1 to 3.
  std::uint8_t pad{};
  /// @brief The four-digit passcode.
  AccessCode passcode;

  /// @brief Copies the command, passcode included.
  ///
  /// @return The copy.
  auto clone() const -> CouchPasscode { return {pad, passcode.clone()}; }
};
/// @brief Send a seat's presence to the console as a feedback history
/// event; the same as CouchPushPresence.
struct CouchPresence {
  /// @brief The seat.
  std::uint8_t pad{};
  /// @brief Whether a controller is present.
  bool present{};
};
/// @brief Send a seat's presence to the console as a feedback history event.
struct CouchPushPresence {
  /// @brief The seat.
  std::uint8_t pad{};
  /// @brief Whether a controller is present.
  bool present{};
};
/// @brief Announce the seats' pads to the console again.
struct CouchAnnouncePads {};
/// @brief Start feeding a seat's controller into the stream, marking the
/// seat joined.
struct CouchOpenPad {
  /// @brief The seat, 1 to 3.
  std::uint8_t pad{};
  /// @brief The controller type, or 0 for the default.
  std::uint8_t controller_type{};
};
/// @brief Accept a PS5's confirmation of a seat and start feeding its
/// controller.
struct CouchAcceptPad {
  /// @brief The seat, 1 to 3.
  std::uint8_t pad{};
};
/// @brief Stop feeding a seat's controller and reset the seat.
struct CouchClosePad {
  /// @brief The seat, 1 to 3.
  std::uint8_t pad{};
};
/// @brief Join a couch seat on a PS4, which has no account sign-in step.
struct CouchJoinPs4 {
  /// @brief The seat, 1 to 3.
  std::uint8_t pad{};
  /// @brief The controller type, or 0 for the default.
  std::uint8_t controller_type{};
};
/// @brief Leave a couch seat on a PS4.
struct CouchLeavePs4 {
  /// @brief The seat, 1 to 3.
  std::uint8_t pad{};
};
/// @brief Join a couch seat, choosing the PS4 or PS5 steps.
struct CouchJoin {
  /// @brief The seat, 1 to 3.
  std::uint8_t pad{};
  /// @brief The controller type, or 0 for the default.
  std::uint8_t controller_type{};
};
/// @brief Remove a couch seat, choosing the PS4 or PS5 steps.
struct CouchRemove {
  /// @brief The seat, 1 to 3.
  std::uint8_t pad{};
};
}  // namespace command

/// @brief A command for a running stream.
using StreamCommand = std::variant<
    command::GotoBed, command::GoHome, command::KeyboardSetText,
    command::KeyboardAccept, command::KeyboardReject, command::SetPadCount,
    command::CouchSetAccountId, command::CouchUserJoin, command::CouchPadJoin,
    command::CouchLeave, command::CouchPasscode, command::CouchPresence,
    command::CouchPushPresence, command::CouchAnnouncePads,
    command::CouchOpenPad, command::CouchAcceptPad, command::CouchClosePad,
    command::CouchJoinPs4, command::CouchLeavePs4, command::CouchJoin,
    command::CouchRemove>;

}  // namespace libchiaking::net::protocol::remote_play
