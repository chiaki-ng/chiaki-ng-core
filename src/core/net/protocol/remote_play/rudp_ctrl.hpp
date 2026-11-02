/// @file
/// @brief The control channel over RUDP, for sessions reached through PSN:
/// the HTTP request/reply exchange inside RUDP session messages, and the
/// framing, encryption and acknowledgement of control messages carried in
/// RUDP frames.
///
/// RUDP frames have a 16-bit type whose subtype says where a control frame
/// starts in the payload: after a 2-byte counter (ctrl_message,
/// ack_ctrl_offset2), after 4 bytes (other types), 6 (ack_ctrl_offset6) or 8
/// (ack_ctrl_offset8). Acknowledging subtypes carry the acknowledged counter
/// at bytes 2..3. The control frame itself is the same 8-byte-header,
/// encrypted-payload frame as on TCP (see CtrlCodec).
#pragma once

#include <libchiaking/detail/wiping_allocator.hpp>
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/protocol/remote_play/auth_material.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/transport/rudp/rudp.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

/// @brief The RUDP transport namespace, shortened.
namespace rudp = net::transport::rudp;

/// @brief One RUDP frame of an inbound packet, with any control frame it
/// carries still encrypted.
struct RudpOngoingCtrl {
  /// @brief The control frame bytes, header included; empty when the frame
  /// carries none or its declared length does not match.
  std::vector<std::byte> ctrl_frame;
  /// @brief The packet's first counter plus one: the counter to acknowledge.
  std::uint16_t remote_counter{};
  /// @brief The counter the console acknowledged, for acknowledging subtypes.
  std::optional<std::uint16_t> ack_counter;
  /// @brief The frame's subtype.
  rudp::Subtype subtype{};
};

/// @brief Why an inbound RUDP control message could not be decoded.
enum class RudpCtrlMessageError : std::uint8_t {
  /// @brief The control frame is malformed or does not fill the RUDP frame.
  malformed,
  /// @brief The inbound cipher counter reached its maximum.
  counter_exhausted,
  /// @brief The cipher failed.
  crypto_failure,
};

/// @brief A decoded inbound control message with its RUDP bookkeeping.
struct RudpCtrlMessageResult {
  /// @brief The message.
  CtrlMessage message;
  /// @brief The inbound cipher counter after this message.
  std::uint64_t next_remote_crypt_counter{};
  /// @brief The RUDP counter to acknowledge.
  std::uint16_t remote_counter{};
  /// @brief The counter the console acknowledged, if any.
  std::optional<std::uint16_t> ack_counter;
  /// @brief The RUDP frame's subtype.
  rudp::Subtype subtype{};
};

/// @brief A byte buffer that wipes itself.
using WipingCtrlBytes =
    std::vector<std::byte, memory::WipingAllocator<std::byte>>;

/// @brief The outcome of decoding one control frame.
using RudpCtrlDecoded = Expected<RudpCtrlMessageResult, RudpCtrlMessageError>;

/// @brief Reassembles control frames that the console splits across RUDP
/// frames.
///
/// Holds at most ctrl_frame_max_size undecoded bytes. Not thread-safe.
class RudpCtrlByteStream {
 public:
  /// @brief Replaces the pending bytes, for bytes left over from the HTTP
  /// reply.
  ///
  /// @param bytes The bytes.
  void seed(WipingCtrlBytes bytes);
  /// @brief The bytes not yet forming a whole frame.
  ///
  /// @return A view into this stream.
  auto pending() const noexcept -> std::span<const std::byte> {
    return pending_;
  }

  /// @brief Appends bytes and splits off every whole frame.
  ///
  /// @param bytes The new bytes.
  /// @return The whole frames, in order; a trailing partial frame stays
  ///     pending.
  /// @retval CtrlFrameError::too_large More than ctrl_frame_max_size would be
  ///     pending, or a frame declares too large a payload.
  auto append(std::span<const std::byte> bytes)
      -> Expected<std::vector<WipingCtrlBytes>, CtrlFrameError>;

  /// @brief Appends a frame's control bytes and decrypts every whole frame.
  ///
  /// Stops at the first frame that fails. A framing failure clears the pending
  /// bytes.
  ///
  /// @param incoming The RUDP frame.
  /// @param crypto The provider that runs the cipher.
  /// @param target The console variant.
  /// @param material The session's material.
  /// @param crypt_counter The next inbound cipher counter.
  /// @return One result per frame; the counter in each success is the one for
  ///     the next.
  auto decode(const RudpOngoingCtrl& incoming, platform::CryptoProvider& crypto,
              RegistrationTarget target, const AuthMaterial& material,
              std::uint64_t crypt_counter) -> std::vector<RudpCtrlDecoded>;

