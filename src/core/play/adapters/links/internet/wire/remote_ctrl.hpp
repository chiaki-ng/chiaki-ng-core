/// @file
/// @brief Declares the control channel of an internet stream: the HTTP
/// startup over RUDP, the session that owns the channel's counters and
/// lifecycle, its inbox and pump, and Connector, which connects
/// one over a hole-punched route.
#pragma once

#include <libchiaking/detail/wiping_allocator.hpp>
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/protocol/remote_play/ctrl_lifecycle.hpp"
#include "src/core/net/protocol/remote_play/rudp_ctrl.hpp"
#include "src/core/net/protocol/remote_play/stream/local_ctrl.hpp"
#include "src/core/net/transport/datagram/transport.hpp"
#include "src/core/net/transport/rudp/limits.hpp"
#include "src/core/net/transport/rudp/rudp.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::play::internet::remote_ctrl {

/// @brief Which step of the control startup failed.
enum class StartupError : std::uint8_t {
  /// @brief The prepared request is empty.
  invalid_request,
  /// @brief The HTTP exchange over RUDP failed.
  exchange_failed,
  /// @brief The reply could not be finalised.
  finalize_failed,
};

/// @brief Why the control startup failed. Plain data.
struct StartupFailure {
  /// @brief The failed step.
  StartupError kind;
  /// @brief The step's own error code.
  std::uint32_t cause{};
  /// @brief The reply's HTTP status, when it could be parsed.
  std::uint16_t http_status{};
  /// @brief How many reply bytes arrived, in bytes.
  std::size_t received_bytes{};
};

/// @brief A completed control startup. Plain data.
struct StartupResult {
  /// @brief The prepared request, with its nonce and keys.
  net::protocol::remote_play::PreparedLocalCtrl prepared;
  /// @brief The console's finalised reply.
  net::protocol::remote_play::RemoteCtrlHttpFinal final;
  /// @brief The next local crypt counter; one more for a PS5.
  std::optional<std::uint64_t> local_crypt_counter{};
};

/// @brief Why sending on the control channel failed.
enum class OutboundStepError : std::uint8_t {
  /// @brief The lifecycle does not ask for this send, or the session is not
  /// active.
  unsupported_effect,
  /// @brief The effect is for another attempt.
  stale_attempt,
  /// @brief The frame could not be encoded.
  encode_failed,
  /// @brief The channel refused the send.
  transport,
  /// @brief The channel sent fewer bytes than the frame.
  short_send,
  /// @brief The PIN is empty or longer than a frame payload.
  invalid_pin,
};

/// @brief The counters of the control channel's sending side. Plain data.
struct OutboundStepState {
  /// @brief The next crypt counter.
  std::uint64_t crypt_counter{};
  /// @brief The next local RUDP counter.
  std::uint16_t local_rudp_counter{};
  /// @brief The attempt.
  std::uint64_t attempt_id{};
};

/// @brief The control lifecycle of one attempt and what it asks for: effects
/// to carry out, notices for the stream, the session id and the ready
/// ticket.
///
/// Events for another attempt are refused by the lifecycle. PIN requests are
/// dropped once the session id or ticket is out or the PIN window closed,
/// unless the prompt is open. Not thread-safe; not copyable.
class Inbox {
 public:
  /// @brief Builds the inbox.
  ///
  /// @param nonce The startup nonce, handed out with the ready ticket.
  /// @param attempt_id The attempt.
  Inbox(SecretArray<net::rudp_nonce_size> nonce,
        std::uint64_t attempt_id) noexcept;
  /// @brief Not copyable; it holds the nonce.
  Inbox(const Inbox&) = delete;
  /// @brief Not copy-assignable; it holds the nonce.
  Inbox& operator=(const Inbox&) = delete;

  /// @brief Starts the lifecycle.
  ///
  /// @return `true` when the lifecycle accepted it.
  [[nodiscard]] bool start();

  /// @brief Takes a received control message.
  ///
  /// @param step The message.
  /// @return `true` when the lifecycle accepted it; `false` when its switch
  ///     flag does not match the message.
  [[nodiscard]] bool accept(
      net::protocol::remote_play::RudpCtrlSwitchStepResult&& step);

