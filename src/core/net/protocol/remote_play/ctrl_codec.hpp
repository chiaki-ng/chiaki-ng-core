/// @file
/// @brief Declares CtrlCodec, the encoder and decoder of the Remote Play
/// control channel: the `sess/ctrl` request that opens it, the console's
/// reply, and the framed messages exchanged for the rest of the session.
///
/// The control channel is the long-lived companion of the Takion stream. It
/// starts as an HTTP request whose encrypted headers prove the session (see
/// ctrl_crypt_counter); once accepted, the same connection (TCP at home, RUDP
/// over PSN) carries framed messages: an 8-byte header of big-endian 32-bit
/// payload length, big-endian 16-bit type and two zero bytes, then a payload
/// encrypted with the session's AuthMaterial at the next counter of its
/// direction. Empty payloads are not encrypted and do not use a counter.
/// Most replies set bit 15 of the request's type (0x8004 answers 0x4); the
/// heartbeat reply is 0x1fe.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"

#include "auth_material.hpp"
#include "limits.hpp"
#include "request_head.hpp"
#include "session_id.hpp"
#include "src/core/net/protocol/remote_play/stream/controller.hpp"
#include "video_codec.hpp"

namespace libchiaking::net::protocol::remote_play {

/// @brief The control message types the library sends or understands.
enum class CtrlMessageType : std::uint16_t {
  /// @brief Console notice whether the picture may be shown; one byte, 1 when
  /// the console has started showing content that may not be streamed, 0 when
  /// it may be streamed again.
  displaya = 0x1,
  /// @brief Console request for the user's console login passcode before the
  /// stream starts; answered with login_pin_rep.
  login_pin_req = 0x4,
  /// @brief Console answer to the passcode; first byte 0 for success, 1 for an
  /// incorrect passcode.
  login = 0x5,
  /// @brief Client request to attach an extra controller to a couch seat:
  /// pad index, then controller type.
  couch_pad_join_req = 0x8,
  /// @brief Client request to detach a couch seat: one byte, the pad index.
  couch_leave_req = 0x9,
  /// @brief Console notice that it dropped a couch seat: one byte, the pad
  /// index.
  couch_pad_dropped = 0xa,
  /// @brief Client announcement that it can supply on-screen keyboard text.
  keyboard_enable = 0xd,
  /// @brief Client announcement of its DualSense, sent after the session id on
  /// PS5 with DualSense features enabled.
  dualsense_connect = 0x11,
  /// @brief Client request that the console enable DualSense features (haptics
  /// and adaptive triggers) for the session.
  enable_dualsense_features = 0x13,
  /// @brief Client request that the console go to its home screen.
  go_home = 0x14,
  /// @brief Console notice with two bytes about protected content; `01 ff`
  /// releases the restriction.
  displayb = 0x16,
  /// @brief Client toggle sent after keyboard_enable; one byte, 1.
  keyboard_enable_toggle = 0x20,
  /// @brief Console notice that a game opened its on-screen keyboard, with the
  /// current text.
  keyboard_open = 0x21,
  /// @brief Console notice that the on-screen keyboard was closed on the
  /// console side.
  keyboard_close_remote = 0x22,
  /// @brief Client message replacing the on-screen keyboard's text.
  keyboard_text_change_req = 0x23,
  /// @brief Console notice of the on-screen keyboard's new text.
  keyboard_text_change_res = 0x24,
  /// @brief Client message closing the on-screen keyboard, accepting or
  /// rejecting its text.
  keyboard_close_req = 0x25,
  /// @brief Client announcement that it has a microphone.
  mic_connect = 0x30,
  /// @brief Console message carrying the session id; see SessionId.
  session_id = 0x33,
  /// @brief Console confirmation, over PSN, that the stream moved onto its
  /// own data route.
  switch_to_stream_connection = 0x34,
  /// @brief Client microphone mute state.
  mic_toggle = 0x36,
  /// @brief Client request that the console enter rest mode.
  goto_bed = 0x50,
  /// @brief Console notice that it could not resolve the account a couch seat
  /// tried to sign in with: pad index, then printable text.
  couch_user_code = 0x61,
  /// @brief Console request for a couch user's console login passcode: one
  /// byte, the pad index.
  couch_passcode_req = 0x64,
  /// @brief Client request to sign a PSN account into a couch seat.
  couch_user_join_req = 0x68,
  /// @brief Console keep-alive request; the client must answer heartbeat_rep.
  heartbeat_req = 0xfe,
  /// @brief Client answer to heartbeat_req; empty.
  heartbeat_rep = 0x1fe,
  /// @brief Client announcement of its display devices; four zero bytes.
  display_devices = 0x910,
  /// @brief Client reply carrying the console login passcode.
  login_pin_rep = 0x8004,
  /// @brief Console reply to couch_pad_join_req: pad index, then result.
  couch_pad_join_rep = 0x8008,
  /// @brief Console reply to couch_leave_req: pad index, then result.
  couch_leave_rep = 0x8009,
  /// @brief Client reply to couch_passcode_req: pad index, then four ASCII
  /// digits.
  couch_passcode_rep = 0x8064,
  /// @brief Console reply to couch_user_join_req: pad index, then status
  /// (couch_user_join_status_passcode_required asks for a passcode).
  couch_user_join_rep = 0x8068,
};

/// @brief Why a control frame could not be read or written.
enum class CtrlFrameError : std::uint8_t {
  /// @brief Fewer bytes than the header or its declared payload.
  incomplete,
  /// @brief The declared payload exceeds ctrl_frame_max_payload inbound, or
  /// 32 bits outbound.
  too_large,
};

/// @brief One control frame located in a byte buffer, still encrypted.
struct CtrlFrameView {
  /// @brief The message type.
  std::uint16_t type{};
  /// @brief The payload, viewing the buffer that was decoded.
  std::span<const std::byte> payload;
  /// @brief The bytes the frame took, header included.
  std::size_t consumed{};
};

/// @brief The size of a control frame header, in bytes.
inline constexpr std::size_t ctrl_frame_header_size = 8;
/// @brief The largest inbound control frame, header included, in bytes.
inline constexpr std::size_t ctrl_frame_max_size = 512;
/// @brief The largest inbound control payload, in bytes (504).
inline constexpr std::size_t ctrl_frame_max_payload =
    ctrl_frame_max_size - ctrl_frame_header_size;
/// @brief The largest outbound payload the 32-bit length field can state.
inline constexpr std::size_t ctrl_frame_max_outbound_payload = 0xffffffffU;

/// @brief The controller type sent for a couch seat when the caller names
/// none.
inline constexpr std::uint8_t couch_default_controller_type = 3;
/// @brief CtrlCouchUserJoinReply::status meaning the console wants the
/// user's login passcode.
inline constexpr std::uint8_t couch_user_join_status_passcode_required = 3;
/// @brief The length of a console login passcode, in digits.
inline constexpr std::size_t couch_passcode_digits = 4;
/// @brief CtrlDisplayA::state meaning the picture may not be shown.
inline constexpr std::uint8_t ctrl_display_a_prohibited = 0x1;
/// @brief CtrlDisplayA::state meaning the picture may be shown again.
inline constexpr std::uint8_t ctrl_display_a_allowed = 0x0;
/// @brief First CtrlDisplayB byte of the release notice.
inline constexpr std::uint8_t ctrl_display_b_released_first = 0x01;
/// @brief Second CtrlDisplayB byte of the release notice.
inline constexpr std::uint8_t ctrl_display_b_released_second = 0xff;
/// @brief Bytes before the text of a keyboard_open payload.
inline constexpr std::size_t keyboard_open_header_size = 0x20;
/// @brief Bytes before the text of a keyboard_text_change_res payload.
inline constexpr std::size_t keyboard_text_response_header_size = 0x28;
/// @brief Bytes before the text of a keyboard_text_change_req payload.
inline constexpr std::size_t keyboard_text_request_header_size = 0x24;
/// @brief Offset of the big-endian 32-bit text length in a keyboard_open
/// payload.
inline constexpr std::size_t keyboard_open_length_offset = 0x1c;
/// @brief Offset of the big-endian 32-bit text length in a
/// keyboard_text_change_res payload.
inline constexpr std::size_t keyboard_text_response_length_offset = 0x08;

/// @brief The console opened its on-screen keyboard.
struct CtrlKeyboardOpen {
  /// @brief The keyboard's current text, as sent (UTF-8).
  std::string text;
};
/// @brief The console closed its on-screen keyboard.
struct CtrlKeyboardRemoteClose {};
/// @brief The console's on-screen keyboard text changed.
struct CtrlKeyboardTextChange {
  /// @brief The new text, as sent (UTF-8).
  std::string text;
};
/// @brief The console's first protected-content notice.
struct CtrlDisplayA {
  /// @brief ctrl_display_a_prohibited or ctrl_display_a_allowed; other values
  /// are ignored.
  std::uint8_t state{};
};
/// @brief The console's second protected-content notice.
struct CtrlDisplayB {
  /// @brief The first byte.
  std::uint8_t first{};
  /// @brief The second byte.
  std::uint8_t second{};
};
/// @brief The console could not resolve the account a couch seat signed in
/// with.
struct CtrlCouchAccountUnresolvable {
  /// @brief The seat's pad index.
  std::uint8_t pad{};
  /// @brief The printable ASCII characters of the rest of the payload.
  std::string printable;
};
/// @brief The console wants a couch user's login passcode.
struct CtrlCouchPasscodeRequest {
  /// @brief The seat's pad index.
  std::uint8_t pad{};
};
/// @brief The console answered a couch user join.
struct CtrlCouchUserJoinReply {
  /// @brief The seat's pad index.
  std::uint8_t pad{};
  /// @brief The console's status byte.
  std::uint8_t status{};
};
/// @brief The console answered a couch pad join.
struct CtrlCouchPadJoinReply {
  /// @brief The seat's pad index.
  std::uint8_t pad{};
  /// @brief The console's result byte.
  std::uint8_t result{};
};
/// @brief The console dropped a couch seat.
struct CtrlCouchPadDropped {
  /// @brief The seat's pad index.
  std::uint8_t pad{};
};
/// @brief The console answered a couch leave.
struct CtrlCouchLeaveReply {
  /// @brief The seat's pad index.
  std::uint8_t pad{};
  /// @brief The console's result byte.
  std::uint8_t result{};
};
/// @brief A notice whose payload was too short or whose declared text did not
/// fit.
struct CtrlMalformedNotice {
  /// @brief The message type.
  std::uint16_t type{};
  /// @brief The payload size received, in bytes.
  std::size_t size{};
};

/// @brief A console notice that does not drive the control channel's own
/// state but is passed on to the stream or the couch seats.
using CtrlNotice =
    std::variant<CtrlKeyboardOpen, CtrlKeyboardRemoteClose,
                 CtrlKeyboardTextChange, CtrlDisplayA, CtrlDisplayB,
                 CtrlCouchAccountUnresolvable, CtrlCouchPasscodeRequest,
                 CtrlCouchUserJoinReply, CtrlCouchPadJoinReply,
                 CtrlCouchPadDropped, CtrlCouchLeaveReply, CtrlMalformedNotice>;

/// @brief One control message to send, before framing and encryption.
struct CtrlOutboundFrame {
  /// @brief The message type.
  std::uint16_t type{};
  /// @brief The plaintext payload.
  std::vector<std::byte> payload;

