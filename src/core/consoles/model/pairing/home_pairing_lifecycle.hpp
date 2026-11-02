/// @file
/// @brief Declares the state machine of one attempt to pair a console at
/// home with a PIN: knock, settle, connect, send the request, read the reply,
/// close.
///
/// The machine only decides; it performs nothing. Each transition queues
/// effects (open the knock, start the pause, connect, clean up, publish the
/// outcome) in a LifecycleContext, and HomePairing carries them out and
/// feeds back what the network did as events. Keeping the order of a pairing
/// here lets it be tested without sockets.
#ifndef LIBCHIAKING_CONSOLES_HOME_PAIRING_LIFECYCLE_HPP
#define LIBCHIAKING_CONSOLES_HOME_PAIRING_LIFECYCLE_HPP

#include <boost/sml.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <variant>

namespace libchiaking::consoles::home_pairing_lifecycle {
/// @brief Boost.SML, which the machine is written in.
namespace sml = boost::sml;

/// @brief Where an attempt stands, as HomePairing reports it.
enum class Step : std::uint8_t {
  /// @brief Not started yet.
  idle,
  /// @brief The knock datagram was sent and the console's answer is awaited.
  knocking,
  /// @brief The console answered the knock; a short pause lets it open its
  /// pairing port before connecting.
  settling,
  /// @brief The TCP connection to the console is being opened.
  connecting,
  /// @brief The request header or its encrypted payload is being sent.
  sending,
  /// @brief The console's reply header or body is being read.
  receiving,
  /// @brief The sockets are being closed after the attempt ended.
  closing,
  /// @brief Closed after the console accepted the request; the result is
  /// ready.
  paired,
  /// @brief Closed after the caller cancelled.
  cancelled,
  /// @brief Closed after a failure, or closing itself failed.
  failed,
};

/// @brief Which part of the TCP exchange is under way, so the right
/// PairingExchange call is made next.
enum class Exchange : std::uint8_t {
  /// @brief Waiting for the connection to open.
  connecting,
  /// @brief Sending the HTTP request header.
  sending_header,
  /// @brief Sending the encrypted request body.
  sending_payload,
  /// @brief Reading the console's reply header.
  awaiting_reply_header,
  /// @brief Reading the rest of the console's reply body.
  reading_reply_body,
};

/// @brief Why an attempt failed.
enum class Error : std::uint8_t {
  /// @brief The request could not be built: an unknown target, an empty or
  /// unusable address, a missing account or a PIN the key derivation refused.
  invalid_request,
  /// @brief A call was made in a step that does not accept it.
  invalid_state,
  /// @brief A socket could not be opened, written or read, or the host did
  /// not resolve.
  network_failure,
  /// @brief Random bytes or a cipher operation failed, or the reply could not
  /// be decrypted.
  crypto_failure,
  /// @brief The console's reply was malformed, too large or cut short.
  invalid_reply,
  /// @brief The console answered with a refusal, typically a wrong PIN.
  refused,
  /// @brief The console did not answer in time, or the overall deadline
  /// passed.
  timeout,
  /// @brief The caller cancelled the attempt.
  cancelled,
  /// @brief The attempt's sockets could not be closed.
  cleanup_failure,
};

/// @brief Machine state behind Step::idle.
struct IdleState {};
/// @brief Machine state behind Step::knocking.
struct KnockingState {};
/// @brief Machine state behind Step::settling.
struct SettlingState {};
/// @brief Machine state behind Step::connecting.
struct ConnectingState {};
/// @brief Machine state behind Step::sending while the header goes out.
struct SendingHeaderState {};
/// @brief Machine state behind Step::sending while the payload goes out.
struct SendingPayloadState {};
/// @brief Machine state behind Step::receiving until the reply header is
/// accepted.
struct AwaitingReplyHeaderState {};
/// @brief Machine state behind Step::receiving while the body is read.
struct ReadingReplyBodyState {};
/// @brief Machine state behind Step::closing.
struct ClosingState {};
/// @brief Machine state behind Step::paired; final.
struct PairedState {};
/// @brief Machine state behind Step::cancelled; final.
struct CancelledState {};
/// @brief Machine state behind Step::failed; final.
struct FailedState {};

/// @brief Event: begin an attempt.
struct Start {
  /// @brief The attempt being started; must not be zero.
  std::uint64_t attempt_id{};
};
/// @brief Event: the console answered the knock.
struct KnockAnswered {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Event: the pause after the knock is over.
struct DelayElapsed {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Event: the TCP connection is open.
struct Connected {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Event: the connection is not open yet after all; go back to
/// waiting for it.
struct ConnectionPending {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Event: the whole request header was sent.
struct HeaderSent {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Event: the whole request body was sent.
struct RequestSent {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Event: the reply header was read and accepted; its body follows.
struct ReplyHeaderAccepted {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Event: the console's reply was decrypted into a registered
/// console.
struct ReplyDecoded {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Event: the caller cancelled.
struct Cancel {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Event: something failed.
struct Failed {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
  /// @brief What failed.
  Error error{Error::network_failure};
};
/// @brief Event: the cleanup finished.
struct Closed {
  /// @brief The attempt the event belongs to.
  std::uint64_t attempt_id{};
  /// @brief Whether every socket closed cleanly.
  bool success{true};
};

/// @brief Effect: open the UDP socket and send the knock.
struct BeginKnock {
  /// @brief The attempt the effect belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Effect: start the pause between the knock's answer and the
/// connection.
struct WaitAfterKnock {
  /// @brief The attempt the effect belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Effect: start the TCP connection to the console that answered.
struct BeginConnect {
  /// @brief The attempt the effect belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Effect: close every socket and wipe the prepared secrets, then
/// report Closed.
struct Cleanup {
  /// @brief The attempt the effect belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Effect: the attempt ended paired.
struct PublishPaired {
  /// @brief The attempt the effect belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Effect: the attempt ended cancelled.
struct PublishCancelled {
  /// @brief The attempt the effect belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Effect: the attempt ended failed.
struct PublishFailed {
  /// @brief The attempt the effect belongs to.
  std::uint64_t attempt_id{};
  /// @brief Why it failed: the first failure reported, or
  /// Error::cleanup_failure when only closing failed.
  Error error{Error::network_failure};
};

/// @brief Any effect the machine can queue.
using LifecycleEffect =
    std::variant<BeginKnock, WaitAfterKnock, BeginConnect, Cleanup,
                 PublishPaired, PublishCancelled, PublishFailed>;

/// @brief How the attempt is to end once closing finishes: paired after a
/// decoded reply, cancelled after Cancel, failed after Failed.
enum class DesiredOutcome { paired, cancelled, failed };
/// @var DesiredOutcome::paired
/// @brief End paired; set by Start and by ReplyDecoded.
/// @var DesiredOutcome::cancelled
/// @brief End cancelled; set by Cancel.
/// @var DesiredOutcome::failed
/// @brief End failed; set by Failed or by a failed close.

/// @brief What the machine shares with its driver: the queued effects, the
/// current attempt and how it should end, plus counts of refused events.
///
/// Owned by the driver, which must drain #effects after each event it
/// processes. Not safe to share between threads.
struct LifecycleContext {
  /// @brief Effects queued by transitions and not yet carried out, oldest
  /// first.
  std::deque<LifecycleEffect> effects;
  /// @brief The attempt in progress; set by Start and cleared once its
  /// outcome is published.
  std::optional<std::uint64_t> active_attempt_id{};
  /// @brief How the attempt is to end once closing finishes.
  DesiredOutcome desired{DesiredOutcome::paired};
  /// @brief The first failure of the attempt; empty while none happened.
  std::optional<Error> error{};
  /// @brief Events refused because they named an attempt that is not the
  /// current one.
  std::size_t stale_events{};
  /// @brief Events refused because the current state does not accept them,
  /// and Start events refused because an attempt was active or the id was
  /// zero.
  std::size_t unexpected_events{};

