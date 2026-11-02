/// @file
/// @brief Declares the vocabulary of Client: its configuration and
/// limits, the commands posted to it, the events it reports, its errors and
/// states, and the link metrics its snapshot carries.
#pragma once

#include "src/core/net/protocol/takion/detail/control_message.hpp"
#include "src/core/net/protocol/takion/detail/crypto_context.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/detail/handshake_types.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/detail/typed_message_codec.hpp"
#include "src/core/net/protocol/takion/limits.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/received_tos_metrics.hpp"
#include "src/core/net/protocol/takion/reliable_retry.hpp"
#include "src/core/net/protocol/takion/result.hpp"
#include "src/core/net/protocol/takion/source_tap.hpp"
#include "src/core/net/protocol/takion/versions.hpp"
#include "src/core/net/transport/datagram/transport.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief Datagrams a client keeps for retry after the socket refused them
/// with would-block, before it drops best-effort traffic instead.
inline constexpr std::size_t default_unsent_datagram_capacity = 64;
/// @brief How long after the socket refused a send the pump is next due to
/// retry the datagrams it kept, in microseconds.
inline constexpr std::uint64_t default_unsent_retry_interval_us = 1'000;

/// @brief Why a Takion client refused a request, dropped a packet, or failed.
enum class ClientError : std::uint8_t {
  /// @brief The configuration or limits are unusable, or a size budget leaves
  /// no
  /// room for a control message.
  invalid_configuration,
  /// @brief A request or packet came in a state that does not allow it, such as
  /// posting before start, a command needing a committed version, or a
  /// feedback packet from the console.
  invalid_state,
  /// @brief The command ring is full; the command was not queued.
  mailbox_full,
  /// @brief The random source failed while picking the verification tag.
  random_failure,
  /// @brief The socket was closed, failed to receive, or failed to close.
  transport_failure,
  /// @brief The socket took fewer bytes than the datagram; transient.
  short_send,
  /// @brief A packet could not be built, or exceeds the maximum fragment size.
  packet_encode_failure,
  /// @brief A received packet could not be parsed.
  packet_decode_failure,
  /// @brief The handshake failed, or an answer to it was refused.
  handshake_failure,
  /// @brief The reliable channel refused a message, gave up a fragment, or
  /// received data or an acknowledgement it rejects.
  reliable_channel_failure,
  /// @brief A control message could not be encoded or decoded.
  control_codec_failure,
  /// @brief Crypto is required but not installed yet.
  crypto_not_installed,
  /// @brief Protecting or opening a packet failed, or crypto could not be
  /// installed.
  crypto_failure,
  /// @brief A version is not implemented, or the version lacks a feature the
  /// request needs.
  unsupported_version,
  /// @brief Reserved; not reported today.
  negotiation_failed,
  /// @brief Reserved; not reported today.
  profile_barrier_blocked,
  /// @brief The socket failed to send; transient.
  send_dropped,
  /// @brief The socket would block and the datagram could not be kept for
  /// retry: the unsent queue was full, its deadline passed, or it may only go
  /// out at once; transient.
  send_blocked,
};

/// @brief Tells whether a send failure leaves the session usable.
///
/// @param error The failure.
/// @return Whether it is send_dropped, short_send or send_blocked.
constexpr bool is_transient_send_error(ClientError error) noexcept {
  return error == ClientError::send_dropped ||
         error == ClientError::short_send || error == ClientError::send_blocked;
}

/// @brief A result of a Takion client operation.
///
/// @tparam T The value on success.
template <class T>
using Result = Expected<T, ClientError>;

/// @brief The same as Result.
///
/// @tparam T The value on success.
template <class T>
using ClientResult = Result<T>;

/// @brief Use one Takion version from the start, with no negotiation.
struct FixedTakionProfile {
  /// @brief The version; committed as soon as the handshake completes.
  std::uint32_t version{takion_bootstrap_version};
};