  /// @brief Compares type and payload.
  ///
  /// @return True when both are equal.
  bool operator==(const CtrlOutboundFrame&) const = default;
};

/// @brief Why a control feature message could not be encoded.
enum class CtrlFeatureCodecError : std::uint8_t {
  /// @brief The pad is not a couch seat (1 to 3).
  invalid_pad,
  /// @brief The account id is empty or not base64 of 8 bytes.
  invalid_account_id,
  /// @brief The passcode is not four ASCII digits.
  invalid_passcode,
  /// @brief The text is too long for the frame's length field.
  too_large,
};

/// @brief A microphone control message in fixed storage, for sending from
/// the stream without allocating.
struct CtrlMicrophoneFrame {
  /// @brief The message type.
  std::uint16_t type{};
  /// @brief The payload bytes; only the first `size` are sent.
  std::array<std::byte, 4> payload{};
  /// @brief The payload length, in bytes.
  std::size_t size{};

  /// @brief The payload.
  ///
  /// @return A view of the first `size` bytes.
  auto bytes() const noexcept -> std::span<const std::byte> {
    return {payload.data(), size};
  }
};

/// @brief The last byte of a mic_toggle payload, as the Windows client sends
/// it.
inline constexpr std::uint8_t ctrl_microphone_toggle_trailer = 89;

/// @brief The console's session id arrived.
struct CtrlSessionId {
  /// @brief The id.
  SessionId value;
};
/// @brief The console's session-id message was unusable; the client will
/// synthesise one.
struct CtrlSessionIdFallback {
  /// @brief Why the message was refused.
  SessionIdError reason;
};
/// @brief The console asked for a heartbeat reply.
struct CtrlHeartbeatRequest {
  /// @brief Set when the request carried a payload, which it normally does
  /// not.
  bool unexpected_payload{false};
};
/// @brief The console asked for the login passcode.
struct CtrlLoginPinRequest {
  /// @brief Set when the request carried a payload.
  bool unexpected_payload{false};
};
/// @brief The console accepted the login passcode.
struct CtrlLoginSuccess {};
/// @brief The console refused the login passcode.
struct CtrlLoginPinIncorrect {};
/// @brief The console confirmed the switch to the stream's own route.
struct CtrlStreamSwitchAck {
  /// @brief Set when the confirmation carried a payload.
  bool unexpected_payload{false};
};
/// @brief A message the library does not act on.
struct CtrlIgnoredMessage {
  /// @brief The message type.
  std::uint16_t type{};
};
/// @brief A message that is passed on as a CtrlNotice.
struct CtrlNoticeMessage {
  /// @brief The notice.
  CtrlNotice notice;
};

/// @brief A decoded inbound control message, in the vocabulary the control
/// lifecycle acts on.
using CtrlMessage =
    std::variant<CtrlSessionId, CtrlSessionIdFallback, CtrlHeartbeatRequest,
                 CtrlLoginPinRequest, CtrlLoginSuccess, CtrlLoginPinIncorrect,
                 CtrlStreamSwitchAck, CtrlIgnoredMessage, CtrlNoticeMessage>;

/// @brief Why the control request could not be encoded.
enum class CtrlRequestError : std::uint8_t {
  /// @brief The target has no RP version.
  invalid_target,
  /// @brief The host is empty or holds characters outside visible ASCII.
  invalid_host,
  /// @brief The port is 0.
  invalid_port,
  /// @brief The registration key is not 16 bytes.
  invalid_key,
  /// @brief The device id is not 32 bytes.
  invalid_did,
  /// @brief The codec has no streaming type.
  invalid_codec,
  /// @brief The cipher failed on a header.
  crypto_failure,
  /// @brief The head or an encrypted value did not fit.
  overflow,
};

/// @brief An encoded control request head and the cipher counter that comes
/// after its encrypted headers.
///
/// Holds encrypted secrets in wiped storage; move-only. Made only by
/// CtrlCodec::encode_startup_request.
class CtrlStartupRequest {
 public:
  /// @brief The bounded head the request is written into.
  using Head = RequestHead<ctrl_request_capacity>;

