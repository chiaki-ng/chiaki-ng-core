/// @file
/// @brief Declares ConnectionHandshake, the state machine that opens a Takion
/// association (INIT, INIT_ACK, COOKIE_ECHO, COOKIE_ACK), with its events,
/// actions and errors.
///
/// The machine performs no I/O: it is handed events (start, an answer
/// received, a deadline passed) and answers with the action the client must
/// take (send this INIT, send this cookie, the association is up). Each start
/// begins a new generation and each resend a new attempt, and events carry
/// the identity they belong to, so a late answer to an earlier attempt is
/// rejected rather than acted on.
#pragma once

#include "src/core/net/protocol/takion/detail/handshake_types.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <boost/sml.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace libchiaking::net::protocol::takion {

/// @brief Where the console's initial reliable sequence number comes from.
enum class RemoteSequencePolicy : std::uint8_t {
  /// @brief Use the console's verification tag as its initial sequence number,
  /// as chiaki-ng does; the default.
  remote_tag_legacy_compatibility,
  /// @brief Use the initial sequence number the INIT_ACK advertises.
  advertised_initial_sequence_number,
};

/// @brief Which handshake run and which send an event belongs to.
struct HandshakeIdentity {
  /// @brief Incremented by every start.
  std::uint64_t generation{};
  /// @brief Which send of the current packet, from 1.
  std::uint32_t attempt{};

  /// @brief Compares both parts.
  ///
  /// @return Whether both identities are the same.
  constexpr bool operator==(const HandshakeIdentity&) const noexcept = default;
};

/// @brief Event: begin a handshake.
struct Start {
  /// @brief Our verification tag; must not be 0.
  VerificationTag local_tag{};
  /// @brief Our first reliable sequence number.
  InitialSequenceNumber local_initial_sequence_number{};
  /// @brief The current time, in monotonic microseconds.
  std::uint64_t now_microseconds{};
};

/// @brief Event: an INIT_ACK arrived.
struct InitAcknowledgementReceived {
  /// @brief The handshake the answer is taken to belong to.
  HandshakeIdentity identity{};
  /// @brief The tag in the packet's message header; must be ours.
  VerificationTag message_tag{};
  /// @brief The chunk's values and cookie.
  InitAcknowledgementValues acknowledgement{};
  /// @brief When it arrived, in monotonic microseconds.
  std::uint64_t now_microseconds{};
};

/// @brief Event: a COOKIE_ACK arrived.
struct CookieAcknowledgementReceived {
  /// @brief The handshake the answer is taken to belong to.
  HandshakeIdentity identity{};
  /// @brief The tag in the packet's message header; must be ours.
  VerificationTag message_tag{};
  /// @brief When it arrived, in monotonic microseconds.
  std::uint64_t now_microseconds{};
};

/// @brief Event: the answer deadline may have passed, or the socket reported
/// a receive error.
struct Timeout {
  /// @brief The handshake being timed.
  HandshakeIdentity identity{};
  /// @brief The current time, in monotonic microseconds.
  std::uint64_t now_microseconds{};
  /// @brief The socket reported a receive failure while handshaking, so the
  /// resend is due now whatever the deadline.
  bool receive_failed{};
};

/// @brief Event: abandon a handshake.
struct Cancel {
  /// @brief The generation to abandon.
  std::uint64_t generation{};
};

/// @brief The events ConnectionHandshake::handle() accepts.
///
/// @tparam Input One of Start, InitAcknowledgementReceived,
///     CookieAcknowledgementReceived, Timeout or Cancel.
template <class Input>
concept HandshakeEvent =
    std::same_as<Input, Start> ||
    std::same_as<Input, InitAcknowledgementReceived> ||
    std::same_as<Input, CookieAcknowledgementReceived> ||
    std::same_as<Input, Timeout> || std::same_as<Input, Cancel>;

/// @brief Why an event was refused or the handshake failed.
enum class HandshakeError : std::uint8_t {
  /// @brief No error.
  none,
  /// @brief The event has no meaning in the current state.
  wrong_state,
  /// @brief The event belongs to an earlier generation or attempt.
  stale_attempt,
  /// @brief An answer was addressed to another tag, or a second INIT_ACK
  /// differs from the first.
  tag_mismatch,
  /// @brief Start was given tag 0.
  zero_local_tag,
  /// @brief The INIT_ACK carried tag 0; fatal.
  zero_remote_tag,
  /// @brief The console's stream counts are zero or do not cover ours: its
  /// outbound must not exceed our inbound, its inbound must cover our
  /// outbound. Fatal on the first INIT_ACK.
  invalid_stream_counts,
  /// @brief A timeout was offered before the deadline.
  timeout_not_due,
  /// @brief The last allowed send went unanswered; fatal.
  attempts_exhausted,
  /// @brief Cancel was handled; fatal.
  cancelled,
};

