/// @file
/// @brief The home control channel as a start sees it: preparing and sending
/// the `sess/ctrl` request, handing its socket to a CtrlOwner, waiting for the
/// session id, and the queues of post-id and runtime control messages.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/wire_sizes.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/protocol/remote_play/auth_material.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/remote_play/ctrl_exchange.hpp"
#include "src/core/net/protocol/remote_play/ctrl_owner.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"
#include "src/core/net/transport/datagram/transport.hpp"

#include <boost/sml.hpp>

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

namespace libchiaking::net::protocol::remote_play {

/// @brief Why the control socket could not be handed to an owner.
enum class LocalCtrlHandoffError : std::uint8_t {
  /// @brief The frame reader refused the connection or its reply.
  reader_failure,
  /// @brief The owner refused to start the attempt.
  owner_start_failure,
};

/// @brief A running control channel and the console's decrypted reply.
///
/// Move-only through the owner.
struct LocalCtrlHandoff {
  /// @brief The channel's owner, already started.
  std::unique_ptr<CtrlOwner> owner;
  /// @brief The reply: server type, prohibit flag, first inbound counter.
  CtrlResponseFinal response;

  /// @brief Takes over an accepted control request's socket and starts an
  /// attempt on it.
  ///
  /// @param crypto The cipher provider; must outlive the owner.
  /// @param connection The startup exchange's connection; taken over.
  /// @param target The console variant.
  /// @param material The session's material; taken over.
  /// @param attempt_id The attempt, nonzero.
  /// @return The running channel.
  /// @retval LocalCtrlHandoffError::reader_failure The reply could not be
  ///     finalised.
  /// @retval LocalCtrlHandoffError::owner_start_failure The owner refused the
  ///     attempt.
  [[nodiscard]] static auto adopt(platform::CryptoProvider& crypto,
                                  CtrlStartupConnection&& connection,
                                  RegistrationTarget target,
                                  AuthMaterial&& material,
                                  std::uint64_t attempt_id)
      -> Expected<LocalCtrlHandoff, LocalCtrlHandoffError>;
};

/// @brief Why a control request could not be prepared.
enum class LocalCtrlPrepareError : std::uint8_t {
  /// @brief The console's variant has no RP version.
  invalid_target,
  /// @brief The host is unusable in a `Host` header.
  invalid_host,
  /// @brief The port is 0.
  invalid_port,
  /// @brief The device id is not 32 bytes.
  invalid_did,
  /// @brief The nonce is not 16 bytes.
  invalid_nonce,
  /// @brief The codec cannot be requested.
  invalid_codec,
  /// @brief The session material could not be derived.
  auth_material_failure,
  /// @brief The request could not be encoded.
  request_failure,
};

/// @brief A control request ready to send, with the secrets it was made
/// from.
///
/// Move-only; everything is wiped on destruction.
struct PreparedLocalCtrl {
  /// @brief The session nonce.
  SecretArray<rp_nonce_size> nonce;
  /// @brief The material derived from it.
  AuthMaterial material;
  /// @brief The encoded request.
  CtrlStartupRequest request;
};

/// @brief Prepares control requests.
///
/// Stateless; every member is static.
class LocalCtrlPreparation {
 public:
  /// @brief Prepares a request from the session reply's nonce.
  ///
  /// @param crypto The cipher provider.
  /// @param console The registered console.
  /// @param host The host for `Host`.
  /// @param port The control port.
  /// @param did The 32-byte device id.
  /// @param codec The codec to ask for.
  /// @param nonce The 16-byte nonce.
  /// @return The prepared request.
  /// @retval LocalCtrlPrepareError Why it could not be prepared.
  [[nodiscard]] static auto prepare_from_nonce(
      platform::CryptoProvider& crypto, const RegisteredConsole& console,
      std::string_view host, std::uint16_t port, std::span<const std::byte> did,
      VideoCodec codec, std::span<const std::byte> nonce)
      -> Expected<PreparedLocalCtrl, LocalCtrlPrepareError>;