  /// @brief The request as text.
  ///
  /// @return A view into this request.
  std::string_view view() const noexcept { return head_.view(); }
  /// @brief The request as bytes to send.
  ///
  /// @return A view into this request.
  std::span<const std::byte> bytes() const noexcept { return head_.bytes(); }
  /// @brief The first cipher counter after the request's encrypted headers.
  ///
  /// @return 3 for PS4 RP 8.0/9.0, 4 for PS4 RP 10.0, 5 for PS5.
  std::uint64_t next_crypt_counter() const noexcept { return next_counter_; }

 private:
  friend class CtrlCodec;

  /// @brief The request head.
  Head head_;
  /// @brief The counter after the last encrypted header.
  std::uint64_t next_counter_{};
};

/// @brief Why the console's control reply could not be read.
enum class CtrlResponseError : std::uint8_t {
  /// @brief The head has not fully arrived.
  incomplete,
  /// @brief The head is longer than rp_max_header_bytes.
  too_large,
  /// @brief The bytes are not an HTTP head, or `RP-Server-Type` is not base64.
  invalid,
};

/// @brief The console's acceptance of the control request, before decryption.
///
/// Move-only, because the encrypted server type is kept in wiped storage.
class CtrlGrant {
 public:
  /// @brief Makes an empty grant: no server type, not prohibited, no head.
  CtrlGrant() noexcept = default;
  CtrlGrant(const CtrlGrant&) = delete;
  CtrlGrant& operator=(const CtrlGrant&) = delete;
  /// @brief Takes over another grant.
  CtrlGrant(CtrlGrant&&) noexcept = default;
  /// @brief Takes over another grant, wiping what this held.
  CtrlGrant& operator=(CtrlGrant&&) noexcept = default;
  ~CtrlGrant() = default;