/// @brief Action: nothing to do (a duplicate INIT_ACK was ignored).
struct NoAction {};

/// @brief Action: send an INIT chunk, with verification tag 0 in the header.
struct SendInit {
  /// @brief The send this is.
  HandshakeIdentity identity{};
  /// @brief Our INIT values.
  InitValues values{};
  /// @brief When to resend it.
  HandshakeDeadline deadline{};
};

/// @brief Action: send a COOKIE_ECHO chunk carrying the cookie.
struct SendCookie {
  /// @brief The send this is.
  HandshakeIdentity identity{};
  /// @brief The console's tag, for the message header.
  VerificationTag remote_tag{};
  /// @brief The cookie to echo.
  HandshakeCookie cookie{};
  /// @brief When to resend it.
  HandshakeDeadline deadline{};
};

/// @brief Action: the association is up.
struct Connected {
  /// @brief The handshake that completed.
  HandshakeIdentity identity{};
  /// @brief What was agreed.
  EstablishedHandshake handshake{};
};

/// @brief Action: the event was refused; the handshake goes on.
struct Rejected {
  /// @brief Why.
  HandshakeError error{HandshakeError::none};
};

/// @brief Action: the handshake failed for good.
struct Failed {
  /// @brief Why.
  HandshakeError error{HandshakeError::none};
};

/// @brief Any action the handshake asks for.
using HandshakeAction =
    std::variant<NoAction, SendInit, SendCookie, Connected, Rejected, Failed>;

/// @brief A point-in-time view of a handshake.
struct HandshakeSnapshot {
  /// @brief Where it is.
  HandshakeState state{HandshakeState::idle};
  /// @brief The current generation.
  std::uint64_t generation{};
  /// @brief The current send.
  std::uint32_t attempt{};
  /// @brief When the outstanding packet is due to be resent.
  HandshakeDeadline deadline{};
  /// @brief What was agreed, once connected.
  std::optional<EstablishedHandshake> established{};
};

namespace handshake_detail {
namespace sml = boost::sml;

/// @brief Not started.
struct IdleState {};
/// @brief INIT sent; waiting for INIT_ACK.
struct AwaitingInitAcknowledgementState {};
/// @brief COOKIE_ECHO sent; waiting for COOKIE_ACK.
struct AwaitingCookieAcknowledgementState {};
/// @brief The association is up.
struct ConnectedState {};
/// @brief Failed or cancelled; a new Start may begin again.
struct FailedState {};

/// @brief The handshake's memory, shared by reference with the machine's
/// guards and actions.
struct Context {
  /// @brief How patiently to resend.
  HandshakeRetryPolicy retry_policy{};
  /// @brief Window we advertise.
  AdvertisedReceiveWindow advertised_receive_window{};
  /// @brief Outbound streams we ask for.
  StreamCount outbound_streams{};
  /// @brief Inbound streams we ask for.
  StreamCount inbound_streams{};
  /// @brief Where the console's initial sequence number comes from.
  RemoteSequencePolicy remote_sequence_policy{};

  /// @brief Current generation; incremented by each Start.
  std::uint64_t generation{};
  /// @brief Current send of the outstanding packet, from 1.
  std::uint32_t attempt{};
  /// @brief When the outstanding packet is due to be resent.
  HandshakeDeadline deadline{};
  /// @brief Our INIT values, once started.
  std::optional<InitValues> local;
  /// @brief The console's INIT_ACK, once accepted.
  std::optional<InitAcknowledgementValues> remote;
  /// @brief What was agreed, once connected.
  std::optional<EstablishedHandshake> established;
  /// @brief The action the last event produced; taken by handle().
  std::optional<HandshakeAction> action;
  /// @brief Events no transition applied to.
  std::size_t unexpected_events{};

  /// @brief Names the current run and send.
  ///
  /// @return The identity.
  HandshakeIdentity identity() const noexcept { return {generation, attempt}; }