  /// @brief Supplies the session id when the console did not send one.
  ///
  /// @param attempt_id The attempt.
  /// @param value The session id.
  /// @return `true` when it was accepted and is now held; `false` outside the
  ///     wait for the id or once an id is known.
  [[nodiscard]] bool publish_fallback(
      std::uint64_t attempt_id, net::protocol::remote_play::SessionId&& value);

  /// @brief Stops the lifecycle.
  ///
  /// @param attempt_id The attempt.
  /// @return `true` when the lifecycle accepted it.
  [[nodiscard]] bool stop(std::uint64_t attempt_id);

  /// @brief Fails the lifecycle.
  ///
  /// @param attempt_id The attempt.
  /// @param reason Why.
  /// @return `true` when the lifecycle accepted it.
  [[nodiscard]] bool fail(
      std::uint64_t attempt_id,
      net::protocol::remote_play::ctrl_lifecycle_detail::FailureReason reason);

  /// @brief Records that the channel closed.
  ///
  /// @param attempt_id The attempt.
  /// @param success Whether it closed cleanly.
  /// @return `true` when the lifecycle accepted it.
  [[nodiscard]] bool closed(std::uint64_t attempt_id, bool success);

  /// @brief Takes the ticket the stream starts with, once.
  ///
  /// @return The session id, nonce and attempt; empty before the session id
  ///     arrived or once taken.
  [[nodiscard]] auto take_ready_ticket()
      -> std::optional<net::protocol::remote_play::LocalCtrlReadyTicket>;

  /// @brief Takes the oldest effect.
  ///
  /// @return The effect; empty when none.
  [[nodiscard]] auto take_effect() -> std::optional<
      net::protocol::remote_play::ctrl_lifecycle_detail::Effect>;

  /// @brief Takes the oldest notice.
  ///
  /// @return The notice; empty when none.
  [[nodiscard]] auto take_notice()
      -> std::optional<net::protocol::remote_play::CtrlNotice>;

  /// @brief Reports whether the post-id features must be sent again.
  ///
  /// @return `true` when a repeat is pending.
  auto post_id_repeat_pending() const noexcept -> bool;

  /// @brief Takes one pending repeat of the post-id features.
  ///
  /// @return `true` when one was pending.
  [[nodiscard]] auto take_post_id_repeat() noexcept -> bool;

  /// @brief Keeps only the first queued PIN request.
  void coalesce_pin_requests();

  /// @brief Records whether the PIN prompt is showing.
  ///
  /// @param open Whether it shows.
  void set_pin_prompt_open(bool open) noexcept;

  /// @brief Closes the PIN window, dropping queued PIN requests unless the
  /// prompt shows.
  void close_pin_window();

  /// @brief Moves queued heartbeat replies ahead of the other effects,
  /// keeping their order.
  void promote_heartbeat_replies();

  /// @brief Reads the oldest effect without taking it.
  ///
  /// @return The effect; null when none; valid until the queue changes.
  auto front_effect() const noexcept
      -> const net::protocol::remote_play::ctrl_lifecycle_detail::Effect*;

  /// @brief Reads where the lifecycle stands.
  ///
  /// @return The state.
  auto state() const noexcept
      -> net::protocol::remote_play::ctrl_lifecycle_detail::State {
    return lifecycle_.state();
  }
  /// @brief Counts the events the lifecycle refused as stale.
  ///
  /// @return The count.
  std::size_t stale_events() const noexcept { return context_.stale_events; }

 private:
  /// @brief Sorts the lifecycle's new effects into the session id, notices,
  /// repeats and queued effects.
  void drain_effects();