  /// @brief Whether `RP-Prohibit: 1` was sent: the console will not show the
  /// picture yet.
  /// @return True when prohibited.
  auto prohibited() const noexcept -> bool { return prohibited_; }
  /// @brief The 16 encrypted bytes of `RP-Server-Type`, when present and of
  /// that length. The last copy of the header counts.
  /// @return The encrypted bytes, or nothing.
  auto encrypted_server_type() const noexcept
      -> const std::optional<SecretArray<rp_server_type_size>>& {
    return encrypted_server_type_;
  }
  /// @brief How many bytes the reply head took; control frames follow it.
  /// @return The head's length.
  auto header_size() const noexcept -> std::size_t { return header_size_; }

 private:
  friend class CtrlCodec;

  /// @brief Whether the console prohibits showing the picture.
  bool prohibited_{false};
  /// @brief The encrypted server type, when usable.
  std::optional<SecretArray<rp_server_type_size>> encrypted_server_type_;
  /// @brief The head's length.
  std::size_t header_size_{};
};

/// @brief The console's refusal of the control request.
class CtrlRefusal {
 public:
  /// @brief Makes a refusal with the status the console sent.
  /// @param status The HTTP status, or 0 when out of range.
  constexpr explicit CtrlRefusal(std::uint16_t status) noexcept
      : status_(status) {}