/// @brief Negotiate a version: offer these with TAKIONPROTOCOLREQUEST once
/// connected.
struct NegotiatedTakionProfiles {
  /// @brief The versions offered; all must be implemented.
  std::vector<std::uint32_t> supported_versions;
};

/// @brief How the version is chosen.
using ProfileSelection =
    std::variant<FixedTakionProfile, NegotiatedTakionProfiles>;

/// @brief What a committed negotiated version changes.
enum class NegotiationWirePolicy : std::uint8_t {
  /// @brief The packet framing switches to the committed version.
  commit_selected_profile,
  /// @brief The framing stays at the bootstrap version; only the commit is
  /// reported. Senkusha uses this.
  retain_bootstrap_profile,
};

/// @brief Sizes, budgets and timers of one Takion client.
struct TakionClientLimits {
  /// @brief Commands the lock-free command ring holds; at least 1. A post to a
  /// full ring is refused with mailbox_full.
  std::size_t mailbox_capacity{default_mailbox_capacity};
  /// @brief Commands one pump executes; at least 1.
  std::size_t maximum_commands_per_pump{default_commands_per_pump};
  /// @brief Datagrams one pump reads; at least 1.
  std::size_t maximum_datagrams_per_pump{default_datagrams_per_pump};
  /// @brief Largest control message, in bytes.
  std::size_t maximum_message_size{default_maximum_message_size};
  /// @brief Largest datagram the client sends, in bytes, envelope included; at
  /// least 9.
  std::size_t maximum_fragment_size{default_fragment_size};
  /// @brief Largest control datagram, in bytes; 0 means the fragment size.
  std::size_t maximum_control_datagram_size{};
  /// @brief Most fragments per reliable message.
  std::size_t maximum_fragment_count{default_fragment_count};
  /// @brief Reliable fragments kept for retransmission.
  std::size_t retransmission_window_size{default_retransmission_window};
  /// @brief Out-of-order reliable fragments held.
  std::size_t receive_reorder_size{default_reorder_size};
  /// @brief Initial reliable retransmission timeout, in microseconds.
  std::uint64_t retransmission_timeout_us{default_retransmission_timeout_us};
  /// @brief Reliable retransmissions before a fragment is given up.
  std::uint32_t maximum_retries{default_maximum_retries};
  /// @brief Shortest reliable retransmission timeout, in microseconds.
  std::uint64_t minimum_retransmission_timeout_us{
      default_minimum_retransmission_timeout_us};
  /// @brief Longest reliable retransmission timeout, in microseconds.
  std::uint64_t maximum_retransmission_timeout_us{
      default_maximum_retransmission_timeout_us};
  /// @brief Datagrams kept for retry after the socket refused them with
  /// would-block; at least 1.
  std::size_t unsent_datagram_capacity{default_unsent_datagram_capacity};
  /// @brief How long after a refused send the pump is next due to retry, in
  /// microseconds; at least 1.
  std::uint64_t unsent_retry_interval_us{default_unsent_retry_interval_us};
};

/// @brief Tells the client which video frames the consumer would throw away,
/// so it can skip decrypting their units.
///
/// The client asks from the thread that pumps it, which must be the thread
/// that changes what the answer depends on.
class VideoUnitGate {
 public:
  /// @brief Releases the gate.
  virtual ~VideoUnitGate() = default;
  /// @brief Tells whether units of a video frame are refused as stale: the
  /// frame is older than the newest one seen, or is that one and finished.
  ///
  /// A frame called stale must stay stale for every unit already handed on.
  ///
  /// @param frame_index The unit's frame index.
  /// @return True to leave the unit's data encrypted.
  virtual auto video_frame_stale(std::uint16_t frame_index) const noexcept
      -> bool = 0;
};