  /// @brief The startup nonce; empty once the ticket is taken.
  std::optional<SecretArray<net::rudp_nonce_size>> nonce_;
  /// @brief The attempt.
  std::uint64_t attempt_id_{};
  /// @brief The session id; empty until it arrives or once the ticket is taken.
  std::optional<net::protocol::remote_play::SessionId> session_id_;
  /// @brief Effects to carry out.
  std::deque<net::protocol::remote_play::ctrl_lifecycle_detail::Effect>
      effects_;
  /// @brief Notices for the stream.
  std::deque<net::protocol::remote_play::CtrlNotice> notices_;
  /// @brief Pending repeats of the post-id features.
  std::size_t post_id_repeats_{};
  /// @brief What the lifecycle's actions write.
  net::protocol::remote_play::ctrl_lifecycle_detail::Context context_;
  /// @brief The control lifecycle.
  net::protocol::remote_play::ctrl_lifecycle_detail::Lifecycle lifecycle_;
  /// @brief Whether the ticket was taken.
  bool ticket_taken_{};
  /// @brief Whether the PIN prompt shows.
  bool pin_prompt_open_{};
  /// @brief Whether the PIN window closed.
  bool pin_window_closed_{};
};

/// @brief Why receiving on the control channel failed.
enum class PumpError : std::uint8_t {
  /// @brief The receive was stopped.
  stopped,
  /// @brief The receive step failed otherwise.
  step_failed,
  /// @brief The lifecycle refused a message.
  inbox_rejected,
  /// @brief The pump is for another attempt.
  stale_attempt,
  /// @brief Nothing arrived in time.
  timeout,
  /// @brief The channel failed.
  receive_failed,
  /// @brief A frame could not be read.
  malformed,
  /// @brief A frame could not be decrypted.
  crypto_failed,
  /// @brief The crypt counter ran out.
  counter_exhausted,
  /// @brief The acknowledgement could not be encoded.
  ack_encode_failed,
  /// @brief The acknowledgement could not be sent.
  ack_send_failed,
  /// @brief The acknowledgement was sent short.
  ack_short_send,
  /// @brief The console closed the channel.
  peer_closed,
};

/// @brief The counters of the control channel's receiving side. Plain data.
struct PumpState {
  /// @brief The console's next crypt counter.
  std::uint64_t remote_crypt_counter{};
  /// @brief The next local RUDP counter.
  std::uint16_t local_counter{};
  /// @brief The console's RUDP counter.
  std::uint16_t remote_counter{};
  /// @brief The attempt.
  std::uint64_t attempt_id{};
  /// @brief What the last receive saw.
  net::protocol::remote_play::RudpCtrlReceiveEvidence last_receive{};
};

/// @brief Receives control messages into an inbox, advancing a copy of the
/// receive counters. Borrows everything it is given. Not thread-safe.
class Pump {
 public:
  /// @brief Builds the pump.
  ///
  /// @param channel The RUDP channel.
  /// @param inbox Takes the control messages.
  /// @param crypto Decrypts the frames.
  /// @param target The firmware target.
  /// @param material The session's keys.
  /// @param state The receive counters to start from.
  /// @param byte_stream Carries partial frames across receives; null for a
  ///     fresh one each time.
  Pump(net::transport::rudp::HandshakeChannel& channel, Inbox& inbox,
       platform::CryptoProvider& crypto,
       net::protocol::remote_play::RegistrationTarget target,
       const net::protocol::remote_play::AuthMaterial& material,
       PumpState state,
       net::protocol::remote_play::RudpCtrlByteStream* byte_stream =
           nullptr) noexcept;

  /// @brief Reads the receive counters.
  ///
  /// @return The counters.
  auto state() const noexcept -> const PumpState& { return state_; }

  /// @brief Receives once; blocks up to @p timeout. Acknowledgements are
  /// skipped and only control messages reach the inbox.
  ///
  /// @param attempt_id The attempt.
  /// @param timeout How long to wait.
  /// @param stop Stops the wait.
  /// @return Nothing; the reason it failed.
  [[nodiscard]] auto pump_once(std::uint64_t attempt_id,
                               std::chrono::microseconds timeout,
                               const platform::StopToken& stop)
      -> Expected<void, PumpError>;

 private:
  /// @brief The RUDP channel.
  net::transport::rudp::HandshakeChannel& channel_;
  /// @brief Takes the control messages.
  Inbox& inbox_;
  /// @brief Decrypts the frames.
  platform::CryptoProvider& crypto_;
  /// @brief The firmware target.
  net::protocol::remote_play::RegistrationTarget target_;
  /// @brief The session's keys.
  const net::protocol::remote_play::AuthMaterial& material_;
  /// @brief The receive counters.
  PumpState state_;
  /// @brief Carries partial frames; may be null.
  net::protocol::remote_play::RudpCtrlByteStream* byte_stream_;
};