  /// @brief The HTTP status the console sent.
  /// @return The status, or 0 when out of range.
  constexpr auto status() const noexcept -> std::uint16_t { return status_; }

 private:
  /// @brief The HTTP status.
  std::uint16_t status_;
};

/// @brief The console's answer to the control request: a grant, or the
/// refusal it sent instead.
using CtrlResponse = Expected<CtrlGrant, CtrlRefusal>;

/// @brief Why a granted server type is not known.
enum class ServerTypeError : std::uint8_t {
  /// @brief The console sent no usable `RP-Server-Type`.
  absent,
  /// @brief The console sent one, but it did not decrypt.
  undecryptable,
};

/// @brief An accepted control reply after decryption.
struct CtrlResponseFinal {
  /// @brief The console's server type, the first decrypted byte of
  /// `RP-Server-Type`: 0 PS4, 1 PS4 Pro, 2 PS5, 3 PS5 Pro. Otherwise why it is
  /// not known; neither cause fails the reply.
  Expected<std::uint8_t, ServerTypeError> server_type{
      unexpected(ServerTypeError::absent)};
  /// @brief Whether the console prohibits showing the picture.
  bool prohibited{false};
  /// @brief The next inbound cipher counter: 1 once the server type used
  /// counter 0, otherwise 0.
  std::uint64_t next_remote_counter{};

  /// @brief The server type when it is known.
  /// @return The server type, or nothing.
  auto known_server_type() const noexcept -> std::optional<std::uint8_t> {
    if (!server_type) return std::nullopt;
    return *server_type;
  }
};

/// @brief Encodes and decodes everything on the control channel.
///
/// Stateless; every member is static and may be called from any thread. The
/// caller keeps the cipher counters.
class CtrlCodec {
 public:
  /// @brief The wire value of a message type.
  ///
  /// @param type The type.
  /// @return Its 16-bit value.
  static constexpr auto message_type_id(CtrlMessageType type) noexcept
      -> std::uint16_t {
    return static_cast<std::uint16_t>(type);
  }

  /// @brief Encodes a frame header.
  ///
  /// @param type The message type.
  /// @param payload_size The payload length in bytes.
  /// @return The 8 header bytes.
  /// @retval CtrlFrameError::too_large The size does not fit 32 bits.
  [[nodiscard]] static auto encode_frame_header(
      std::uint16_t type, std::size_t payload_size) noexcept
      -> Expected<std::array<std::byte, ctrl_frame_header_size>,
                  CtrlFrameError>;

  /// @brief Locates the first frame in a buffer.
  ///
  /// @param bytes Received bytes, starting at a frame header.
  /// @return The frame, viewing `bytes`.
  /// @retval CtrlFrameError::incomplete The header or payload is not all there.
  /// @retval CtrlFrameError::too_large The payload exceeds 504 bytes.
  [[nodiscard]] static auto decode_frame(
      std::span<const std::byte> bytes) noexcept
      -> Expected<CtrlFrameView, CtrlFrameError>;

  /// @brief Decodes a decrypted payload as a notice, when its type is one.
  ///
  /// @param type The message type.
  /// @param payload The decrypted payload.
  /// @return The notice (CtrlMalformedNotice for a short payload), or nothing
  ///     when the type is not a notice.
  [[nodiscard]] static auto decode_notice(std::uint16_t type,
                                          std::span<const std::byte> payload)
      -> std::optional<CtrlNotice>;

  /// @brief The rest-mode request; empty payload.
  ///
  /// @return The message.
  static auto encode_goto_bed() -> CtrlOutboundFrame;

  /// @brief The go-home request: 16 bytes, byte 1 = 0xff.
  ///
  /// @return The message.
  static auto encode_go_home() -> CtrlOutboundFrame;