/// @brief How one Takion client is set up.
struct ClientConfig {
  /// @brief The version the handshake and pre-commit packets are framed with.
  std::uint32_t bootstrap_version{takion_bootstrap_version};
  /// @brief A fixed version, or the versions to negotiate.
  ProfileSelection profile_selection{
      FixedTakionProfile{takion_bootstrap_version}};
  /// @brief What a negotiated commit changes.
  NegotiationWirePolicy negotiation_wire_policy{
      NegotiationWirePolicy::commit_selected_profile};
  /// @brief How long after the offer to wait for the console's choice, in
  /// microseconds, before settling on the bootstrap version; empty waits
  /// forever.
  std::optional<std::uint64_t> negotiation_timeout_us;
  /// @brief Framing added to every sent datagram.
  PacketEnvelope envelope{PlainEnvelope{}};
  /// @brief Framing expected on received datagrams; empty means `envelope`.
  std::optional<PacketEnvelope> receive_envelope;
  /// @brief Limits and timers.
  TakionClientLimits limits{};
  /// @brief How patiently INIT and COOKIE_ECHO are resent.
  HandshakeRetryPolicy handshake_retry{};
  /// @brief Whether packets are protected.
  CryptoPolicy crypto_policy{CryptoPolicy::disabled};
  /// @brief With crypto required: allow BIG, TAKIONPROTOCOLREQUEST and
  /// DISCONNECT, and plain control and congestion packets, before the keys are
  /// installed, deferring acknowledgements until they are.
  bool allow_plain_control_until_crypto{};
  /// @brief Reliable channel for control messages.
  std::uint16_t control_channel{control_channel_id};
  /// @brief Reliable channel for Senkusha commands.
  std::uint16_t senkusha_control_channel{senkusha_control_channel_id};
  /// @brief Reliable channel for STREAMINFOACK.
  std::uint16_t stream_info_ack_channel{stream_info_ack_channel_id};
  /// @brief Report the size of every reliable message received
  /// (ControlPayloadReceived).
  bool report_control_payload_lengths{};
  /// @brief Receives keys and sealed media for session sharing; null for none.
  /// Must outlive the client.
  SourceTap* source_tap{};
  /// @brief Picks out stale video units whose data is passed on still
  /// encrypted and marked so; null decrypts every unit. Must outlive the
  /// client.
  const VideoUnitGate* video_gate{};
  /// @brief The largest loss fraction the video frame metrics report to the
  /// server; more is capped, as the congestion report caps it. 1 reports loss
  /// as measured.
  double video_frame_loss_max{1.0};
};

/// @brief Identifies a control message whose delivery the poster wants to
/// follow.
struct ControlOperationId {
  /// @brief The id; chosen by the poster.
  std::uint64_t value{};
  /// @brief Compares the ids.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const ControlOperationId&) const noexcept = default;
};

/// @brief Identifies a probe whose sending the poster wants to follow.
struct ProbeOperationId {
  /// @brief The id; chosen by the poster.
  std::uint64_t value{};
  /// @brief Compares the ids.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const ProbeOperationId&) const noexcept = default;
};

/// @brief Command: send a control message reliably.
struct SendControl {
  /// @brief The message.
  ControlMessage message;
  /// @brief An id to follow its delivery by (ControlSent, ReliableAcked,
  /// PacketRejected).
  std::optional<ControlOperationId> operation_id{};
  /// @brief Until when, in monotonic microseconds, its datagrams may wait in
  /// the unsent queue for a socket that refused them with would-block; past
  /// it they are dropped as send_blocked and the reliable channel resends
  /// them. Empty keeps them until they are sent.
  std::optional<std::uint64_t> send_deadline_us{};
};

/// @brief The medium of a Senkusha probe.
enum class SenkushaProbeMedia : std::uint8_t {
  /// @brief An audio packet (type 0x03).
  audio,
  /// @brief A video packet (type 0x02).
  video,
};

/// @brief Command: send a Senkusha probe as one datagram.
struct SendProbe {
  /// @brief The probe's medium.
  SenkushaProbeMedia media{SenkushaProbeMedia::audio};
  /// @brief The packet after its type byte.
  std::vector<std::byte> encoded_av_payload;
  /// @brief The datagram size the probe must come to, envelope included; it is
  /// refused when the size differs or exceeds the fragment size.
  std::size_t expected_datagram_size{};
  /// @brief An id for the ProbeSent event.
  std::optional<ProbeOperationId> operation_id{};
  /// @brief Not used: a probe goes out at once or is reported blocked, and is
  /// never kept for retry, so its send time stays exact.
  std::optional<std::uint64_t> send_deadline_us{};
};