 private:
  /// @brief Checks the inputs of a request.
  ///
  /// @param console The registered console.
  /// @param host The host.
  /// @param port The port.
  /// @param did The device id.
  /// @param codec The codec.
  /// @return Nothing, or the first problem found.
  [[nodiscard]] static auto validate(const RegisteredConsole& console,
                                     std::string_view host, std::uint16_t port,
                                     std::span<const std::byte> did,
                                     VideoCodec codec)
      -> Expected<void, LocalCtrlPrepareError>;
  /// @brief Derives the material and encodes the request.
  ///
  /// @param crypto The cipher provider.
  /// @param console The registered console.
  /// @param host The host.
  /// @param port The port.
  /// @param did The device id.
  /// @param codec The codec.
  /// @param nonce The 16-byte nonce.
  /// @return The prepared request.
  /// @retval LocalCtrlPrepareError::auth_material_failure Derivation failed.
  /// @retval LocalCtrlPrepareError::request_failure Encoding failed.
  [[nodiscard]] static auto build_from_nonce(
      platform::CryptoProvider& crypto, const RegisteredConsole& console,
      std::string_view host, std::uint16_t port, std::span<const std::byte> did,
      VideoCodec codec, std::span<const std::byte> nonce)
      -> Expected<PreparedLocalCtrl, LocalCtrlPrepareError>;
};

/// @brief Why a control attempt failed, other than its TCP exchange.
enum class LocalCtrlFault : std::uint8_t {
  /// @brief The call does not fit the attempt's phase.
  invalid_state,
  /// @brief The request could not be prepared.
  prepare_failed,
  /// @brief The socket could not be handed to an owner.
  handoff_failure,
  /// @brief Closing the channel failed.
  cleanup_failed,
};

/// @brief Why a control attempt failed: its own fault or its TCP exchange's.
using LocalCtrlError = std::variant<LocalCtrlFault, ConsoleExchangeError>;

/// @brief An accepted control request: the running channel and the nonce.
struct LocalCtrlStartupResult {
  /// @brief The running channel.
  LocalCtrlHandoff handoff;
  /// @brief The session nonce, for the stream's launch.
  SecretArray<rp_nonce_size> nonce;
};

}  // namespace libchiaking::net::protocol::remote_play

namespace libchiaking::net::protocol::remote_play::local_ctrl_lifecycle {
namespace sml = boost::sml;

/// @brief Phase of a control attempt.
enum class State : std::uint8_t { idle, exchanging, ready, finished };
/// @var libchiaking::net::protocol::remote_play::local_ctrl_lifecycle::State::idle
/// @brief Not started.
/// @var libchiaking::net::protocol::remote_play::local_ctrl_lifecycle::State::exchanging
/// @brief Sending the request and reading the reply.
/// @var libchiaking::net::protocol::remote_play::local_ctrl_lifecycle::State::ready
/// @brief The channel runs.
/// @var libchiaking::net::protocol::remote_play::local_ctrl_lifecycle::State::finished
/// @brief Failed, cancelled or taken.

/// @brief State tag: not started.
struct Idle {};
/// @brief State tag: exchanging.
struct Exchanging {};
/// @brief State tag: ready.
struct Ready {};
/// @brief State tag: finished.
struct Finished {};

/// @brief Event: the request was sent.
struct Start {};
/// @brief Event: the socket was handed to an owner.
struct HandedOff {};
/// @brief Event: the attempt failed.
struct Failed {};
/// @brief Event: the channel was taken.
struct Taken {};
/// @brief Event: the owner cancelled.
struct Cancel {};

/// @brief Effect: cancel the TCP exchange.
struct AbandonExchange {};
/// @brief Effect: stop and close the running channel.
struct CloseReady {};

/// @brief What the attempt must do after a cancel.
using Effect = std::variant<AbandonExchange, CloseReady>;

/// @brief The machine's data.
struct Context {
  /// @brief The last event's effect.
  std::optional<Effect> effect;
  /// @brief Events the machine refused.
  std::size_t unexpected_events{};
};

/// @brief The transition table.
struct Fsm {
  /// @brief Builds the table.
  ///
  /// @return The boost::sml table.
  auto operator()() const noexcept {
    using namespace sml;
    const auto abandon_exchange = [](Context& c) {
      c.effect = AbandonExchange{};
    };
    const auto close_ready = [](Context& c) { c.effect = CloseReady{}; };

    return make_transition_table(
        *state<Idle> + sml::event<Start> = state<Exchanging>,
        state<Idle> + sml::event<Cancel> = state<Finished>,
        state<Exchanging> + sml::event<HandedOff> = state<Ready>,
        state<Exchanging> + sml::event<Failed> = state<Finished>,
        state<Exchanging> + sml::event<Cancel> / abandon_exchange =
            state<Finished>,
        state<Ready> + sml::event<Taken> = state<Finished>,
        state<Ready> + sml::event<Cancel> / close_ready = state<Finished>,
        state<Finished> + sml::event<Cancel> = state<Finished>);
  }
};

/// @brief The attempt's state machine with its context.
///
/// Not thread-safe.
class Lifecycle {
 public:
  /// @brief Makes an idle machine.
  Lifecycle() : machine_(context_) {}
  Lifecycle(const Lifecycle&) = delete;
  Lifecycle& operator=(const Lifecycle&) = delete;