 private:
  /// @brief Bytes waiting for the rest of their frame.
  WipingCtrlBytes pending_;
};

/// @brief The heartbeat reply's message type.
inline constexpr std::uint16_t ctrl_heartbeat_reply_type =
    CtrlCodec::message_type_id(CtrlMessageType::heartbeat_rep);
/// @brief The login passcode reply's message type.
inline constexpr std::uint16_t ctrl_login_pin_reply_type =
    CtrlCodec::message_type_id(CtrlMessageType::login_pin_rep);

/// @brief Why an outbound control message could not be encoded.
enum class RudpCtrlOutboundError : std::uint8_t {
  /// @brief The payload does not fit one RUDP frame.
  too_large,
  /// @brief The outbound cipher counter reached its maximum.
  counter_exhausted,
  /// @brief The cipher failed.
  crypto_failure,
  /// @brief The RUDP frame could not be encoded.
  encode_failure,
};

/// @brief An encoded outbound control packet and the counters after it.
struct RudpCtrlOutbound {
  /// @brief The RUDP packet to send.
  std::vector<std::byte> bytes;
  /// @brief The outbound cipher counter after it.
  std::uint64_t next_crypt_counter{};
  /// @brief The RUDP counter after it.
  std::uint16_t next_rudp_counter{};
};

/// @brief Why an RUDP request/reply exchange failed.
enum class RudpCtrlHttpExchangeError : std::uint8_t {
  /// @brief Sending failed.
  send_failed = 1,
  /// @brief The channel took fewer bytes than the packet.
  short_send,
  /// @brief Receiving failed, or (with resend_on_unexpected) a header-only
  /// packet arrived.
  receive_failed,
  /// @brief The stop token fired.
  stopped,
  /// @brief No reply arrived after all sends.
  timeout,
  /// @brief A packet could not be encoded.
  codec,
};

/// @brief The reply to an RUDP request and the counters to continue on.
struct RudpCtrlHttpExchangeOutcome {
  /// @brief The reply, reassembled from the console's frames.
  rudp::CtrlHttpResponse response{};
  /// @brief The next local RUDP counter.
  std::uint16_t next_local_counter{};
  /// @brief The console's RUDP counter.
  std::uint16_t remote_counter{};
  /// @brief How many reply bytes arrived.
  std::size_t received_bytes{};
};

/// @brief A failed RUDP exchange and how far it got.
struct RudpCtrlHttpExchangeFailure {
  /// @brief The failure.
  RudpCtrlHttpExchangeError error{};
  /// @brief Reply bytes seen in the last partial packet.
  std::size_t received_bytes{};
  /// @brief The next local RUDP counter reached.
  std::uint16_t next_local_counter{};
};

/// @brief How long to wait for each reply and whether to resend early.
struct RudpRetryPolicy {
  /// @brief How long to wait after each send, in microseconds (1.5 s by
  /// default).
  std::uint64_t timeout_us{rudp::rudp_ctrl_reply_timeout_us};
  /// @brief Whether a packet that is not the reply ends the wait at once and
  /// resends.
  bool resend_on_unexpected{false};
};

/// @brief The retry policy of the reference client: 1 s per send, resend on
/// any unexpected packet.
inline constexpr RudpRetryPolicy kReferenceRudpRetryPolicy{
    rudp::rudp_expect_timeout_us, true};

/// @brief One HTTP request sent over RUDP and its reply.
///
/// Used for the session request, the control request and PSN registration.
/// The request is sent as an RUDP session message up to three times; after
/// each, packets are read until the policy's timeout. The reply is
/// acknowledged unless the caller asked otherwise. Blocks the calling thread;
/// not thread-safe.
class RudpCtrlHttpExchange {
 public:
  /// @brief Prepares an exchange.
  ///
  /// @param next_local_counter The RUDP counter of the first send.
  /// @param remote_counter The console's counter to acknowledge.
  /// @param request The HTTP request bytes; must outlive the exchange.
  /// @param channel The RUDP channel; must outlive the exchange.
  /// @param clock The monotonic clock; must outlive the exchange.
  /// @param stop_token Cancels the exchange.
  /// @param policy The retry policy.
  /// @param acknowledge_response Whether to acknowledge the reply.
  RudpCtrlHttpExchange(
      std::uint16_t next_local_counter, std::uint16_t remote_counter,
      std::span<const std::byte> request, rudp::HandshakeChannel& channel,
      platform::ClockProvider& clock, platform::StopToken stop_token,
      RudpRetryPolicy policy = {}, bool acknowledge_response = true);