/// @brief Command: send a congestion report; allowed before a version is
/// committed.
struct SendCongestion {
  /// @brief The report.
  CongestionValues values;
};

/// @brief Command: send the controller state.
struct SendFeedbackState {
  /// @brief The state.
  FeedbackStateValues values;
  /// @brief Feedback sequence number.
  std::uint16_t sequence{};
};

/// @brief Command: send controller events.
struct SendFeedbackHistory {
  /// @brief Feedback sequence number.
  std::uint16_t sequence{};
  /// @brief The events.
  std::vector<FeedbackHistoryEvent> events;
  /// @brief Controller slot for every event, 0 to 3.
  std::uint8_t controller_id{};
  /// @brief A slot per event, or empty.
  std::vector<std::uint8_t> event_controller_ids{};
};

/// @brief Command: send a microphone audio packet built by the caller.
struct SendMicrophone {
  /// @brief The packet from its type byte, which must name the audio family;
  /// any extended header is added here.
  std::vector<std::byte> packet;
  /// @brief Offset of the payload to encrypt within `packet`; at least 1.
  std::size_t payload_offset{};
};

/// @brief Command: send a length-prefixed packet, never protected.
struct SendLengthPrefixed {
  /// @brief The payload; at most 65535 bytes.
  std::vector<std::byte> payload;
};

/// @brief Command: send a GKTRACE packet.
struct SendTrace {
  /// @brief The trace sequence number.
  std::uint32_t sequence{};
};

/// @brief Command: send a blob by generic data.
struct SendGenericData {
  /// @brief The blob; 1 to 51199 bytes.
  std::vector<std::byte> data;
  /// @brief Its metadata; under 2048 bytes.
  std::string metadata;
};

/// @brief Command: send an extended message.
struct SendExtendedMessage {
  /// @brief The protobuf body; at most 1024 bytes.
  std::vector<std::byte> message;
};

/// @brief Command: install the session's keys.
struct InstallCrypto {
  /// @brief Creates the cipher directions; must outlive the client.
  CryptoProvider* provider{};
  /// @brief Material of the direction this side sends with.
  CryptoMaterial local;
  /// @brief Material of the direction the console sends with.
  CryptoMaterial remote;
  /// @brief The first local key position.
  std::uint64_t local_key_position{};
  /// @brief The remote key position reconstruction starts from.
  std::uint64_t remote_key_position{};
};

/// @brief Command: stop the session after the commands before it.
struct Stop {};

/// @brief Any command Client::post() takes.
using TakionCommand =
    std::variant<SendControl, SendProbe, SendCongestion, SendFeedbackState,
                 SendFeedbackHistory, SendMicrophone, SendLengthPrefixed,
                 SendTrace, SendGenericData, SendExtendedMessage, InstallCrypto,
                 Stop>;