  /// @brief Arms the resend deadline one response timeout from now.
  ///
  /// @param now_microseconds The current time, in monotonic microseconds.
  void arm_deadline(std::uint64_t now_microseconds) noexcept {
    deadline = {timing::Deadline::after_us(
        now_microseconds, retry_policy.response_timeout_microseconds)};
  }

  /// @brief Builds the INIT action for the current send.
  ///
  /// @return The action.
  SendInit send_init() const { return {identity(), *local, deadline}; }

  /// @brief Builds the COOKIE_ECHO action for the current send.
  ///
  /// @return The action.
  SendCookie send_cookie() const {
    return {identity(), remote->init.tag, remote->cookie, deadline};
  }

  /// @brief Checks the console's stream counts against ours.
  ///
  /// @param candidate The console's INIT_ACK.
  /// @return Whether both are non-zero, its outbound does not exceed our
  /// inbound
  ///     and its inbound is at least our outbound.
  bool accepts_stream_counts(
      const InitAcknowledgementValues& candidate) const noexcept {
    return candidate.init.outbound_streams.value != 0 &&
           candidate.init.inbound_streams.value != 0 &&
           candidate.init.outbound_streams.value <= inbound_streams.value &&
           candidate.init.inbound_streams.value >= outbound_streams.value;
  }