  /// @brief The keyboard announcement pair: keyboard_enable (16 bytes with
  /// bytes 3, 6 and 7 = 01, 05, ae) then keyboard_enable_toggle (`01`).
  ///
  /// @return The two messages, in sending order.
  static auto encode_keyboard_enable() -> std::array<CtrlOutboundFrame, 2>;

  /// @brief Encodes the keyboard text replacement.
  ///
  /// Layout: big-endian 32-bit counter at 0, big-endian 32-bit length at 4 and
  /// again at 0x20, the text at 0x24.
  ///
  /// @param counter The client's keyboard edit number, starting at 1.
  /// @param text The new text (UTF-8).
  /// @return The message.
  /// @retval CtrlFeatureCodecError::too_large The text is too long.
  [[nodiscard]] static auto encode_keyboard_set_text(std::uint32_t counter,
                                                     std::string_view text)
      -> Expected<CtrlOutboundFrame, CtrlFeatureCodecError>;

  /// @brief The keyboard close that accepts the text: four zero bytes.
  ///
  /// @return The message.
  static auto encode_keyboard_accept() -> CtrlOutboundFrame;

  /// @brief The keyboard close that rejects the text: `00 00 00 01`.
  ///
  /// @return The message.
  static auto encode_keyboard_reject() -> CtrlOutboundFrame;

  /// @brief Whether a couch account id is the base64 of an 8-byte account id.
  ///
  /// @param account_id The text.
  /// @return True when it decodes.
  static auto valid_couch_account_id(std::string_view account_id) -> bool;

  /// @brief Encodes a couch user join: 16 bytes of 8-byte account id, pad
  /// index, couch_default_controller_type and six zero bytes.
  ///
  /// @param pad The seat, 1 to 3.
  /// @param account_id The account id in base64.
  /// @return The message.
  /// @retval CtrlFeatureCodecError::invalid_pad The pad is not a couch seat.
  /// @retval CtrlFeatureCodecError::invalid_account_id The id is empty or
  ///     invalid.
  [[nodiscard]] static auto encode_couch_user_join(std::uint8_t pad,
                                                   std::string_view account_id)
      -> Expected<CtrlOutboundFrame, CtrlFeatureCodecError>;

  /// @brief Encodes a couch pad join: pad index, then controller type.
  ///
  /// @param pad The seat, 1 to 3.
  /// @param controller_type The type, or 0 for couch_default_controller_type.
  /// @return The message.
  /// @retval CtrlFeatureCodecError::invalid_pad The pad is not a couch seat.
  [[nodiscard]] static auto encode_couch_pad_join(std::uint8_t pad,
                                                  std::uint8_t controller_type)
      -> Expected<CtrlOutboundFrame, CtrlFeatureCodecError>;

  /// @brief Encodes a couch leave: one byte, the pad index.
  ///
  /// @param pad The seat, 1 to 3.
  /// @return The message.
  /// @retval CtrlFeatureCodecError::invalid_pad The pad is not a couch seat.
  [[nodiscard]] static auto encode_couch_leave(std::uint8_t pad)
      -> Expected<CtrlOutboundFrame, CtrlFeatureCodecError>;

  /// @brief Encodes a couch passcode reply: 8 bytes of pad index, the four
  /// ASCII digits and three zero bytes.
  ///
  /// @param pad The seat, 1 to 3.
  /// @param passcode Four ASCII digits.
  /// @return The message.
  /// @retval CtrlFeatureCodecError::invalid_pad The pad is not a couch seat.
  /// @retval CtrlFeatureCodecError::invalid_passcode The passcode is not four
  ///     digits.
  [[nodiscard]] static auto encode_couch_passcode(std::uint8_t pad,
                                                  std::string_view passcode)
      -> Expected<CtrlOutboundFrame, CtrlFeatureCodecError>;

  /// @brief The microphone announcement: mic_connect with `00 00`.
  ///
  /// @return The message.
  static constexpr auto microphone_connect_frame() noexcept
      -> CtrlMicrophoneFrame {
    return {message_type_id(CtrlMessageType::mic_connect),
            {std::byte{0}, std::byte{0}},
            2};
  }

  /// @brief The microphone mute message: mic_toggle with `00 01 <muted> 59`.
  ///
  /// @param muted Whether the microphone is muted.
  /// @return The message.
  static constexpr auto microphone_mute_frame(bool muted) noexcept
      -> CtrlMicrophoneFrame {
    return {message_type_id(CtrlMessageType::mic_toggle),
            {std::byte{0}, std::byte{1}, muted ? std::byte{1} : std::byte{0},
             std::byte{ctrl_microphone_toggle_trailer}},
            4};
  }