namespace client_event {
/// @brief The association is up and the reliable channel exists; no version
/// is committed yet.
struct Connected {
  /// @brief What was agreed.
  EstablishedHandshake handshake;
};
/// @brief A version is committed: at once for a fixed version, after the
/// console's choice and a quiet channel when negotiating.
struct ProfileCommitted {
  /// @brief The version.
  std::uint32_t version{};
  /// @brief Its features.
  FeatureSet features{};
};
/// @brief A control message arrived. A TAKIONPROTOCOLREQUEST has already been
/// answered and STREAMINFO's traffic-class rule applied.
struct ControlReceived {
  /// @brief The message.
  ControlMessage message;
  /// @brief The raw message, kept only when the codec treats it as
  /// unsupported; empty otherwise.
  std::vector<std::byte> unsupported_payload{};
};
/// @brief An audio unit. The client emits it for the version 7 layout with the
/// raw bytes after the type byte; the Remote Play stream layer reuses it for
/// assembled audio with the other fields set.
struct EncodedAudio {
  /// @brief The bytes.
  std::vector<std::byte> bytes;
  /// @brief Whether the type byte had bit 0x10 set.
  bool uses_nalu_info_structs{};
  /// @brief The lane tag, set by the stream layer.
  std::optional<std::uint8_t> av_tag{};
  /// @brief The audio profile, set by the stream layer.
  std::uint8_t audio_profile{};
  /// @brief Whether this is a codec header rather than audio, set by the stream
  /// layer.
  bool profile_header{};
  /// @brief Whether the stream is raw PCM, set by the stream layer.
  bool raw_pcm{};
  /// @brief Whether this is the party voice chat a PS4 host sends beside the
  /// game audio, set by the stream layer.
  bool voice{};
};
/// @brief A video unit. The client emits it for the version 7 layout with the
/// raw bytes after the type byte; the Remote Play stream layer reuses it for
/// assembled frames with the other fields set.
struct EncodedVideo {
  /// @brief The bytes.
  std::vector<std::byte> bytes;
  /// @brief Frame index, set by the stream layer.
  std::uint16_t frame_index{};
  /// @brief Codec byte, set by the stream layer.
  std::uint8_t codec{};
  /// @brief Adaptive stream index, set by the stream layer.
  std::uint8_t adaptive_stream_index{};
  /// @brief Frames lost since the previous delivered one, set by the stream
  /// layer.
  std::int32_t frames_lost{};
  /// @brief Whether the frame was pointed at an older reference picture
  /// because its own was lost, set by the stream layer.
  bool frame_recovered{};
  /// @brief Whether the type byte had bit 0x10 set.
  bool uses_nalu_info_structs{};
  /// @brief Whether this is a codec header rather than a frame, set by the
  /// stream layer.
  bool profile_header{};
  /// @brief Dynamic-range flag, set by the stream layer.
  bool dynamic_range_flag{};
  /// @brief Suppress-presentation flag, set by the stream layer.
  bool suppress_presentation{};
};
/// @brief A decoded and decrypted video or audio unit, for layouts from
/// version 9.
struct AvUnitReceived {
  /// @brief The unit.
  AvPacketValues values;
};
/// @brief Haptics audio for a pad; produced by the Remote Play stream layer,
/// not by the Takion client.
struct Haptics {
  /// @brief The haptics samples.
  std::vector<std::byte> bytes;
  /// @brief The pad.
  std::uint8_t pad_index{};
};
/// @brief One mono Opus packet for the speaker built into a pad; produced by
/// the Remote Play stream layer, not by the Takion client.
struct ControllerSpeaker {
  /// @brief The Opus packet; empty marks a lost frame for concealment.
  std::vector<std::byte> bytes;
  /// @brief The pad.
  std::uint8_t pad_index{};
};
/// @brief Rumble (reliable data type 7): selector, left and right strength.
struct Rumble {
  /// @brief Player, 0 to 3: the selector's low 2 bits.
  std::uint8_t player_index{};
  /// @brief Left motor strength.
  std::uint8_t left{};
  /// @brief Right motor strength.
  std::uint8_t right{};
  /// @brief The raw selector byte.
  std::uint8_t unknown{};
};
/// @brief Adaptive trigger effects (reliable data type 11): player, effect
/// types, two reserved bytes and ten parameters per trigger, after an 8-byte
/// prefix when the message is 33 bytes or more.
struct TriggerEffects {
  /// @brief Player: the byte, or from version 16 its low nibble.
  std::uint8_t player_index{};
  /// @brief Left trigger effect type.
  std::uint8_t type_left{};
  /// @brief Right trigger effect type.
  std::uint8_t type_right{};
  /// @brief Left trigger parameters.
  std::array<std::uint8_t, 10> left{};
  /// @brief Right trigger parameters.
  std::array<std::uint8_t, 10> right{};
};
/// @brief Pad information (reliable data type 9): lightbar colour and pad
/// settings. Messages of 4 bytes are ignored.
struct PadInfo {
  /// @brief Haptic intensity, when the message carries it.
  std::optional<std::uint8_t> haptic_intensity;
  /// @brief Trigger effect intensity, when the message carries it.
  std::optional<std::uint8_t> trigger_intensity;
  /// @brief Pad index: the byte, or from version 16 its low nibble.
  std::uint8_t index{};
  /// @brief Lightbar red, green, blue.
  std::array<std::uint8_t, 3> led{};
  /// @brief Whether the pad's user is valid.
  bool user_valid{};
  /// @brief Whether motion is to be reset; pad 0 only.
  bool motion_reset{};
  /// @brief Two motion setting bytes, for pad 0.
  std::optional<std::array<std::uint8_t, 2>> motion_settings;
};
/// @brief Reliable data type 13 from version 17: a selector and four bytes,
/// meaning not known.
struct ControllerMessage13 {
  /// @brief The selector byte.
  std::uint8_t selector{};
  /// @brief The four bytes.
  std::array<std::uint8_t, 4> payload{};
};
/// @brief A packet or reliable message the client passes on unparsed: an
/// extended message (header included) or a reliable message of an
/// unmodelled data type.
struct ProbeReceived {
  /// @brief The family it arrived as; control for reliable messages.
  PacketFamily family{};
  /// @brief The bytes.
  std::vector<std::byte> bytes;
};
/// @brief The size of one reliable message, when the config asks for it.
struct ControlPayloadReceived {
  /// @brief Bytes after the data type byte, plus 1 when the data type is not
  /// one the library models.
  std::size_t size{};
};
/// @brief A posted probe was handed to the socket, or failed transiently.
struct ProbeSent {
  /// @brief The probe's id.
  ProbeOperationId operation_id{};
  /// @brief When the last send attempt was made, in monotonic microseconds.
  std::uint64_t now_microseconds{};
  /// @brief Whether the socket took it.
  bool delivered{true};
  /// @brief Whether it failed because the socket would block.
  bool blocked{false};
};
/// @brief Every fragment of a followed control message left the socket.
struct ControlSent {
  /// @brief The message's id.
  ControlOperationId operation_id{};
  /// @brief When sending began, in monotonic microseconds.
  std::uint64_t now_microseconds{};
};
/// @brief A packet of a family the client does not interpret: congestion
/// reports from the console.
struct RawFamilyPacketReceived {
  /// @brief The family.
  PacketFamily family{};
  /// @brief The payload.
  std::vector<std::byte> bytes;
};
/// @brief The console acknowledged one of our reliable fragments.
struct ReliableAcked {
  /// @brief The fragment's sequence.
  std::uint32_t sequence{};
  /// @brief Time since its first transmission, in microseconds.
  std::uint64_t round_trip_time_us{};
  /// @brief The message's id, if it was followed.
  std::optional<ControlOperationId> operation_id{};
  /// @brief Whether this completes the followed message.
  bool operation_complete{};
};
/// @brief Something was dropped or failed without ending the session: a bad
/// datagram, a refused handshake answer, or a transient send failure.
struct PacketRejected {
  /// @brief Why.
  ClientError error{};
  /// @brief The followed message a failed send belonged to.
  std::optional<ControlOperationId> operation_id{};
  /// @brief The source line that rejected a received packet; 0 when not
  /// given.
  std::uint32_t line{};
  /// @brief Up to the first 64 bytes of what was rejected, when given.
  std::vector<std::byte> bytes{};
};
/// @brief A length-prefixed packet arrived.
struct LengthPrefixedReceived {
  /// @brief Its payload.
  std::vector<std::byte> payload;
};
/// @brief A CLIENTINFO packet arrived.
struct ClientInfoReceived {
  /// @brief Its fields.
  ClientInfoValues values;
};
/// @brief A GKTRACE packet arrived, its tag verified when crypto is required.
struct TraceReceived {
  /// @brief Its sequence number.
  std::uint32_t sequence{};
};
/// @brief A generic-data blob was queued.
struct GenericDataSubmitted {
  /// @brief Its transfer id.
  std::uint32_t transfer_id{};
};
/// @brief A generic-data blob arrived whole.
struct GenericDataReceived {
  /// @brief Its transfer id.
  std::uint32_t transfer_id{};
  /// @brief Its metadata.
  std::string metadata;
  /// @brief The blob.
  std::vector<std::byte> data;
};
/// @brief An outgoing generic-data transfer ended.
struct GenericDataTransferEnded {
  /// @brief Its transfer id.
  std::uint32_t transfer_id{};
  /// @brief How.
  GenericDataOutcome outcome{GenericDataOutcome::completed};
};
/// @brief The client stopped and closed its socket at the caller's request.
struct Closed {};
/// @brief The client failed; the socket is closed and no further events
/// follow.
struct TerminalFailure {
  /// @brief Why.
  ClientError error{};
};
}  // namespace client_event