/// @brief Why a control session could not be created.
enum class SessionError : std::uint8_t {
  /// @brief The attempt was 0, or the lifecycle refused to start.
  invalid_attempt,
};
/// @brief Why a frame that arrived with the startup reply could not be
/// taken.
enum class InitialFrameError : std::uint8_t {
  /// @brief The frame could not be read.
  malformed,
  /// @brief The frame could not be decrypted.
  crypto_failed,
  /// @brief The crypt counter ran out.
  counter_exhausted,
  /// @brief The lifecycle refused the message.
  inbox_rejected,
};

/// @brief An internet stream's control channel: its keys, both counters,
/// its lifecycle and the post-id feature frames.
///
/// Built by Session::create. Sends block on the channel. Not
/// thread-safe; not copyable.
class Session {
 public:
  /// @brief Builds the session from a completed startup and starts its
  /// lifecycle.
  ///
  /// @param startup The startup.
  /// @param attempt_id The attempt; not 0.
  /// @return The session; SessionError::invalid_attempt.
  [[nodiscard]] static auto create(StartupResult&& startup,
                                   std::uint64_t attempt_id)
      -> Expected<std::unique_ptr<Session>, SessionError>;

  /// @brief Not copyable; it holds keys.
  Session(const Session&) = delete;
  /// @brief Not copy-assignable; it holds keys.
  Session& operator=(const Session&) = delete;

  /// @brief Reads the stream of partial frames.
  ///
  /// @return The stream.
  auto byte_stream() noexcept
      -> net::protocol::remote_play::RudpCtrlByteStream& {
    return byte_stream_;
  }
  /// @brief Reads the send buffer.
  ///
  /// @return The buffer.
  auto send_buffer() noexcept -> net::transport::rudp::SendBuffer& {
    return send_buffer_;
  }
  /// @brief Reads the inbox.
  ///
  /// @return The inbox.
  auto inbox() noexcept -> Inbox& { return inbox_; }
  /// @brief Reads the receive counters.
  ///
  /// @return The counters.
  auto pump_state() const noexcept -> const PumpState& { return pump_state_; }
  /// @brief Reads the send counters.
  ///
  /// @return The counters.
  auto outbound_state() const noexcept -> const OutboundStepState& {
    return outbound_state_;
  }
  /// @brief Reads the session's keys.
  ///
  /// @return The keys.
  auto material() const noexcept
      -> const net::protocol::remote_play::AuthMaterial& {
    return material_;
  }
  /// @brief Takes the counters after the stream switched.
  ///
  /// @param attempt_id The attempt.
  /// @param remote_crypt_counter The console's crypt counter; not below the
  ///     current one.
  /// @param next_local_counter The next local RUDP counter.
  /// @return `true` when taken.
  [[nodiscard]] auto apply_switch_counters(
      std::uint64_t attempt_id, std::uint64_t remote_crypt_counter,
      std::uint16_t next_local_counter) noexcept -> bool;
  /// @brief Takes the ticket the stream starts with.
  ///
  /// @return As Inbox::take_ready_ticket.
  [[nodiscard]] auto take_ready_ticket()
      -> std::optional<net::protocol::remote_play::LocalCtrlReadyTicket>;

  /// @brief Takes a switch step that was already acknowledged.
  ///
  /// @param result The step.
  /// @return `true` when it is for this attempt, does not move the crypt
  ///     counter back, and the inbox accepted it.
  [[nodiscard]] auto accept_acked_switch_step(
      net::protocol::remote_play::RudpCtrlSwitchStepResult result) -> bool;

  /// @brief Decrypts and takes a frame that arrived with the startup reply.
  ///
  /// @param frame The frame.
  /// @param crypto Decrypts it.
  /// @param target The firmware target.
  /// @return Nothing; the reason it was not taken.
  [[nodiscard]] auto accept_acked_initial_frame(
      std::span<const std::byte> frame, platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target)
      -> Expected<void, InitialFrameError>;