  /// @brief Feeds an event, clearing the previous effect.
  ///
  /// @tparam Event One of the event types above.
  /// @param event The event.
  /// @return True when taken; false (and counted) otherwise.
  template <class Event>
  [[nodiscard]] bool process(const Event& event) {
    context_.effect.reset();
    if (machine_.process_event(event)) return true;
    ++context_.unexpected_events;
    return false;
  }

  /// @brief Takes the last event's effect.
  ///
  /// @return The effect, or nothing.
  auto take_effect() noexcept -> std::optional<Effect> {
    return std::exchange(context_.effect, std::nullopt);
  }

  /// @brief The phase.
  ///
  /// @return The state.
  State state() const noexcept {
    if (machine_.is(sml::state<Exchanging>)) return State::exchanging;
    if (machine_.is(sml::state<Ready>)) return State::ready;
    if (machine_.is(sml::state<Finished>)) return State::finished;
    return State::idle;
  }

 private:
  /// @brief The context.
  Context context_;
  /// @brief The machine.
  sml::sm<Fsm> machine_;
};

}  // namespace libchiaking::net::protocol::remote_play::local_ctrl_lifecycle

namespace libchiaking::net::protocol::remote_play {

/// @brief What a start needs from the control channel to launch the stream:
/// the session id and the nonce.
///
/// Move-only; secrets are wiped.
struct LocalCtrlReadyTicket {
  /// @brief The session id, received or synthesised.
  SessionId session_id;
  /// @brief The session nonce.
  SecretArray<rp_nonce_size> nonce;
  /// @brief The control attempt it belongs to.
  std::uint64_t attempt_id{};
};

/// @brief Why a control session step failed.
enum class LocalCtrlSessionIdError : std::uint8_t {
  /// @brief The call does not fit the session's state.
  invalid_state,
  /// @brief The control channel failed.
  reader_failure,
  /// @brief The passcode exceeds a control payload.
  pin_too_large,
};

/// @brief One step of carrying out control effects.
struct LocalCtrlEffectStep;

/// @brief A running home control channel up to and after the session id.
///
/// Wraps the CtrlOwner: drains its effects, keeps the first session id and
/// the console's notices, and carries out heartbeat, passcode and cleanup
/// effects one at a time, an outbound send at a time. Passcode requests that
/// arrive after the session id are dropped unless a passcode prompt is open.
/// Not thread-safe.
class LocalCtrlSessionId {
 public:
  /// @brief Carries out the next control effect.
  ///
  /// A pending heartbeat or passcode send is continued first. Effects of
  /// another attempt are dropped as stale.
  ///
  /// @param ctrl The session.
  /// @param attempt_id The current attempt.
  /// @return What happened: nothing to do, a send advanced, a passcode is
  ///     needed, a notification to act on, or a stale effect dropped.
  /// @retval LocalCtrlSessionIdError::reader_failure The channel failed.
  /// @retval LocalCtrlSessionIdError::invalid_state An effect could not run.
  [[nodiscard]] static auto step_effect(LocalCtrlSessionId& ctrl,
                                        std::uint64_t attempt_id)
      -> Expected<LocalCtrlEffectStep, LocalCtrlSessionIdError>;