/// @brief Any event a Client reports.
using Event = std::variant<
    client_event::Connected, client_event::ProfileCommitted,
    client_event::ControlReceived, client_event::EncodedAudio,
    client_event::EncodedVideo, client_event::AvUnitReceived,
    client_event::Haptics, client_event::Rumble, client_event::TriggerEffects,
    client_event::PadInfo, client_event::ControllerMessage13,
    client_event::ProbeReceived, client_event::ControlPayloadReceived,
    client_event::ProbeSent, client_event::ControlSent,
    client_event::RawFamilyPacketReceived, client_event::ReliableAcked,
    client_event::PacketRejected, client_event::LengthPrefixedReceived,
    client_event::ClientInfoReceived, client_event::TraceReceived,
    client_event::GenericDataSubmitted, client_event::GenericDataReceived,
    client_event::GenericDataTransferEnded, client_event::Closed,
    client_event::TerminalFailure>;

/// @brief Receives a Client's events.
///
/// Called synchronously from inside the client's owner calls (start,
/// pump_once, flush_commands and the stops), on the thread making them, while
/// that call holds the owner lock. It may call Client::post() and the
/// readers (snapshot, next_deadline_us, crypto_installed); a command it posts
/// runs on a later pump, behind those already queued. It must not make an
/// owner call, and should be quick.
class EventSink {
 public:
  /// @brief Releases the sink.
  virtual ~EventSink() = default;
  /// @brief Takes one event.
  ///
  /// @param event The event.
  virtual void on_event(Event event) = 0;
};