  /// @brief Assembles what was agreed.
  ///
  /// @return The established association.
  EstablishedHandshake establish() const noexcept {
    const auto remote_sequence =
        remote_sequence_policy ==
                RemoteSequencePolicy::remote_tag_legacy_compatibility
            ? InitialSequenceNumber{remote->init.tag.value}
            : remote->init.initial_sequence_number;
    return EstablishedHandshake{
        local->tag,
        remote->init.tag,
        local->initial_sequence_number,
        remote_sequence,
        outbound_streams,
        inbound_streams,
        remote->init.advertised_receive_window,
    };
  }
};

/// @brief The handshake's Boost.SML transition table.
///
/// INIT_ACK is accepted only for the current identity, addressed to our tag,
/// with a non-zero tag and acceptable stream counts; it moves to awaiting the
/// COOKIE_ACK and restarts the attempt count. A repeated identical INIT_ACK is
/// ignored. COOKIE_ACK addressed to us completes it. A due timeout resends
/// until the attempts run out.
struct Fsm {
  /// @brief Builds the transition table.
  ///
  /// @return The table.
  auto operator()() const noexcept {
    using namespace sml;

    const auto valid_start = [](const Start& event) noexcept {
      return event.local_tag.value != 0;
    };
    const auto current = [](const auto& event,
                            const Context& context) noexcept {
      return event.identity == context.identity();
    };
    const auto addressed_to_us = [](const auto& event,
                                    const Context& context) noexcept {
      return event.message_tag == context.local->tag;
    };
    const auto remote_tag_nonzero =
        [](const InitAcknowledgementReceived& event) noexcept {
          return event.acknowledgement.init.tag.value != 0;
        };
    const auto stream_counts_valid =
        [](const InitAcknowledgementReceived& event,
           const Context& context) noexcept {
          return context.accepts_stream_counts(event.acknowledgement);
        };
    const auto duplicate = [](const InitAcknowledgementReceived& event,
                              const Context& context) noexcept {
      return context.remote == event.acknowledgement;
    };
    const auto timeout_due = [](const Timeout& event,
                                const Context& context) noexcept {
      return event.receive_failed ||
             event.now_microseconds >= context.deadline.monotonic_microseconds;
    };
    const auto attempts_available = [](const Context& context) noexcept {
      return context.attempt < context.retry_policy.maximum_attempts;
    };
    const auto cancel_matches = [](const Cancel& event,
                                   const Context& context) noexcept {
      return event.generation == context.generation;
    };

    const auto start = [](const Start& event, Context& context) {
      ++context.generation;
      context.attempt = 1;
      context.local = InitValues{
          event.local_tag,
          context.advertised_receive_window,
          context.outbound_streams,
          context.inbound_streams,
          event.local_initial_sequence_number,
      };
      context.remote.reset();
      context.established.reset();
      context.arm_deadline(event.now_microseconds);
      context.action = context.send_init();
    };
    const auto stage_init_acknowledgement =
        [](const InitAcknowledgementReceived& event, Context& context) {
          context.remote = event.acknowledgement;
          context.attempt = 1;
          context.arm_deadline(event.now_microseconds);
          context.action = context.send_cookie();
        };
    const auto ignore_duplicate = [](Context& context) {
      context.action = NoAction{};
    };
    const auto complete = [](Context& context) {
      context.established = context.establish();
      context.action = Connected{context.identity(), *context.established};
    };
    const auto retry = [](const Timeout& event, Context& context) {
      ++context.attempt;
      context.arm_deadline(event.now_microseconds);
    };
    const auto resend_init = [](Context& context) {
      context.action = context.send_init();
    };
    const auto resend_cookie = [](Context& context) {
      context.action = context.send_cookie();
    };
    const auto reject = [](HandshakeError error) {
      return [error](Context& context) { context.action = Rejected{error}; };
    };
    const auto fail = [](HandshakeError error) {
      return [error](Context& context) { context.action = Failed{error}; };
    };
    const auto unexpected = [](Context& context) {
      ++context.unexpected_events;
      context.action = Rejected{HandshakeError::wrong_state};
    };

    return make_transition_table(
        *state<IdleState> + event<Start>[valid_start] / start =
            state<AwaitingInitAcknowledgementState>,
        state<IdleState> +
            event<Start>[!valid_start] / reject(HandshakeError::zero_local_tag),

        state<FailedState> + event<Start>[valid_start] / start =
            state<AwaitingInitAcknowledgementState>,
        state<FailedState> +
            event<Start>[!valid_start] / reject(HandshakeError::zero_local_tag),

        state<AwaitingInitAcknowledgementState> +
            event<InitAcknowledgementReceived>[current && addressed_to_us &&
                                               remote_tag_nonzero &&
                                               stream_counts_valid] /
                stage_init_acknowledgement =
            state<AwaitingCookieAcknowledgementState>,
        state<AwaitingInitAcknowledgementState> +
            event<InitAcknowledgementReceived>[current && addressed_to_us &&
                                               remote_tag_nonzero] /
                fail(HandshakeError::invalid_stream_counts) =
            state<FailedState>,
        state<AwaitingInitAcknowledgementState> +
            event<InitAcknowledgementReceived>[current && addressed_to_us] /
                fail(HandshakeError::zero_remote_tag) = state<FailedState>,
        state<AwaitingInitAcknowledgementState> +
            event<InitAcknowledgementReceived>[current] /
                reject(HandshakeError::tag_mismatch),
        state<AwaitingInitAcknowledgementState> +
            event<InitAcknowledgementReceived>[!current] /
                reject(HandshakeError::stale_attempt),

        state<AwaitingCookieAcknowledgementState> +
            event<InitAcknowledgementReceived>[current && addressed_to_us &&
                                               remote_tag_nonzero &&
                                               stream_counts_valid &&
                                               duplicate] /
                ignore_duplicate,
        state<AwaitingCookieAcknowledgementState> +
            event<InitAcknowledgementReceived>[current && addressed_to_us &&
                                               remote_tag_nonzero &&
                                               stream_counts_valid] /
                reject(HandshakeError::tag_mismatch),
        state<AwaitingCookieAcknowledgementState> +
            event<InitAcknowledgementReceived>[current && addressed_to_us &&
                                               remote_tag_nonzero] /
                reject(HandshakeError::invalid_stream_counts),
        state<AwaitingCookieAcknowledgementState> +
            event<InitAcknowledgementReceived>[current && addressed_to_us] /
                reject(HandshakeError::zero_remote_tag),
        state<AwaitingCookieAcknowledgementState> +
            event<InitAcknowledgementReceived>[current] /
                reject(HandshakeError::tag_mismatch),
        state<AwaitingCookieAcknowledgementState> +
            event<InitAcknowledgementReceived>[!current] /
                reject(HandshakeError::stale_attempt),

        state<AwaitingCookieAcknowledgementState> +
            event<CookieAcknowledgementReceived>[current && addressed_to_us] /
                complete = state<ConnectedState>,
        state<AwaitingCookieAcknowledgementState> +
            event<CookieAcknowledgementReceived>[current] /
                reject(HandshakeError::tag_mismatch),
        state<AwaitingCookieAcknowledgementState> +
            event<CookieAcknowledgementReceived>[!current] /
                reject(HandshakeError::stale_attempt),

        state<AwaitingInitAcknowledgementState> +
            event<Timeout>[current && timeout_due && attempts_available] /
                (retry, resend_init),
        state<AwaitingInitAcknowledgementState> +
            event<Timeout>[current && timeout_due] /
                fail(HandshakeError::attempts_exhausted) = state<FailedState>,
        state<AwaitingInitAcknowledgementState> +
            event<Timeout>[current && !timeout_due] /
                reject(HandshakeError::timeout_not_due),
        state<AwaitingInitAcknowledgementState> +
            event<Timeout>[!current] / reject(HandshakeError::stale_attempt),

        state<AwaitingCookieAcknowledgementState> +
            event<Timeout>[current && timeout_due && attempts_available] /
                (retry, resend_cookie),
        state<AwaitingCookieAcknowledgementState> +
            event<Timeout>[current && timeout_due] /
                fail(HandshakeError::attempts_exhausted) = state<FailedState>,
        state<AwaitingCookieAcknowledgementState> +
            event<Timeout>[current && !timeout_due] /
                reject(HandshakeError::timeout_not_due),
        state<AwaitingCookieAcknowledgementState> +
            event<Timeout>[!current] / reject(HandshakeError::stale_attempt),

        state<AwaitingInitAcknowledgementState> +
            event<Cancel>[cancel_matches] / fail(HandshakeError::cancelled) =
            state<FailedState>,
        state<AwaitingInitAcknowledgementState> +
            event<Cancel>[!cancel_matches] /
                reject(HandshakeError::stale_attempt),
        state<AwaitingCookieAcknowledgementState> +
            event<Cancel>[cancel_matches] / fail(HandshakeError::cancelled) =
            state<FailedState>,
        state<AwaitingCookieAcknowledgementState> +
            event<Cancel>[!cancel_matches] /
                reject(HandshakeError::stale_attempt),

        state<_> + unexpected_event<Start> / unexpected,
        state<_> + unexpected_event<InitAcknowledgementReceived> / unexpected,
        state<_> + unexpected_event<CookieAcknowledgementReceived> / unexpected,
        state<_> + unexpected_event<Timeout> / unexpected,
        state<_> + unexpected_event<Cancel> / unexpected);
  }
};

}  // namespace handshake_detail