  /// @brief Takes over a running channel.
  ///
  /// @param result The accepted request.
  /// @param attempt_id The attempt.
  LocalCtrlSessionId(LocalCtrlStartupResult&& result,
                     std::uint64_t attempt_id) noexcept;
  LocalCtrlSessionId(const LocalCtrlSessionId&) = delete;
  LocalCtrlSessionId& operator=(const LocalCtrlSessionId&) = delete;

  /// @brief Reads at most one control frame.
  ///
  /// Never blocks.
  ///
  /// @return True when a frame was handled.
  /// @retval CtrlReaderError::invalid_state There is no owner, the channel is
  ///     not waiting for the id or active, or the lifecycle refused the
  ///     message.
  /// @retval CtrlReaderError Why the channel failed.
  [[nodiscard]] auto pump_once() -> Expected<bool, CtrlReaderError>;
  /// @brief Takes the session id once it arrived.
  ///
  /// @return The id, or nothing.
  [[nodiscard]] auto take_session_id() -> std::optional<SessionId>;
  /// @brief Takes the nonce.
  ///
  /// @return The nonce, or nothing once taken.
  [[nodiscard]] auto take_nonce() -> std::optional<SecretArray<rp_nonce_size>>;
  /// @brief Takes the session id and nonce together once the id arrived.
  ///
  /// @return The ticket, or nothing.
  [[nodiscard]] auto take_ready_ticket() -> std::optional<LocalCtrlReadyTicket>;
  /// @brief Feeds a synthesised session id in place of the console's.
  ///
  /// @param fallback The synthesised id.
  /// @return True when it was taken as the session id.
  [[nodiscard]] auto publish_fallback(SessionId&& fallback) -> bool;
  /// @brief Takes the next effect, unless a send is pending.
  ///
  /// @return The effect, or nothing.
  [[nodiscard]] auto take_effect()
      -> std::optional<ctrl_lifecycle_detail::Effect>;
  /// @brief Takes the next console notice.
  ///
  /// @return The notice, or nothing.
  [[nodiscard]] auto take_notice() -> std::optional<CtrlNotice>;
  /// @brief Whether the post-id announcements must be sent again.
  ///
  /// @return True when a repeat is owed.
  auto post_id_repeat_pending() const noexcept -> bool {
    return post_id_repeats_ != 0;
  }
  /// @brief Takes one owed repeat of the post-id announcements.
  ///
  /// @return True when one was owed.
  [[nodiscard]] auto take_post_id_repeat() noexcept -> bool;
  /// @brief The next effect, left in place.
  ///
  /// @return The effect, or null.
  auto front_effect() const noexcept -> const ctrl_lifecycle_detail::Effect* {
    return effects_.empty() ? nullptr : &effects_.front();
  }
  /// @brief Whether a heartbeat reply is still being sent.
  ///
  /// @return True while sending.
  auto heartbeat_pending() const noexcept -> bool { return heartbeat_pending_; }
  /// @brief Whether a passcode reply is still being sent.
  ///
  /// @return True while sending.
  auto pin_pending() const noexcept -> bool { return pin_pending_; }