  /// @brief Whether an event belongs to the attempt in progress.
  ///
  /// @param attempt_id The attempt the event names.
  /// @return True when an attempt is in progress and it is `attempt_id`.
  bool is_current(std::uint64_t attempt_id) const noexcept {
    return active_attempt_id && *active_attempt_id == attempt_id;
  }
};

/// @brief The transition table of a pairing attempt.
///
/// From idle, Start knocks; KnockAnswered settles; DelayElapsed connects;
/// Connected, HeaderSent, RequestSent and ReplyHeaderAccepted walk through
/// the exchange, and ConnectionPending returns to connecting. ReplyDecoded,
/// Cancel and Failed in any active step queue a Cleanup and move to closing,
/// where Closed ends the attempt as paired, cancelled or failed. A failed
/// close always ends it failed. The final states accept nothing, so a
/// machine serves one attempt.
struct LifecycleFsm {
  /// @brief Builds the transition table.
  ///
  /// @return The Boost.SML table described above.
  auto operator()() const noexcept {
    using namespace sml;

    const auto begin_knock = [](const Start& event, LifecycleContext& context) {
      context.active_attempt_id = event.attempt_id;
      context.desired = DesiredOutcome::paired;
      context.error.reset();
      context.effects.emplace_back(BeginKnock{event.attempt_id});
    };
    const auto wait_after_knock = [](const KnockAnswered& event,
                                     LifecycleContext& context) {
      context.effects.emplace_back(WaitAfterKnock{event.attempt_id});
    };
    const auto begin_connect = [](const DelayElapsed& event,
                                  LifecycleContext& context) {
      context.effects.emplace_back(BeginConnect{event.attempt_id});
    };
    const auto begin_cleanup_paired = [](const ReplyDecoded& event,
                                         LifecycleContext& context) {
      context.desired = DesiredOutcome::paired;
      context.effects.emplace_back(Cleanup{event.attempt_id});
    };
    const auto begin_cleanup_cancelled = [](const Cancel& event,
                                            LifecycleContext& context) {
      context.desired = DesiredOutcome::cancelled;
      context.effects.emplace_back(Cleanup{event.attempt_id});
    };
    const auto begin_cleanup_failed = [](const Failed& event,
                                         LifecycleContext& context) {
      context.desired = DesiredOutcome::failed;
      context.error = event.error;
      context.effects.emplace_back(Cleanup{event.attempt_id});
    };
    const auto record_closing_failure = [](const Failed& event,
                                           LifecycleContext& context) {
      context.desired = DesiredOutcome::failed;
      if (!context.error) context.error = event.error;
    };
    const auto publish_terminal = [](const Closed& event,
                                     LifecycleContext& context) {
      if (event.success) {
        switch (context.desired) {
          case DesiredOutcome::paired:
            context.effects.emplace_back(PublishPaired{event.attempt_id});
            break;
          case DesiredOutcome::cancelled:
            context.effects.emplace_back(PublishCancelled{event.attempt_id});
            break;
          case DesiredOutcome::failed:
            context.effects.emplace_back(
                PublishFailed{event.attempt_id,
                              context.error.value_or(Error::network_failure)});
            break;
        }
      } else {
        context.desired = DesiredOutcome::failed;
        if (!context.error) context.error = Error::cleanup_failure;
        context.effects.emplace_back(PublishFailed{
            event.attempt_id, context.error.value_or(Error::cleanup_failure)});
      }
      context.active_attempt_id.reset();
    };

    const auto paired = [](const Closed& e, const LifecycleContext& c) {
      return e.success && c.desired == DesiredOutcome::paired;
    };
    const auto cancelled = [](const Closed& e, const LifecycleContext& c) {
      return e.success && c.desired == DesiredOutcome::cancelled;
    };
    const auto failed = [](const Closed& e, const LifecycleContext& c) {
      return !e.success || c.desired == DesiredOutcome::failed;
    };

    return make_transition_table(
        *state<IdleState> + event<Start> / begin_knock = state<KnockingState>,
        state<KnockingState> + event<KnockAnswered> / wait_after_knock =
            state<SettlingState>,
        state<SettlingState> + event<DelayElapsed> / begin_connect =
            state<ConnectingState>,
        state<ConnectingState> + event<Connected> = state<SendingHeaderState>,
        state<SendingHeaderState> + event<ConnectionPending> =
            state<ConnectingState>,
        state<SendingHeaderState> + event<HeaderSent> =
            state<SendingPayloadState>,
        state<SendingPayloadState> + event<RequestSent> =
            state<AwaitingReplyHeaderState>,
        state<AwaitingReplyHeaderState> + event<ReplyHeaderAccepted> =
            state<ReadingReplyBodyState>,
        state<AwaitingReplyHeaderState> +
            event<ReplyDecoded> / begin_cleanup_paired = state<ClosingState>,
        state<ReadingReplyBodyState> +
            event<ReplyDecoded> / begin_cleanup_paired = state<ClosingState>,

        state<KnockingState> + event<Cancel> / begin_cleanup_cancelled =
            state<ClosingState>,
        state<SettlingState> + event<Cancel> / begin_cleanup_cancelled =
            state<ClosingState>,
        state<ConnectingState> + event<Cancel> / begin_cleanup_cancelled =
            state<ClosingState>,
        state<SendingHeaderState> + event<Cancel> / begin_cleanup_cancelled =
            state<ClosingState>,
        state<SendingPayloadState> + event<Cancel> / begin_cleanup_cancelled =
            state<ClosingState>,
        state<AwaitingReplyHeaderState> +
            event<Cancel> / begin_cleanup_cancelled = state<ClosingState>,
        state<ReadingReplyBodyState> + event<Cancel> / begin_cleanup_cancelled =
            state<ClosingState>,

        state<KnockingState> + event<Failed> / begin_cleanup_failed =
            state<ClosingState>,
        state<SettlingState> + event<Failed> / begin_cleanup_failed =
            state<ClosingState>,
        state<ConnectingState> + event<Failed> / begin_cleanup_failed =
            state<ClosingState>,
        state<SendingHeaderState> + event<Failed> / begin_cleanup_failed =
            state<ClosingState>,
        state<SendingPayloadState> + event<Failed> / begin_cleanup_failed =
            state<ClosingState>,
        state<AwaitingReplyHeaderState> + event<Failed> / begin_cleanup_failed =
            state<ClosingState>,
        state<ReadingReplyBodyState> + event<Failed> / begin_cleanup_failed =
            state<ClosingState>,

        state<ClosingState> + event<Failed> / record_closing_failure,

        state<ClosingState> + event<Closed>[paired] / publish_terminal =
            state<PairedState>,
        state<ClosingState> + event<Closed>[cancelled] / publish_terminal =
            state<CancelledState>,
        state<ClosingState> + event<Closed>[failed] / publish_terminal =
            state<FailedState>);
  }
};

/// @brief The machine with its event filter: events are processed only for
/// the attempt in progress, and refused events are counted in the context.
///
/// Holds a reference to its context, which must outlive it. Not safe to
/// share between threads.
class Lifecycle {
 public:
  /// @brief Makes an idle machine.
  ///
  /// @param context Where effects are queued; must outlive this object.
  explicit Lifecycle(LifecycleContext& context)
      : context_(context), machine_(context) {}

