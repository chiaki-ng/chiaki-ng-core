/// @file
/// @brief The state machine of one control channel attempt: waiting for the
/// session id, active, closing, and the effects each received message asks
/// the owner to carry out.
///
/// The machine does no I/O. Its owner (CtrlOwner at home, the internet link's
/// remote control session over PSN) feeds it events tagged with an attempt
/// id, then drains Context::effects and acts on them. Events from an older
/// attempt are counted and ignored.
#pragma once

#include <libchiaking/platform/runtime/threading.hpp>
#include "ctrl_codec.hpp"

#include <boost/sml.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <variant>

namespace libchiaking::net::protocol::remote_play::ctrl_lifecycle_detail {
namespace sml = boost::sml;

/// @brief The phase of a control attempt.
enum class State : std::uint8_t {
  /// @brief Not started.
  idle,
  /// @brief Started; waiting for the console's session id. A login passcode
  /// may be requested in this phase.
  waiting_id,
  /// @brief The session id arrived; the channel serves heartbeats and notices.
  active,
  /// @brief Stopping or failed; waiting for the owner to close the transport.
  closing,
  /// @brief Closed cleanly after a stop.
  stopped,
  /// @brief Closed after a failure.
  failed
};
/// @brief What made a control attempt fail.
enum class FailureReason : std::uint8_t { transport, protocol };
/// @var libchiaking::net::protocol::remote_play::ctrl_lifecycle_detail::FailureReason::transport
/// @brief The connection failed or closed.
/// @var libchiaking::net::protocol::remote_play::ctrl_lifecycle_detail::FailureReason::protocol
/// @brief The console sent something the session cannot continue after.

/// @brief State tag: not started.
struct Idle {};
/// @brief State tag: waiting for the session id.
struct WaitingId {};
/// @brief State tag: active.
struct Active {};
/// @brief State tag: closing.
struct Closing {};
/// @brief State tag: stopped.
struct Stopped {};
/// @brief State tag: failed.
struct Failed {};

/// @brief Event: start an attempt.
struct Start {
  /// @brief The attempt's id; must be nonzero.
  std::uint64_t attempt_id{};
};
/// @brief Event: a control message was received and decoded.
struct Received {
  /// @brief The attempt it arrived on.
  std::uint64_t attempt_id{};
  /// @brief The message; actions may move parts of it into effects.
  CtrlMessage& message;
};
/// @brief Event: the owner wants to stop.
struct Stop {
  /// @brief The attempt to stop.
  std::uint64_t attempt_id{};
};
/// @brief Event: the transport failed.
struct Failure {
  /// @brief The attempt that failed.
  std::uint64_t attempt_id{};
  /// @brief Why.
  FailureReason reason{};
};
/// @brief Event: the owner closed the transport after Cleanup.
struct Closed {
  /// @brief The attempt that closed.
  std::uint64_t attempt_id{};
  /// @brief Whether the close itself succeeded.
  bool success{true};
};

/// @brief Effect: hand the console's session id to the stream.
struct PublishSessionId {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief The id.
  SessionId value;
};
/// @brief Effect: the console's session-id message was unusable; the owner
/// synthesises an id. The machine keeps waiting for a usable one.
struct PublishSessionIdFallback {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief Why the message was refused.
  SessionIdError reason;
};
/// @brief Effect: ask the user for the console login passcode.
struct RequestPin {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief Whether the console refused a previous passcode.
  bool incorrect{};
};
/// @brief Effect: answer the console's heartbeat.
struct SendHeartbeatReply {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Effect: the console confirmed the switch to the data route (PSN
/// sessions); published once per attempt.
struct PublishSwitchAck {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Effect: close the transport, then report Closed.
struct Cleanup {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Effect: the attempt stopped cleanly.
struct PublishStopped {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Effect: the attempt failed.
struct PublishFailed {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief Why; transport when the close failed with no earlier reason.
  FailureReason reason;
};
/// @brief Effect: pass a console notice on to the stream or the couch seats.
struct PublishNotice {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
  /// @brief The notice.
  CtrlNotice notice;
};
/// @brief Effect: the console resent a session id while active; send the
/// post-id announcements again.
struct RepeatPostIdFeatures {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};
/// @brief Effect: the console asked for a passcode after the session id,
/// which the session treats as a control failure.
struct PublishCtrlFailed {
  /// @brief The attempt it belongs to.
  std::uint64_t attempt_id{};
};

/// @brief Something the owner must do, produced by a transition.
using Effect =
    std::variant<PublishSessionId, PublishSessionIdFallback, RequestPin,
                 SendHeartbeatReply, PublishSwitchAck, Cleanup, PublishStopped,
                 PublishFailed, PublishNotice, RepeatPostIdFeatures,
                 PublishCtrlFailed>;

/// @brief The machine's data, owned by the owner and shared with it.
struct Context {
  /// @brief Effects not yet taken, oldest first.
  std::deque<Effect> effects;
  /// @brief The running attempt; cleared once Stopped or Failed is published.
  std::optional<std::uint64_t> active_attempt_id;
  /// @brief The first failure recorded while stopping.
  std::optional<FailureReason> pending_failure;
  /// @brief Whether a passcode was asked for and not yet accepted.
  bool pin_requested{false};
  /// @brief Whether the switch confirmation was already published.
  bool switch_ack_published{false};
  /// @brief Events refused because they named another attempt.
  std::size_t stale_events{};
  /// @brief Events the machine had no transition for.
  std::size_t unexpected_events{};
};

/// @brief The transition table.
struct Fsm {
  /// @brief Builds the table.
  ///
  /// @return The boost::sml table.
  auto operator()() const noexcept {
    using namespace sml;
    const auto is_id = [](const Received& e) {
      return std::holds_alternative<CtrlSessionId>(e.message);
    };
    const auto is_fallback = [](const Received& e) {
      return std::holds_alternative<CtrlSessionIdFallback>(e.message);
    };
    const auto is_pin = [](const Received& e) {
      return std::holds_alternative<CtrlLoginPinRequest>(e.message);
    };
    const auto is_other = [is_id, is_fallback, is_pin](const Received& e) {
      return !is_id(e) && !is_fallback(e) && !is_pin(e);
    };
    const auto begin = [](const Start& e, Context& c) {
      c.active_attempt_id = e.attempt_id;
      c.pending_failure.reset();
      c.pin_requested = false;
      c.switch_ack_published = false;
    };
    const auto publish_id = [](const Received& e, Context& c) {
      if (auto* id = std::get_if<CtrlSessionId>(&e.message))
        c.effects.emplace_back(
            PublishSessionId{e.attempt_id, std::move(id->value)});
      else
        c.effects.emplace_back(PublishSessionIdFallback{
            e.attempt_id, std::get<CtrlSessionIdFallback>(e.message).reason});
    };
    const auto request_pin = [](const Received& e, Context& c) {
      c.pin_requested = true;
      c.effects.emplace_back(RequestPin{e.attempt_id, false});
    };
    const auto handle_other = [](const Received& e, Context& c) {
      if (std::holds_alternative<CtrlHeartbeatRequest>(e.message))
        c.effects.emplace_back(SendHeartbeatReply{e.attempt_id});
      else if (std::holds_alternative<CtrlLoginSuccess>(e.message))
        c.pin_requested = false;
      else if (std::holds_alternative<CtrlLoginPinIncorrect>(e.message) &&
               c.pin_requested)
        c.effects.emplace_back(RequestPin{e.attempt_id, true});
      else if (std::holds_alternative<CtrlStreamSwitchAck>(e.message) &&
               !c.switch_ack_published) {
        c.switch_ack_published = true;
        c.effects.emplace_back(PublishSwitchAck{e.attempt_id});
      } else if (std::holds_alternative<CtrlSessionId>(e.message) ||
                 std::holds_alternative<CtrlSessionIdFallback>(e.message))
        c.effects.emplace_back(RepeatPostIdFeatures{e.attempt_id});
      else if (auto* notice = std::get_if<CtrlNoticeMessage>(&e.message))
        c.effects.emplace_back(
            PublishNotice{e.attempt_id, std::move(notice->notice)});
    };
    const auto publish_ctrl_failed = [](const Received& e, Context& c) {
      c.effects.emplace_back(PublishCtrlFailed{e.attempt_id});
    };
    const auto stop = [](const Stop& e, Context& c) {
      c.effects.emplace_back(Cleanup{e.attempt_id});
    };
    const auto fail = [](const Failure& e, Context& c) {
      c.pending_failure = e.reason;
      c.effects.emplace_back(Cleanup{e.attempt_id});
    };
    const auto record_close_failure = [](const Failure& e, Context& c) {
      if (!c.pending_failure) c.pending_failure = e.reason;
    };
    const auto closed_ok = [](const Closed& e, const Context& c) {
      return e.success && !c.pending_failure;
    };
    const auto closed_failed = [](const Closed& e, const Context& c) {
      return !e.success || c.pending_failure.has_value();
    };
    const auto publish_stopped = [](const Closed& e, Context& c) {
      c.effects.emplace_back(PublishStopped{e.attempt_id});
      c.active_attempt_id.reset();
    };
    const auto publish_failed = [](const Closed& e, Context& c) {
      c.effects.emplace_back(PublishFailed{
          e.attempt_id, c.pending_failure.value_or(FailureReason::transport)});
      c.active_attempt_id.reset();
    };

    return make_transition_table(
        *state<Idle> + sml::event<Start> / begin = state<WaitingId>,
        state<WaitingId> + sml::event<Received>[is_id] / publish_id =
            state<Active>,
        state<WaitingId> + sml::event<Received>[is_fallback] / publish_id,
        state<WaitingId> + sml::event<Received>[is_pin] / request_pin,
        state<WaitingId> + sml::event<Received>[is_other] / handle_other,
        state<Active> + sml::event<Received>[is_pin] / publish_ctrl_failed,
        state<Active> + sml::event<Received>[!is_pin] / handle_other,
        state<WaitingId> + sml::event<Stop> / stop = state<Closing>,
        state<Active> + sml::event<Stop> / stop = state<Closing>,
        state<WaitingId> + sml::event<Failure> / fail = state<Closing>,
        state<Active> + sml::event<Failure> / fail = state<Closing>,
        state<Closing> + sml::event<Failure> / record_close_failure,
        state<Closing> + sml::event<Closed>[closed_ok] / publish_stopped =
            state<Stopped>,
        state<Closing> + sml::event<Closed>[closed_failed] / publish_failed =
            state<Failed>);
  }
};

/// @brief Runs the machine for its owner.
///
/// Must be used only on the thread that made it (checked in debug builds).
class Lifecycle {
 public:
  /// @brief Makes an idle machine over a context.
  ///
  /// @param context The context; must outlive the machine.
  explicit Lifecycle(Context& context) : context_(context), machine_(context) {}
  /// @brief Checks, in debug builds, that it dies on its owning thread.
  ~Lifecycle() { assert_owner(); }
  Lifecycle(const Lifecycle&) = delete;
  Lifecycle& operator=(const Lifecycle&) = delete;

  /// @brief Starts an attempt.
  ///
  /// @param event The start event.
  /// @return False (and counted) when the id is 0, an attempt is running, or
  ///     the machine refuses.
  [[nodiscard]] bool process(const Start& event) {
    assert_owner();
    if (event.attempt_id == 0 || context_.active_attempt_id)
      return unexpected();
    return dispatch(event);
  }

  /// @brief Feeds an event of the running attempt.
  ///
  /// @tparam Event Any event other than Start.
  /// @param event The event.
  /// @return False when it names another attempt (counted stale) or the
  ///     machine refuses it (counted unexpected).
  template <class Event>
  [[nodiscard]] bool process(const Event& event) {
    assert_owner();
    if (!context_.active_attempt_id ||
        *context_.active_attempt_id != event.attempt_id) {
      ++context_.stale_events;
      return false;
    }
    return dispatch(event);
  }

  /// @brief The current phase.
  ///
  /// @return The state.
  State state() const noexcept {
    assert_owner();
    if (machine_.is(sml::state<Idle>)) return State::idle;
    if (machine_.is(sml::state<WaitingId>)) return State::waiting_id;
    if (machine_.is(sml::state<Active>)) return State::active;
    if (machine_.is(sml::state<Closing>)) return State::closing;
    if (machine_.is(sml::state<Stopped>)) return State::stopped;
    return State::failed;
  }

 private:
  /// @brief Asserts, in debug builds, that the caller is the owning thread.
  void assert_owner() const noexcept {
#ifndef NDEBUG
    assert(platform::ThreadIdentity::current() == owner_);
#endif
  }
  /// @brief Counts an unexpected event.
  ///
  /// @return False.
  bool unexpected() {
    ++context_.unexpected_events;
    return false;
  }
  /// @brief Feeds an event to the machine.
  ///
  /// @tparam Event The event type.
  /// @param event The event.
  /// @return True when the machine took it.
  template <class Event>
  [[nodiscard]] bool dispatch(const Event& event) {
    if (!machine_.process_event(event)) return unexpected();
    return true;
  }

  /// @brief The context.
  Context& context_;
  /// @brief The machine.
  sml::sm<Fsm> machine_;
  /// @brief The thread that made the machine.
  [[maybe_unused]] const platform::ThreadIdentity owner_{
      platform::ThreadIdentity::current()};
};

}  // namespace libchiaking::net::protocol::remote_play::ctrl_lifecycle_detail