  /// @brief Decodes a decrypted inbound message.
  ///
  /// @param type The message type.
  /// @param payload The decrypted payload.
  /// @return The message; types the library does not know become
  ///     CtrlIgnoredMessage, and a login result other than 0 or 1 too.
  static auto decode_message(std::uint16_t type,
                             std::span<const std::byte> payload) -> CtrlMessage;

  /// @brief Encodes the control request that opens the control channel.
  ///
  /// Writes `GET <ctrl path>`, `Host`, `User-Agent: remoteplay Windows`,
  /// `Connection: keep-alive`, `Content-Length: 0`, `RP-Auth`, `RP-Version`,
  /// `RP-Did`, `RP-ControllerType`, `RP-ClientType`, `RP-OSType`,
  /// `RP-ConPath: 1`, then `RP-StartBitrate` (PS4 RP 10.0 and PS5; a zero
  /// 32-bit value), `RP-StreamingType` (PS5; little-endian 32-bit) and
  /// `RP-PadProcNo` (PS5). Each encrypted value is sealed at its
  /// ctrl_crypt_counter and sent in standard base64.
  ///
  /// @param crypto The provider that runs the cipher.
  /// @param target The console variant.
  /// @param material The session's material from the session nonce.
  /// @param host The console's host or address for `Host`.
  /// @param port The console's control port.
  /// @param registration_key The 16-byte registration key.
  /// @param did The client's 32-byte device id.
  /// @param codec The codec to ask for (used on PS5 only).
  /// @return The request.
  /// @retval CtrlRequestError::invalid_target The target has no RP version.
  /// @retval CtrlRequestError::invalid_host The host is unusable.
  /// @retval CtrlRequestError::invalid_port The port is 0.
  /// @retval CtrlRequestError::invalid_key The key is not 16 bytes.
  /// @retval CtrlRequestError::invalid_did The device id is not 32 bytes.
  /// @retval CtrlRequestError::invalid_codec The codec has no streaming type.
  /// @retval CtrlRequestError::crypto_failure The cipher failed.
  /// @retval CtrlRequestError::overflow The head did not fit.
  [[nodiscard]] static auto encode_startup_request(
      platform::CryptoProvider& crypto, RegistrationTarget target,
      const AuthMaterial& material, std::string_view host, std::uint16_t port,
      std::span<const std::byte> registration_key,
      std::span<const std::byte> did, VideoCodec codec)
      -> Expected<CtrlStartupRequest, CtrlRequestError>;

  /// @brief Reads the console's reply to the control request.
  ///
  /// @param received The bytes received so far.
  /// @return The grant, or the refusal a non-200 reply carries.
  /// @retval CtrlResponseError::too_large The head is too long.
  /// @retval CtrlResponseError::invalid Not an HTTP head, or `RP-Server-Type`
  ///     is not base64.
  /// @retval CtrlResponseError::incomplete The head has not fully arrived.
  [[nodiscard]] static auto parse_response(
      std::span<const std::byte> received) noexcept
      -> Expected<CtrlResponse, CtrlResponseError>;

  /// @brief Decrypts the server type of a grant at counter 0.
  ///
  /// A server type that fails to decrypt does not fail the reply; the result
  /// names it ServerTypeError::undecryptable.
  ///
  /// @param grant The console's grant.
  /// @param crypto The provider that runs the cipher.
  /// @param target The console variant.
  /// @param material The session's material.
  /// @return The final reply.
  static auto finalize_response(const CtrlGrant& grant,
                                platform::CryptoProvider& crypto,
                                RegistrationTarget target,
                                const AuthMaterial& material) noexcept
      -> CtrlResponseFinal;

  /// @brief The messages the client sends once the session id has arrived.
  ///
  /// With DualSense: enable_dualsense_features `00 40 00` and a 16-byte
  /// dualsense_connect. With keyboard: the encode_keyboard_enable() pair.
  /// Always: mic_toggle `00 01 01 59` twice, then display_devices with four
  /// zero bytes.
  ///
  /// @param dualsense Whether to announce DualSense features.
  /// @param keyboard Whether to announce keyboard support.
  /// @return The messages, in sending order.
  static auto post_id_frames(bool dualsense, bool keyboard)
      -> std::vector<CtrlOutboundFrame>;

  /// @brief Whether a codec can be requested.
  ///
  /// @param codec The codec.
  /// @return True when it has a streaming type.
  static auto supports(VideoCodec codec) noexcept -> bool {
    return streaming_type(codec).has_value();
  }