/// @brief One Takion association handshake.
///
/// Owned by the Takion client and driven under its lock; not copyable.
/// handle() is synchronous and does no I/O.
class ConnectionHandshake {
 public:
  /// @brief Makes an idle handshake.
  ///
  /// @param retry_policy Sends per packet and the wait for each.
  /// @param advertised_receive_window Window to advertise in INIT.
  /// @param outbound_streams Outbound streams to ask for.
  /// @param inbound_streams Inbound streams to ask for.
  /// @param remote_sequence_policy Where the console's initial sequence number
  ///     comes from.
  explicit ConnectionHandshake(
      HandshakeRetryPolicy retry_policy = {},
      AdvertisedReceiveWindow advertised_receive_window = {},
      StreamCount outbound_streams = StreamCount{default_outbound_stream_count},
      StreamCount inbound_streams = StreamCount{default_inbound_stream_count},
      RemoteSequencePolicy remote_sequence_policy =
          RemoteSequencePolicy::remote_tag_legacy_compatibility);

  /// @brief Not copyable.
  ConnectionHandshake(const ConnectionHandshake&) = delete;
  /// @brief Not copy-assignable.
  ConnectionHandshake& operator=(const ConnectionHandshake&) = delete;

  /// @brief Feeds an event and returns what to do about it.
  ///
  /// @tparam Input One of the HandshakeEvent types.
  /// @param event The event.
  /// @return SendInit, SendCookie or Connected to act on; NoAction for an
  ///     ignored duplicate; Rejected when the event is refused and the
  ///     handshake goes on; Failed when it has failed.
  template <HandshakeEvent Input>
  [[nodiscard]] HandshakeAction handle(const Input& event) {
    context_.action.reset();
    machine_.process_event(event);
    return std::exchange(context_.action, std::nullopt)
        .value_or(Rejected{HandshakeError::wrong_state});
  }

  /// @brief Reads where the handshake is.
  ///
  /// @return The state, identity, deadline and agreement.
  HandshakeSnapshot snapshot() const;

 private:
  /// @brief The handshake's memory.
  handshake_detail::Context context_;
  /// @brief The machine.
  handshake_detail::sml::sm<handshake_detail::Fsm> machine_;
};

}  // namespace libchiaking::net::protocol::takion