/// @brief Where a Takion client is.
enum class ClientState : std::uint8_t {
  /// @brief Constructed; start() not called.
  idle,
  /// @brief The handshake is running.
  handshaking,
  /// @brief The association is up.
  connected,
  /// @brief A DISCONNECT was sent and is waited on; passed through within
  /// Client::request_stop_with_disconnect().
  disconnecting,
  /// @brief Closing; passed through within one call.
  stopping,
  /// @brief Stopped at the caller's request.
  closed,
  /// @brief Failed.
  failed,
};

/// @brief The reliable channel's round-trip estimate.
struct RoundTripMetrics {
  /// @brief Smoothed round-trip time, in microseconds (millisecond resolution).
  std::uint64_t smoothed_us{};
  /// @brief Its variation, in microseconds (millisecond resolution).
  std::uint64_t variation_us{};
  /// @brief Percentiles of the latest samples, at microsecond resolution.
  RoundTripSpread spread{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const RoundTripMetrics&) const = default;
};

/// @brief Jitter and loss from extended headers, version 20 on.
struct ExtensionMetrics {
  /// @brief Jitter, in microseconds.
  std::uint64_t jitter_us{};
  /// @brief Packets counted.
  std::uint64_t received{};
  /// @brief Packets missing.
  std::uint64_t lost{};
  /// @brief Loss of the last second, a fraction.
  double loss_ratio{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const ExtensionMetrics&) const = default;
};