  /// @brief Receives once into the inbox; blocks up to @p timeout. The
  /// counters advance on success or a refused message.
  ///
  /// @param channel The RUDP channel.
  /// @param crypto Decrypts the frames.
  /// @param target The firmware target.
  /// @param timeout How long to wait.
  /// @param stop Stops the wait.
  /// @return Nothing; the reason it failed.
  [[nodiscard]] auto receive_once(
      net::transport::rudp::HandshakeChannel& channel,
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target,
      std::chrono::microseconds timeout, const platform::StopToken& stop)
      -> Expected<void, PumpError>;

  /// @brief Sends a heartbeat reply nothing asked for, on a timer: the console
  /// ends the session once it has heard no heartbeat reply for 30 s, and a
  /// reply only sent when asked is lost for good when its RUDP retries run
  /// out. The channel's send result is
  /// not checked.
  ///
  /// @param channel The RUDP channel.
  /// @param crypto Seals the frame.
  /// @param target The firmware target.
  /// @return The send counters; the send's error.
  [[nodiscard]] auto send_keepalive(
      net::transport::rudp::HandshakeChannel& channel,
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target)
      -> Expected<OutboundStepState, OutboundStepError>;

  /// @brief Sends the heartbeat reply the oldest effect asks for and takes the
  /// effect. The channel's send result is not checked.
  ///
  /// @param channel The RUDP channel.
  /// @param crypto Seals the frame.
  /// @param target The firmware target.
  /// @return The send counters; unsupported_effect when the oldest effect is
  ///     not a heartbeat reply; the send's error.
  [[nodiscard]] auto send_heartbeat_once(
      net::transport::rudp::HandshakeChannel& channel,
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target)
      -> Expected<OutboundStepState, OutboundStepError>;

  /// @brief Sends the PIN the oldest effect asks for and takes the effect.
  ///
  /// @param channel The RUDP channel.
  /// @param crypto Seals the frame.
  /// @param target The firmware target.
  /// @param pin The PIN.
  /// @return The send counters; unsupported_effect when the oldest effect is
  ///     not a PIN request; the send's error.
  [[nodiscard]] auto send_pin_once(
      net::transport::rudp::HandshakeChannel& channel,
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target,
      std::span<const std::byte> pin)
      -> Expected<OutboundStepState, OutboundStepError>;

  /// @brief Sends the post-id feature frames still unsent; blocks on the
  /// channel.
  ///
  /// @param channel The RUDP channel.
  /// @param crypto Seals the frames.
  /// @param target The firmware target.
  /// @param enable_dualsense Whether DualSense features are offered.
  /// @param enable_keyboard Whether keyboard features are offered.
  /// @return Nothing; unsupported_effect before the session is active; the
  ///     send's error.
  [[nodiscard]] auto send_post_id_features(
      net::transport::rudp::HandshakeChannel& channel,
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target,
      bool enable_dualsense, bool enable_keyboard = false)
      -> Expected<void, OutboundStepError>;

  /// @brief Reports whether all post-id frames were sent.
  ///
  /// @return `true` once sent.
  auto post_id_complete() const noexcept -> bool { return post_id_complete_; }

  /// @brief Sends the post-id frames again for each pending repeat.
  ///
  /// @param channel The RUDP channel.
  /// @param crypto Seals the frames.
  /// @param target The firmware target.
  /// @param enable_dualsense Whether DualSense features are offered.
  /// @param enable_keyboard Whether keyboard features are offered.
  /// @return Nothing; the send's error.
  [[nodiscard]] auto repeat_post_id_features(
      net::transport::rudp::HandshakeChannel& channel,
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target,
      bool enable_dualsense, bool enable_keyboard)
      -> Expected<void, OutboundStepError>;
  /// @brief Reports whether the console said Remote Play is prohibited.
  ///
  /// @return `true` when it did.
  auto rp_prohibited() const noexcept -> bool { return rp_prohibited_; }
  /// @brief Reads the server type the console reported.
  ///
  /// @return The type; empty when not reported.
  auto server_type() const noexcept -> std::optional<std::uint8_t> {
    return server_type_;
  }