  /// @brief Starts an attempt.
  ///
  /// @param event The start event.
  /// @return True when the attempt started and BeginKnock was queued; false
  ///     when the id is zero, an attempt is active, or the machine is not
  ///     idle.
  [[nodiscard]] bool process(const Start& event) {
    if (event.attempt_id == 0 || context_.active_attempt_id) {
      ++context_.unexpected_events;
      return false;
    }
    return process_current(event);
  }

  /// @brief Feeds an event of the attempt in progress to the machine.
  ///
  /// @tparam Event One of the event types above other than Start.
  /// @param event The event.
  /// @return True when a transition took it; false when it names another
  ///     attempt (counted as stale) or the current state does not accept it
  ///     (counted as unexpected).
  template <class Event>
  [[nodiscard]] bool process(const Event& event) {
    if (!context_.is_current(event.attempt_id)) {
      ++context_.stale_events;
      return false;
    }
    return process_current(event);
  }

  /// @brief Where the attempt stands.
  ///
  /// @return The step of the current machine state.
  Step state() const noexcept {
    if (machine_.is(sml::state<IdleState>)) return Step::idle;
    if (machine_.is(sml::state<KnockingState>)) return Step::knocking;
    if (machine_.is(sml::state<SettlingState>)) return Step::settling;
    if (machine_.is(sml::state<ConnectingState>)) return Step::connecting;
    if (machine_.is(sml::state<SendingHeaderState>) ||
        machine_.is(sml::state<SendingPayloadState>))
      return Step::sending;
    if (machine_.is(sml::state<AwaitingReplyHeaderState>) ||
        machine_.is(sml::state<ReadingReplyBodyState>))
      return Step::receiving;
    if (machine_.is(sml::state<ClosingState>)) return Step::closing;
    if (machine_.is(sml::state<PairedState>)) return Step::paired;
    if (machine_.is(sml::state<CancelledState>)) return Step::cancelled;
    return Step::failed;
  }