/// @brief Video frame metrics, version 20 on. Times are microseconds.
struct VideoMetrics {
  /// @brief Whether the version sends video frame metrics reports.
  bool reports_present{};
  /// @brief Throughput, in bits per second.
  std::uint64_t throughput_bps{};
  /// @brief Packet loss, a fraction.
  double loss_ratio{};
  /// @brief Sender interval between frames.
  std::int32_t send_interval_us{};
  /// @brief Arrival interval between frames.
  std::int32_t receive_interval_us{};
  /// @brief Arrival-to-complete time of the last timed frame.
  std::int32_t transfer_time_us{};
  /// @brief Sender time across the last timed frame.
  std::int32_t transmission_time_us{};
  /// @brief Bytes of the last timed frame.
  std::uint32_t frame_bytes{};
  /// @brief Unwrapped number of the latest frame.
  std::uint32_t frame_number{};
  /// @brief Whether the last timing was estimated.
  bool frame_estimated{};
  /// @brief Reports sent.
  std::uint32_t reports_sent{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const VideoMetrics&) const = default;
};

/// @brief An IP traffic class split into its parts.
struct TrafficClass {
  /// @brief DSCP, 0 to 63.
  std::uint8_t dscp{};
  /// @brief ECN, 0 to 3.
  std::uint8_t ecn{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const TrafficClass&) const = default;
};

/// @brief Received traffic-class counts, for versions with the feature.
struct TosMetrics {
  /// @brief The latest packet's class.
  std::optional<TrafficClass> last{};
  /// @brief Packets counted.
  std::uint64_t packets{};
  /// @brief Reports sent.
  std::uint64_t reports{};
  /// @brief Packets per DSCP value.
  std::array<std::uint64_t, received_tos_dscp_count> dscp_counts{};
  /// @brief Packets per ECN value.
  std::array<std::uint64_t, received_tos_ecn_count> ecn_counts{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const TosMetrics&) const = default;
};

/// @brief What the client measures about its link.
struct LinkMetrics {
  /// @brief The committed version's feature mask; 0 before commit.
  std::uint32_t features{};
  /// @brief Round-trip estimate, once sampled.
  std::optional<RoundTripMetrics> round_trip{};
  /// @brief Extended-header metrics, when the version has extended headers.
  std::optional<ExtensionMetrics> extension{};
  /// @brief Video metrics, when the version has extended headers.
  std::optional<VideoMetrics> video{};
  /// @brief Traffic-class counts, once counting started.
  std::optional<TosMetrics> tos{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  bool operator==(const LinkMetrics&) const = default;
};

/// @brief A view of a Takion client as the last owner call left it; the
/// queued command count is read when the snapshot is taken.
struct Snapshot {
  /// @brief Where it is.
  ClientState state{ClientState::idle};
  /// @brief The version packets are framed with now.
  std::optional<std::uint32_t> active_version;
  /// @brief Whether a version is committed.
  bool profile_committed{};
  /// @brief Whether keys are installed.
  bool crypto_installed{};
  /// @brief Commands waiting in the command ring, read when the snapshot is
  /// taken.
  std::size_t queued_commands{};
  /// @brief Reliable fragments not yet acknowledged.
  std::size_t outstanding_fragments{};
  /// @brief Whether a received control message is partly reassembled.
  bool incomplete_control_message{};
  /// @brief Whether closing the socket has begun.
  bool close_attempted{};
  /// @brief Reliable fragments sent once without tracking because the window
  /// was full.
  std::uint64_t untracked_fragments{};
  /// @brief Link metrics.
  LinkMetrics link{};
  /// @brief The receive buffer the operating system gave the socket, in
  /// bytes; nothing when the transport cannot tell.
  std::optional<std::size_t> kernel_receive_buffer;
  /// @brief Datagrams the socket refused with would-block that wait for
  /// retry.
  std::size_t unsent_datagrams{};
};

}  // namespace libchiaking::net::protocol::takion