 private:
  /// @brief One encrypted header value in base64, in wiped storage.
  struct EncryptedField {
    /// @brief The base64 characters; only the first `size` are meaningful.
    SecretArray<encrypted_base64_capacity, char> text;
    /// @brief How many characters were written.
    std::size_t size{};

    /// @brief The value.
    ///
    /// @return A view of the characters.
    auto view() const noexcept -> std::string_view {
      return {text.data(), size};
    }
  };

  /// @brief Copies text out of a payload.
  ///
  /// @param payload The payload.
  /// @param offset Where the text starts.
  /// @param length How many bytes it has.
  /// @return The text, or nothing when it runs past the payload.
  [[nodiscard]] static auto text_from(std::span<const std::byte> payload,
                                      std::size_t offset, std::uint32_t length)
      -> std::optional<std::string>;
  /// @brief Reads a big-endian 32-bit length; the caller ensures the bytes
  /// exist.
  ///
  /// @param payload The payload.
  /// @param offset Where the length is.
  /// @return The length.
  static auto declared_length(std::span<const std::byte> payload,
                              std::size_t offset) noexcept -> std::uint32_t;
  /// @brief Reinterprets a byte as a character.
  ///
  /// @param value The byte.
  /// @return The character.
  static auto to_char(std::byte value) noexcept -> char;
  /// @brief Makes the notice for a payload that is too short.
  ///
  /// @param type The message type.
  /// @param payload The payload.
  /// @return A CtrlMalformedNotice.
  static auto malformed(std::uint16_t type, std::span<const std::byte> payload)
      -> CtrlNotice;
  /// @brief Decodes a notice whose payload is one byte.
  ///
  /// @tparam Notice A notice aggregate of one byte.
  /// @param type The message type.
  /// @param payload The payload; longer payloads are accepted.
  /// @return The notice, or a CtrlMalformedNotice when empty.
  template <class Notice>
  static auto one_byte_notice(std::uint16_t type,
                              std::span<const std::byte> payload)
      -> CtrlNotice {
    if (payload.empty()) return malformed(type, payload);
    return Notice{std::to_integer<std::uint8_t>(payload[0])};
  }
  /// @brief Decodes a notice whose payload is two bytes.
  ///
  /// @tparam Notice A notice aggregate of two bytes.
  /// @param type The message type.
  /// @param payload The payload; longer payloads are accepted.
  /// @return The notice, or a CtrlMalformedNotice when shorter.
  template <class Notice>
  static auto two_byte_notice(std::uint16_t type,
                              std::span<const std::byte> payload)
      -> CtrlNotice {
    if (payload.size() < 2) return malformed(type, payload);
    return Notice{std::to_integer<std::uint8_t>(payload[0]),
                  std::to_integer<std::uint8_t>(payload[1])};
  }
  /// @brief Whether a pad index names a couch seat (1 to 3).
  ///
  /// @param pad The index.
  /// @return True for a couch seat.
  static auto valid_ctrl_pad(std::uint8_t pad) noexcept -> bool;
  /// @brief Path, version and which optional headers a target's control
  /// request carries.
  struct TargetFacts;
  /// @brief Looks up a target's control request facts.
  ///
  /// @param target The console variant.
  /// @return The facts, or CtrlRequestError::invalid_target.
  static auto facts(RegistrationTarget target)
      -> Expected<TargetFacts, CtrlRequestError>;
  /// @brief The `RP-StreamingType` value of a codec.
  ///
  /// @param codec The codec.
  /// @return 1, 2 or 3, or nothing for a value outside the enumeration.
  static auto streaming_type(VideoCodec codec) noexcept
      -> std::optional<std::uint32_t>;
  /// @brief Encrypts one header value and encodes it in base64.
  ///
  /// @param crypto The provider that runs the cipher.
  /// @param target The console variant.
  /// @param material The session's material.
  /// @param counter The header's ctrl_crypt_counter.
  /// @param plain The value, at most 32 bytes.
  /// @return The encoded value.
  /// @retval CtrlRequestError::crypto_failure The cipher failed.
  /// @retval CtrlRequestError::overflow The base64 did not fit.
  static auto encrypted_base64(platform::CryptoProvider& crypto,
                               RegistrationTarget target,
                               const AuthMaterial& material,
                               std::uint64_t counter,
                               std::span<const std::byte> plain)
      -> Expected<EncryptedField, CtrlRequestError>;
};

}  // namespace libchiaking::net::protocol::remote_play