  /// @brief Which part of the TCP exchange is under way.
  ///
  /// @return The exchange part, or nothing outside connecting, sending and
  ///     receiving.
  std::optional<Exchange> exchange() const noexcept {
    if (machine_.is(sml::state<ConnectingState>)) return Exchange::connecting;
    if (machine_.is(sml::state<SendingHeaderState>))
      return Exchange::sending_header;
    if (machine_.is(sml::state<SendingPayloadState>))
      return Exchange::sending_payload;
    if (machine_.is(sml::state<AwaitingReplyHeaderState>))
      return Exchange::awaiting_reply_header;
    if (machine_.is(sml::state<ReadingReplyBodyState>))
      return Exchange::reading_reply_body;
    return std::nullopt;
  }

 private:
  /// @brief Feeds an event to the machine, counting it as unexpected when no
  /// transition takes it.
  ///
  /// @tparam Event Any event type above.
  /// @param event The event.
  /// @return True when a transition took it.
  template <class Event>
  [[nodiscard]] bool process_current(const Event& event) {
    if (!machine_.process_event(event)) {
      ++context_.unexpected_events;
      return false;
    }
    return true;
  }

  /// @brief Where effects are queued and refused events counted.
  LifecycleContext& context_;
  /// @brief The Boost.SML machine running LifecycleFsm on #context_.
  sml::sm<LifecycleFsm> machine_;
};

}  // namespace libchiaking::consoles::home_pairing_lifecycle

#endif