  /// @brief Moves heartbeat replies ahead of other effects, keeping their
  /// order.
  void promote_heartbeat_replies();
  /// @brief Keeps only the first of several queued passcode requests.
  void coalesce_pin_requests();
  /// @brief Records whether the caller is showing a passcode prompt.
  ///
  /// @param open Whether it is open.
  void set_pin_prompt_open(bool open) noexcept { pin_prompt_open_ = open; }
  /// @brief Sends the reply to the next effect when it is a heartbeat.
  ///
  /// @return True when one was started, false when the next effect is not a
  ///     heartbeat or a send is pending.
  /// @retval LocalCtrlSessionIdError Why it failed.
  [[nodiscard]] auto run_next_heartbeat()
      -> Expected<bool, LocalCtrlSessionIdError>;
  /// @brief Continues sending a heartbeat reply.
  ///
  /// @return True when sent.
  /// @retval LocalCtrlSessionIdError Why it failed.
  [[nodiscard]] auto pump_heartbeat_outbound()
      -> Expected<bool, LocalCtrlSessionIdError>;
  /// @brief Sends the passcode for the next effect when it is a passcode
  /// request.
  ///
  /// @param pin The passcode as ASCII digits.
  /// @return True when one was started, false otherwise.
  /// @retval LocalCtrlSessionIdError::pin_too_large The passcode is too long.
  /// @retval LocalCtrlSessionIdError Why it failed.
  [[nodiscard]] auto run_next_pin_reply(std::span<const std::byte> pin)
      -> Expected<bool, LocalCtrlSessionIdError>;
  /// @brief Continues sending a passcode reply.
  ///
  /// @return True when sent.
  /// @retval LocalCtrlSessionIdError Why it failed.
  [[nodiscard]] auto pump_pin_outbound()
      -> Expected<bool, LocalCtrlSessionIdError>;
  /// @brief Closes the channel when the next effect is a cleanup.
  ///
  /// @return True when closed, false when the next effect is not a cleanup.
  /// @retval LocalCtrlSessionIdError Why it failed.
  [[nodiscard]] auto run_next_cleanup()
      -> Expected<bool, LocalCtrlSessionIdError>;
  /// @brief The channel's owner.
  ///
  /// @return The owner, or null.
  auto owner() noexcept -> CtrlOwner* { return result_.handoff.owner.get(); }
  /// @brief The console's decrypted control reply.
  ///
  /// @return The reply.
  auto ctrl_response() const noexcept -> const CtrlResponseFinal& {
    return result_.handoff.response;
  }

 private:
  /// @brief Moves the owner's effects into this session's queues.
  void drain_effects();

  /// @brief The running channel and nonce.
  LocalCtrlStartupResult result_;
  /// @brief The attempt.
  std::uint64_t attempt_id_{};
  /// @brief The session id until taken.
  std::optional<SessionId> session_id_;
  /// @brief The nonce until taken.
  std::optional<SecretArray<rp_nonce_size>> nonce_;
  /// @brief Effects to carry out.
  std::deque<ctrl_lifecycle_detail::Effect> effects_;
  /// @brief Console notices to pass on.
  std::deque<CtrlNotice> notices_;
  /// @brief Owed repeats of the post-id announcements.
  std::size_t post_id_repeats_{};
  /// @brief Whether the session id was captured.
  bool captured_{};
  /// @brief Whether a heartbeat reply is being sent.
  bool heartbeat_pending_{};
  /// @brief Whether a passcode reply is being sent.
  bool pin_pending_{};
  /// @brief Whether the caller shows a passcode prompt.
  bool pin_prompt_open_{};
};

/// @brief One home control request from send to a running channel.
///
/// Non-blocking; the owner polls pollable() and pumps. Not thread-safe.
class LocalCtrlAttempt {
 public:
  /// @brief What to send and where. References must outlive start().
  struct Config {
    /// @brief The registered console.
    const RegisteredConsole& console;
    /// @brief The host for `Host`.
    std::string_view host;
    /// @brief The control port.
    std::uint16_t port;
    /// @brief The 32-byte device id.
    std::span<const std::byte> did;
    /// @brief The codec to ask for.
    VideoCodec codec;
    /// @brief The 16-byte nonce from the session reply.
    std::span<const std::byte> nonce;
    /// @brief The console's control endpoint.
    platform::SocketEndpoint endpoint;
    /// @brief The attempt, nonzero.
    std::uint64_t attempt_id;
  };