  /// @brief Sends the request and waits for the reply.
  ///
  /// Blocks for at most three times the policy's timeout plus send time.
  ///
  /// @return The reply.
  /// @retval RudpCtrlHttpExchangeError Any failure; see
  ///     RudpCtrlHttpExchangeFailure.
  [[nodiscard]] auto run()
      -> Expected<RudpCtrlHttpExchangeOutcome, RudpCtrlHttpExchangeFailure>;

 private:
  /// @brief Whether a frame type's subtype lane carries control bytes.
  ///
  /// @param frame_type The RUDP frame type.
  /// @return True for the ctrl_message and ack_ctrl_offset6 lanes.
  static auto carries_ctrl_bytes(std::uint16_t frame_type) noexcept -> bool;
  /// @brief How many reply bytes a packet that is not a complete reply holds.
  ///
  /// @param datagram The packet.
  /// @return The count, or nothing when it holds none.
  auto received_ctrl_bytes(std::span<const std::byte> datagram) const
      -> std::optional<std::size_t>;

  /// @brief The next local RUDP counter.
  std::uint16_t next_local_counter_;
  /// @brief The console's RUDP counter.
  std::uint16_t remote_counter_;
  /// @brief The request bytes.
  std::span<const std::byte> request_;
  /// @brief The RUDP channel.
  rudp::HandshakeChannel& channel_;
  /// @brief The monotonic clock.
  platform::ClockProvider& clock_;
  /// @brief Cancels the exchange.
  platform::StopToken stop_token_;
  /// @brief The retry policy.
  RudpRetryPolicy policy_;
  /// @brief Whether to acknowledge the reply.
  bool acknowledge_response_;
};

/// @brief Why an RUDP control reply could not be finalised.
enum class RemoteCtrlHttpFinalizeError : std::uint8_t {
  /// @brief The reply head was not complete.
  incomplete = 1,
  /// @brief The reply head was too long.
  too_large,
  /// @brief The reply could not be read or decrypted.
  invalid,
  /// @brief The console answered with a status other than 200.
  nonaccepted,
  /// @brief Reserved; finalize_http() does not produce it.
  prohibited,
};

/// @brief An accepted RUDP control reply, decrypted.
struct RemoteCtrlHttpFinal {
  /// @brief The decrypted reply.
  CtrlResponseFinal response{};
  /// @brief The next local RUDP counter.
  std::uint16_t next_local_counter{};
  /// @brief The console's RUDP counter.
  std::uint16_t remote_counter{};
  /// @brief Bytes after the reply head, the start of the first control frames.
  WipingCtrlBytes trailing_received;
};

/// @brief Why one receive step of the RUDP control channel failed.
enum class RudpCtrlSwitchStepError : std::uint8_t {
  /// @brief The stop token fired.
  stopped,
  /// @brief Nothing arrived in time.
  timeout,
  /// @brief Receiving failed.
  receive_failed,
  /// @brief The packet's RUDP frames could not be decoded.
  malformed,
  /// @brief Reserved; the receiver does not produce it.
  crypto_failed,
  /// @brief The inbound cipher counter reached its maximum.
  counter_exhausted,
  /// @brief An acknowledgement could not be encoded.
  ack_encode_failed,
  /// @brief Reserved; failed acknowledgement sends are counted instead.
  ack_send_failed,
  /// @brief Reserved; short acknowledgement sends are counted instead.
  ack_short_send,
  /// @brief The console sent an RUDP finish packet.
  peer_closed,
};

/// @brief What an RUDP frame delivered.
enum class RudpCtrlDelivery : std::uint8_t {
  /// @brief A control message.
  control,
  /// @brief A bare acknowledgement of the client's packets.
  acknowledgement,
  /// @brief A frame with no control message, which the client acknowledged.
  transport
};

/// @brief One delivery of a receive step.
struct RudpCtrlSwitchStepResult {
  /// @brief The message, or only its counters for other deliveries.
  RudpCtrlMessageResult message;
  /// @brief The attempt it arrived on.
  std::uint64_t attempt_id{};
  /// @brief Whether it is the console's switch confirmation (0x34).
  bool switch_notification{};
  /// @brief What the frame delivered.
  RudpCtrlDelivery delivery{RudpCtrlDelivery::control};
};