  /// @brief Sends one control frame once the session is active.
  ///
  /// @param channel The RUDP channel.
  /// @param crypto Seals the frame.
  /// @param target The firmware target.
  /// @param type The frame type.
  /// @param payload The payload.
  /// @return Nothing; unsupported_effect before the session is active; the
  ///     encode or send error. The counters advance even when the send fails.
  [[nodiscard]] auto send_ctrl_frame(
      net::transport::rudp::HandshakeChannel& channel,
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target, std::uint16_t type,
      std::span<const std::byte> payload) -> Expected<void, OutboundStepError>;

  /// @brief Sends one post-id frame; the channel's send result is not
  /// checked.
  ///
  /// @param channel The RUDP channel.
  /// @param crypto Seals the frame.
  /// @param target The firmware target.
  /// @param type The frame type.
  /// @param payload The payload.
  /// @return Nothing; OutboundStepError::encode_failed.
  [[nodiscard]] auto send_post_id_frame(
      net::transport::rudp::HandshakeChannel& channel,
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target, std::uint16_t type,
      std::span<const std::byte> payload) -> Expected<void, OutboundStepError>;

 private:
  /// @brief Builds the session.
  ///
  /// @param nonce The startup nonce.
  /// @param material The session's keys.
  /// @param pump The receive counters.
  /// @param outbound The send counters.
  /// @param attempt_id The attempt.
  Session(SecretArray<net::rudp_nonce_size> nonce,
          net::protocol::remote_play::AuthMaterial material, PumpState pump,
          OutboundStepState outbound, std::uint64_t attempt_id);

  /// @brief Partial frames across receives.
  net::protocol::remote_play::RudpCtrlByteStream byte_stream_;
  /// @brief The send buffer.
  net::transport::rudp::SendBuffer send_buffer_;
  /// @brief The session's keys.
  net::protocol::remote_play::AuthMaterial material_;
  /// @brief The lifecycle and what it asks for.
  Inbox inbox_;
  /// @brief The receive counters.
  PumpState pump_state_;
  /// @brief The send counters.
  OutboundStepState outbound_state_;
  /// @brief The attempt.
  std::uint64_t attempt_id_{};
  /// @brief The next post-id frame to send.
  std::size_t post_id_index_{};
  /// @brief Whether all post-id frames were sent.
  bool post_id_complete_{};
  /// @brief Whether the console said Remote Play is prohibited.
  bool rp_prohibited_{};
  /// @brief The server type the console reported.
  std::optional<std::uint8_t> server_type_;
};

/// @brief Why connecting the control channel failed.
enum class ConnectError : std::uint8_t {
  /// @brief The attempt was 0.
  invalid_attempt,
  /// @brief No transport was given.
  invalid_transport,
  /// @brief The host is empty.
  invalid_host,
  /// @brief The port is 0.
  invalid_port,
  /// @brief The connect was stopped.
  stopped,
  /// @brief The startup request could not be prepared.
  prepare_failed,
  /// @brief The RUDP handshake failed.
  connect_failed,
  /// @brief The HTTP startup failed.
  startup_failed,
  /// @brief The session could not be created.
  session_failed,
  /// @brief The frames after the reply are too long, or one could not be
  /// framed.
  trailing_too_large,
  /// @brief A frame after the reply could not be read.
  trailing_malformed,
  /// @brief A frame after the reply could not be decrypted.
  trailing_crypto_failed,
  /// @brief The crypt counter ran out on a frame after the reply.
  trailing_counter_exhausted,
  /// @brief The lifecycle refused a frame after the reply.
  trailing_lifecycle_failed,
};

/// @brief Why connecting the control channel failed. Plain data.
struct ConnectFailure {
  /// @brief What failed.
  ConnectError kind;
  /// @brief The failed step's own error code.
  std::uint32_t cause{};
  /// @brief The startup's failure, for ConnectError::startup_failed.
  std::optional<StartupFailure> startup;
};

/// @brief A connected control channel. Plain data.
struct Connected {
  /// @brief The RUDP connection.
  net::transport::rudp::Connected connected;
  /// @brief The control session.
  std::unique_ptr<Session> session;
};