  /// @brief Makes an idle attempt.
  ///
  /// @param crypto The cipher provider; must outlive the attempt.
  /// @param sockets The socket provider; must outlive the attempt.
  /// @param clock The monotonic clock; must outlive the attempt.
  LocalCtrlAttempt(platform::CryptoProvider& crypto,
                   platform::SocketProvider& sockets,
                   const platform::ClockProvider& clock) noexcept;
  LocalCtrlAttempt(const LocalCtrlAttempt&) = delete;
  LocalCtrlAttempt& operator=(const LocalCtrlAttempt&) = delete;

  /// @brief Prepares the request and starts connecting.
  ///
  /// @param config What to send and where.
  /// @return Nothing on success.
  /// @retval LocalCtrlFault::invalid_state Started already, or attempt 0.
  /// @retval LocalCtrlFault::prepare_failed The request could not be made.
  /// @retval ConsoleExchangeError The TCP exchange could not begin.
  [[nodiscard]] auto start(const Config& config)
      -> Expected<void, LocalCtrlError>;
  /// @brief Advances the exchange; on an accepted reply hands the socket over.
  ///
  /// @return True once the channel runs.
  /// @retval LocalCtrlFault::invalid_state Not started, or finished.
  /// @retval LocalCtrlFault::handoff_failure The handoff failed.
  /// @retval ConsoleExchangeError The exchange failed or the console refused.
  [[nodiscard]] auto pump_once() -> Expected<bool, LocalCtrlError>;
  /// @brief The running channel.
  ///
  /// @return The session, or null until ready.
  auto session() noexcept -> LocalCtrlSessionId* {
    return lifecycle_.state() == local_ctrl_lifecycle::State::ready
               ? session_.get()
               : nullptr;
  }
  /// @brief Cancels the exchange, or stops and closes a running channel.
  ///
  /// @return Nothing on success.
  /// @retval LocalCtrlFault::cleanup_failed Closing failed.
  [[nodiscard]] auto cancel() -> Expected<void, LocalCtrlError>;
  /// @brief How far the last failed exchange got.
  ///
  /// @return The progress, or nothing.
  auto last_failure_progress() const noexcept
      -> std::optional<ConsoleExchangeProgress> {
    return exchange_.last_failure_progress();
  }
  /// @brief What the owner polls.
  ///
  /// @return The exchange's or the channel's socket, or null.
  auto pollable() noexcept -> const platform::Pollable*;

 private:
  /// @brief Fails the attempt.
  ///
  /// @param error Why.
  /// @return The error.
  [[nodiscard]] auto fail(LocalCtrlError error) -> Unexpected<LocalCtrlError>;
  /// @brief Cancels the exchange and fails the attempt.
  ///
  /// @param error Why.
  /// @return The error.
  [[nodiscard]] auto abandon(LocalCtrlError error)
      -> Unexpected<LocalCtrlError>;
  /// @brief Wipes the request and cancels the exchange.
  ///
  /// @return The exchange's cancel result.
  [[nodiscard]] auto abandon_exchange() -> Expected<void, ConsoleExchangeError>;
  /// @brief Stops and closes the running channel.
  ///
  /// @return Nothing, or LocalCtrlFault::cleanup_failed.
  [[nodiscard]] auto close_session() -> Expected<void, LocalCtrlError>;