/// @brief What the last receive step saw, for diagnostics.
struct RudpCtrlReceiveEvidence {
  /// @brief The packet's size.
  std::size_t packet_bytes{};
  /// @brief The packet's first frame type.
  std::uint16_t first_type{};
  /// @brief How many RUDP frames it held.
  std::size_t frame_count{};
  /// @brief How many of them carried no control message.
  std::size_t transport_only_frames{};
  /// @brief How many acknowledgements failed to send.
  std::size_t ack_send_failures{};
  /// @brief How many control frames failed to decode.
  std::size_t dropped_frames{};
  /// @brief The framing error, when the packet could not be decoded.
  std::optional<rudp::FrameError> framing_error;
  /// @brief The last control frame error.
  std::optional<RudpCtrlMessageError> frame_error;
  /// @brief Each frame's RUDP subtype and the console's counter it carried,
  /// in packet order, for logging.
  std::vector<std::pair<rudp::Subtype, std::uint16_t>> frames_seen;
};

/// @brief What a receive step needs from the control session. References
/// must outlive the step.
struct RudpCtrlSwitchStepInput {
  /// @brief The provider that runs the cipher.
  platform::CryptoProvider& crypto;
  /// @brief The console variant.
  RegistrationTarget target;
  /// @brief The session's material.
  const AuthMaterial& material;
  /// @brief The next inbound cipher counter.
  std::uint64_t remote_crypt_counter;
  /// @brief The next local RUDP counter, used in acknowledgements.
  std::uint16_t local_counter;
  /// @brief The current attempt.
  std::uint64_t attempt_id;
  /// @brief The reassembly buffer of the session.
  RudpCtrlByteStream& byte_stream;
};

/// @brief Receives one RUDP packet of the control channel, acknowledges it
/// and decodes its control messages.
///
/// Not thread-safe.
class RudpCtrlReceiver {
 public:
  /// @brief Waits for one packet and decodes it.
  ///
  /// Blocks for at most `timeout`. Every frame that carries data is
  /// acknowledged; a frame whose messages all fail still yields a transport
  /// delivery.
  ///
  /// @param channel The RUDP channel.
  /// @param input The session's state.
  /// @param timeout How long to wait.
  /// @param stop Cancels the wait.
  /// @return The deliveries, in order.
  /// @retval RudpCtrlSwitchStepError::stopped The stop token fired.
  /// @retval RudpCtrlSwitchStepError::timeout Nothing arrived.
  /// @retval RudpCtrlSwitchStepError::receive_failed Receiving failed.
  /// @retval RudpCtrlSwitchStepError::peer_closed A finish packet arrived.
  /// @retval RudpCtrlSwitchStepError::malformed The packet could not be
  ///     decoded.
  /// @retval RudpCtrlSwitchStepError::counter_exhausted The cipher counter is
  ///     spent.
  /// @retval RudpCtrlSwitchStepError::ack_encode_failed An acknowledgement
  ///     could not be encoded.
  [[nodiscard]] auto step(rudp::HandshakeChannel& channel,
                          RudpCtrlSwitchStepInput input,
                          std::chrono::microseconds timeout,
                          const platform::StopToken& stop)
      -> Expected<std::vector<RudpCtrlSwitchStepResult>,
                  RudpCtrlSwitchStepError>;

  /// @brief What the last step saw.
  ///
  /// @return The evidence.
  auto evidence() const noexcept -> const RudpCtrlReceiveEvidence& {
    return evidence_;
  }

 private:
  /// @brief Where the first frame type sits in a packet.
  static constexpr std::size_t first_frame_type_offset =
      rudp::FrameCodec::header_bytes - sizeof(std::uint16_t);

  /// @brief Notes a packet's size and first frame type.
  ///
  /// @param packet The packet.
  void record_packet(std::span<const std::byte> packet) noexcept;
  /// @brief Sends an acknowledgement, counting a failed send.
  ///
  /// @param channel The RUDP channel.
  /// @param local_counter The local RUDP counter.
  /// @param remote_counter The counter acknowledged.
  /// @return Nothing, or RudpCtrlSwitchStepError::ack_encode_failed.
  [[nodiscard]] auto acknowledge(rudp::HandshakeChannel& channel,
                                 std::uint16_t local_counter,
                                 std::uint16_t remote_counter)
      -> Expected<void, RudpCtrlSwitchStepError>;
  /// @brief Makes the delivery of a frame without a control message.
  ///
  /// @param frame The frame.
  /// @param input The session's state.
  /// @param delivery What it delivered.
  /// @return The delivery.
  static auto transport_result(const RudpOngoingCtrl& frame,
                               const RudpCtrlSwitchStepInput& input,
                               RudpCtrlDelivery delivery)
      -> RudpCtrlSwitchStepResult;