/// @brief Connects the control channel of an internet stream. Static
/// functions only; the connects block on the network.
class Connector {
 public:
  /// @brief Runs the HTTP startup over RUDP; blocks on the exchange.
  ///
  /// @param channel The RUDP channel.
  /// @param completion The RUDP handshake's counters.
  /// @param prepared The prepared request.
  /// @param crypto Reads the reply.
  /// @param target The firmware target.
  /// @param clock The monotonic clock.
  /// @param stop_token Stops the exchange.
  /// @return The startup; the failure.
  [[nodiscard]] static auto start_http(
      net::transport::rudp::HandshakeChannel& channel,
      const net::transport::rudp::HandshakeCompletion& completion,
      net::protocol::remote_play::PreparedLocalCtrl&& prepared,
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target,
      platform::ClockProvider& clock, platform::StopToken stop_token)
      -> Expected<StartupResult, StartupFailure>;

  /// @brief Sends a heartbeat reply; the channel's send result is not checked.
  ///
  /// @param crypto Seals the frame.
  /// @param target The firmware target.
  /// @param material The session's keys.
  /// @param channel The RUDP channel.
  /// @param state The send counters.
  /// @param effect The effect; must be a heartbeat reply for the attempt.
  /// @return The advanced counters; unsupported_effect, stale_attempt or
  ///     encode_failed.
  [[nodiscard]] static auto send_heartbeat(
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target,
      const net::protocol::remote_play::AuthMaterial& material,
      net::transport::rudp::HandshakeChannel& channel, OutboundStepState state,
      const net::protocol::remote_play::ctrl_lifecycle_detail::Effect& effect)
      -> Expected<OutboundStepState, OutboundStepError>;

  /// @brief Sends a PIN.
  ///
  /// @param crypto Seals the frame.
  /// @param target The firmware target.
  /// @param material The session's keys.
  /// @param channel The RUDP channel.
  /// @param state The send counters.
  /// @param effect The effect; must be a PIN request for the attempt.
  /// @param pin The PIN.
  /// @return The advanced counters; the reason it was not sent.
  [[nodiscard]] static auto send_pin(
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target,
      const net::protocol::remote_play::AuthMaterial& material,
      net::transport::rudp::HandshakeChannel& channel, OutboundStepState state,
      const net::protocol::remote_play::ctrl_lifecycle_detail::Effect& effect,
      std::span<const std::byte> pin)
      -> Expected<OutboundStepState, OutboundStepError>;

  /// @brief Takes the whole frames that arrived with the startup reply,
  /// leaving a trailing partial frame in @p trailing.
  ///
  /// @param owner The control session.
  /// @param trailing The bytes after the reply.
  /// @param crypto Decrypts the frames.
  /// @param target The firmware target.
  /// @return Nothing; the trailing frame error.
  [[nodiscard]] static auto drain_initial_frames(
      Session& owner, net::protocol::remote_play::WipingCtrlBytes& trailing,
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target)
      -> Expected<void, ConnectError>;

  /// @brief Connects the control channel over an existing RUDP connection,
  /// handshaking again from its counters with the given nonce; blocks on the
  /// network.
  ///
  /// @param connected The RUDP connection.
  /// @param platform The host's providers.
  /// @param crypto Prepares and reads the startup.
  /// @param console The registered console.
  /// @param host The console's host for the request.
  /// @param port The console's port; not 0.
  /// @param did The client's device id.
  /// @param codec The video codec asked for.
  /// @param nonce The startup nonce.
  /// @param stop Stops the connect.
  /// @param attempt The attempt; not 0.
  /// @return The connected channel; the failure.
  [[nodiscard]] static auto connect_registered(
      net::transport::rudp::Connected connected,
      platform::PlatformFactory& platform, platform::CryptoProvider& crypto,
      const net::protocol::remote_play::RegisteredConsole& console,
      std::string_view host, std::uint16_t port, std::span<const std::byte> did,
      net::protocol::remote_play::VideoCodec codec,
      std::span<const std::byte> nonce, platform::StopToken stop,
      std::uint64_t attempt) -> Expected<Connected, ConnectFailure>;
};

}  // namespace libchiaking::play::internet::remote_ctrl