  /// @brief The cipher provider.
  platform::CryptoProvider& crypto_;
  /// @brief The TCP exchange.
  CtrlStartupExchange exchange_;
  /// @brief The prepared request and its secrets.
  PreparedLocalCtrl prepared_;
  /// @brief The console variant.
  RegistrationTarget target_{};
  /// @brief The running channel.
  std::unique_ptr<LocalCtrlSessionId> session_;
  /// @brief The attempt.
  std::uint64_t attempt_id_{};
  /// @brief The attempt's state machine.
  local_ctrl_lifecycle::Lifecycle lifecycle_;
};

/// @brief What one effect step did.
enum class LocalCtrlEffectProgress : std::uint8_t {
  /// @brief Nothing was waiting.
  idle,
  /// @brief A send or effect advanced.
  advanced,
  /// @brief The console wants the login passcode.
  pin_required,
  /// @brief An effect for the caller to act on.
  notification,
  /// @brief An effect of an older attempt was dropped.
  stale,
};

/// @brief The outcome of LocalCtrlSessionId::step_effect().
struct LocalCtrlEffectStep {
  /// @brief What happened.
  LocalCtrlEffectProgress progress{LocalCtrlEffectProgress::idle};
  /// @brief The effect for the caller, with notification.
  std::optional<ctrl_lifecycle_detail::Effect> notification;
};

/// @brief Sends the announcements that follow the session id, one at a time.
///
/// Not thread-safe.
class LocalPostIdCtrl {
 public:
  /// @brief Prepares the announcements.
  ///
  /// @param enable_dualsense Whether to announce DualSense features.
  /// @param enable_keyboard Whether to announce keyboard support.
  explicit LocalPostIdCtrl(bool enable_dualsense = false,
                           bool enable_keyboard = false);

  /// @brief Sends as many announcements as the socket takes.
  ///
  /// @param owner The channel's owner.
  /// @param attempt_id The attempt.
  /// @return True when all are sent, false when the socket is full.
  /// @retval CtrlReaderError Why sending failed.
  [[nodiscard]] auto pump(CtrlOwner& owner, std::uint64_t attempt_id)
      -> Expected<bool, CtrlReaderError>;

  /// @brief Starts the announcements over, for a repeated session id.
  void restart() noexcept;

  /// @brief Whether every announcement was sent.
  ///
  /// @return True when done.
  auto done() const noexcept -> bool { return index_ >= frames_.size(); }

 private:
  /// @brief The announcements.
  std::vector<CtrlOutboundFrame> frames_;
  /// @brief The next announcement.
  std::size_t index_{};
  /// @brief Whether the current announcement is part sent.
  bool pending_{};
};

/// @brief A queue of control messages a caller sends during the stream,
/// such as microphone state.
///
/// Not thread-safe.
class LocalRuntimeCtrl {
 public:
  /// @brief The most messages queued.
  static constexpr std::size_t capacity = 16;

  /// @brief Queues a message.
  ///
  /// @param type The message type.
  /// @param payload The plaintext payload.
  /// @return False when full or the payload is too large.
  [[nodiscard]] auto queue(std::uint16_t type,
                           std::span<const std::byte> payload) -> bool;

  /// @brief Whether a message is part sent.
  ///
  /// @return True while sending.
  auto pending() const noexcept -> bool { return pending_; }

  /// @brief Sends as many queued messages as the socket takes.
  ///
  /// @param owner The channel's owner.
  /// @param attempt_id The attempt.
  /// @return True when the queue is empty, false when the socket is full.
  /// @retval CtrlReaderError Why sending failed; the queue is cleared.
  [[nodiscard]] auto pump(CtrlOwner& owner, std::uint64_t attempt_id)
      -> Expected<bool, CtrlReaderError>;

  /// @brief Empties the queue.
  void clear() noexcept;

 private:
  /// @brief One queued message.
  struct Frame {
    /// @brief The message type.
    std::uint16_t type;
    /// @brief The plaintext payload.
    std::vector<std::byte> payload;
  };
  /// @brief The queue.
  std::deque<Frame> frames_;
  /// @brief Whether the front message is part sent.
  bool pending_{};
};

}  // namespace libchiaking::net::protocol::remote_play