  /// @brief What the last step saw.
  RudpCtrlReceiveEvidence evidence_{};
};

/// @brief Encodes and decodes control messages in RUDP frames.
///
/// Stateless; every member is static.
class RudpCtrl {
 public:
  /// @brief Splits a packet into RUDP frames and locates their control frames.
  ///
  /// @param packet The packet.
  /// @return One entry per frame.
  /// @retval rudp::FrameError::malformed A frame is too short.
  /// @retval rudp::FrameError::too_large A control frame declares too large a
  ///     payload.
  /// @retval rudp::FrameError Any framing error of the packet.
  [[nodiscard]] static auto decode_ongoing(std::span<const std::byte> packet)
      -> Expected<std::vector<RudpOngoingCtrl>, rudp::FrameError>;

  /// @brief Decrypts and decodes one control frame.
  ///
  /// @param crypto The provider that runs the cipher.
  /// @param target The console variant.
  /// @param material The session's material.
  /// @param remote_crypt_counter The next inbound cipher counter.
  /// @param incoming The frame.
  /// @return The message.
  /// @retval RudpCtrlMessageError::malformed The frame does not fill its
  ///     bytes.
  /// @retval RudpCtrlMessageError::counter_exhausted The counter is spent.
  /// @retval RudpCtrlMessageError::crypto_failure The cipher failed.
  [[nodiscard]] static auto decode_message(platform::CryptoProvider& crypto,
                                           RegistrationTarget target,
                                           const AuthMaterial& material,
                                           std::uint64_t remote_crypt_counter,
                                           const RudpOngoingCtrl& incoming)
      -> Expected<RudpCtrlMessageResult, RudpCtrlMessageError>;

  /// @brief Encodes a control message as an RUDP ctrl_message packet.
  ///
  /// The payload is the big-endian RUDP counter, the control frame header and
  /// the encrypted payload. The passcode reply is encrypted at the low 16 bits
  /// of the counter minus one, as the reference client does; every other
  /// message at the counter itself.
  ///
  /// @param crypto The provider that runs the cipher.
  /// @param target The console variant.
  /// @param material The session's material.
  /// @param type The message type.
  /// @param plaintext The payload; an empty one is not encrypted.
  /// @param crypt_counter The next outbound cipher counter.
  /// @param rudp_counter The RUDP counter of the packet.
  /// @return The packet and the counters after it.
  /// @retval RudpCtrlOutboundError::too_large The payload does not fit.
  /// @retval RudpCtrlOutboundError::counter_exhausted The counter is spent.
  /// @retval RudpCtrlOutboundError::crypto_failure The cipher failed.
  /// @retval RudpCtrlOutboundError::encode_failure The frame could not be
  ///     encoded.
  [[nodiscard]] static auto encode_outbound(
      platform::CryptoProvider& crypto, RegistrationTarget target,
      const AuthMaterial& material, std::uint16_t type,
      std::span<const std::byte> plaintext, std::uint64_t crypt_counter,
      std::uint16_t rudp_counter)
      -> Expected<RudpCtrlOutbound, RudpCtrlOutboundError>;

  /// @brief Reads and decrypts the control reply of an RUDP exchange.
  ///
  /// @param outcome The exchange's reply.
  /// @param crypto The provider that runs the cipher.
  /// @param target The console variant.
  /// @param material The session's material.
  /// @return The reply, its counters and the bytes after its head.
  /// @retval RemoteCtrlHttpFinalizeError::incomplete The head is incomplete.
  /// @retval RemoteCtrlHttpFinalizeError::too_large The head is too long.
  /// @retval RemoteCtrlHttpFinalizeError::nonaccepted The status is not 200.
  /// @retval RemoteCtrlHttpFinalizeError::invalid Any other failure.
  [[nodiscard]] static auto finalize_http(
      const RudpCtrlHttpExchangeOutcome& outcome,
      platform::CryptoProvider& crypto, RegistrationTarget target,
      const AuthMaterial& material)
      -> Expected<RemoteCtrlHttpFinal, RemoteCtrlHttpFinalizeError>;

 private:
  /// @brief The cipher counter of a passcode reply.
  ///
  /// @param crypt_counter The next outbound cipher counter.
  /// @return Its low 16 bits minus one, wrapped to 64 bits.
  static auto login_pin_cipher_counter(std::uint64_t crypt_counter) noexcept
      -> std::uint64_t;
};

}  // namespace libchiaking::net::protocol::remote_play
