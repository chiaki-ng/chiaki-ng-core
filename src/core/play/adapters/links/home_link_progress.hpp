/// @file
/// @brief Declares home_link_progress::Progress, the Boost.SML machine that
/// orders a home link's start: session request, control channel, session id,
/// calibration and the active session, with the login prompt and control
/// failures alongside.
#pragma once

#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/links/link_failure.hpp"

#include <boost/sml.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <variant>

namespace libchiaking::play::home_link_progress {

/// @brief Where a home link's start stands, as the link reads it.
enum class Stage : std::uint8_t {
  /// @brief Nothing was asked yet.
  idle,
  /// @brief The session request was sent and waits for the console.
  requesting_session,
  /// @brief The console accepted; the control channel is being opened.
  awaiting_control,
  /// @brief The control channel is open and the session id is awaited.
  awaiting_session_id,
  /// @brief The session id is known; calibration is next.
  session_id_known,
  /// @brief The connection is being measured.
  calibrating,
  /// @brief The active session runs.
  active,
  /// @brief The route was released; final.
  released,
};

/// @brief A control channel failure: the stream error and its detail.
struct ControlFailure {
  /// @brief The stream error.
  net::protocol::remote_play::StreamError error;
  /// @brief The failure's detail; filled with a control receive failure when
  /// deferred without one.
  std::optional<LinkFailure> detail;
};

/// @brief Start state: nothing asked. The initial state.
struct Idle {};
/// @brief Start state: the session request is out.
struct RequestingSession {};
/// @brief Start state: the control channel is being opened.
struct AwaitingControl {};
/// @brief Start state: the session id is awaited.
struct AwaitingSessionId {};
/// @brief Start state: the session id is known.
struct SessionIdKnown {};
/// @brief Start state: calibrating.
struct Calibrating {};
/// @brief Start state: the active session runs.
struct Active {};
/// @brief Start state: the route was released.
struct Released {};

/// @brief Login prompt state: the console has not asked for a login code.
/// The initial state.
struct PinNotAsked {};
/// @brief Login prompt state: the console asked and the player has not
/// answered.
struct PinAsked {};
/// @brief Login prompt state: the code was sent.
struct PinAnswered {};

/// @brief Control answer state: the control channel has not answered. The
/// initial state.
struct ControlStarting {};
/// @brief Control answer state: a session id was made up before the control
/// channel existed and must be published to it once it answers.
struct ControlOwesSessionId {};
/// @brief Control answer state: the control channel answered.
struct ControlAnswered {};

/// @brief Event: the session request was sent.
struct SessionRequested {};
/// @brief Event: the console accepted the session request.
struct SessionAccepted {};
/// @brief Event: the control channel opened.
struct ControlOpened {};
/// @brief Event: the session id arrived or was made up.
struct SessionIdReceived {};
/// @brief Event: a session id was made up before the control channel
/// existed.
struct SessionIdWithoutControl {};
/// @brief Event: the control channel answered its startup.
struct ControlAnswer {};
/// @brief Event: calibration started.
struct CalibrationStarted {};
/// @brief Event: the active session started.
struct ActiveSessionStarted {};
/// @brief Event: the control channel failed.
struct ControlFailed {
  /// @brief The failure.
  ControlFailure failure;
};
/// @brief Event: the console asked for a login code.
struct PinRequested {};
/// @brief Event: the login code was sent.
struct PinSent {};
/// @brief Event: the link released its route.
struct RouteReleased {};

/// @brief Effect: fail the stream with this control failure now.
struct EndStream {
  /// @brief The failure.
  ControlFailure failure;
};
/// @brief Effect: fail the stream with a control failure that was deferred
/// until the active session started.
struct EndStreamForEarlierFailure {
  /// @brief The failure.
  ControlFailure failure;
};
/// @brief Effect: log that a control failure was deferred.
struct NoteDeferredFailure {};
/// @brief Effect: ask the player for the login code.
struct AskForPin {
  /// @brief Whether the console asked again after a code was sent.
  bool retry{};
};
/// @brief Effect: publish the made-up session id to the control channel.
struct PublishLateSessionId {};

/// @brief Something the link must do because of a transition.
using Effect =
    std::variant<EndStream, EndStreamForEarlierFailure, NoteDeferredFailure,
                 AskForPin, PublishLateSessionId>;

/// @brief What the machine's actions write: effects for the link, the
/// deferred failure, and how many events did not apply.
struct Context {
  /// @brief Effects not yet taken, oldest first.
  std::deque<Effect> effects;
  /// @brief The first control failure met during the session id or
  /// calibration stages.
  std::optional<ControlFailure> deferred;
  /// @brief Events processed in a state that does not take them.
  std::size_t unexpected_events{};
};

/// @brief The transition table of a home link's start.
///
/// Three regions run side by side. The start region walks from Idle to
/// Active; a control failure ends the stream at once while the session id is
/// awaited, is deferred (the first only) once it is known or while
/// calibrating, and is ignored once active; the deferred one ends the stream
/// when the active session starts. Releasing the route from any start state
/// clears every effect and the deferral. The login prompt region asks once,
/// then again only after a code was sent. The control answer region notes a
/// session id made up before the control channel, to publish it when the
/// channel answers. Stateless.
struct Machine {
  /// @brief Builds the transition table for Boost.SML.
  ///
  /// @return The table of all three regions.
  auto operator()() const noexcept {
    using namespace boost::sml;

    const auto end_stream = [](const ControlFailed& event, Context& context) {
      context.effects.emplace_back(EndStream{event.failure});
    };
    const auto defer_first = [](const ControlFailed& event, Context& context) {
      if (context.deferred) return;
      auto failure = event.failure;
      if (!failure.detail)
        failure.detail =
            LinkFailure{net::protocol::remote_play::FailureStage::ctrl_receive,
                        failure.error};
      context.deferred.emplace(std::move(failure));
      context.effects.emplace_back(NoteDeferredFailure{});
    };
    const auto report_deferred = [](Context& context) {
      if (auto failure = std::exchange(context.deferred, std::nullopt))
        context.effects.emplace_back(
            EndStreamForEarlierFailure{std::move(*failure)});
    };
    const auto ask_first = [](Context& context) {
      context.effects.emplace_back(AskForPin{false});
    };
    const auto ask_again = [](Context& context) {
      context.effects.emplace_back(AskForPin{true});
    };
    const auto publish_late = [](Context& context) {
      context.effects.emplace_back(PublishLateSessionId{});
    };
    const auto unchanged = [] {};
    const auto forget = [](Context& context) {
      context.effects.clear();
      context.deferred.reset();
    };

    return make_transition_table(
        *state<Idle> + event<SessionRequested> = state<RequestingSession>,
        state<RequestingSession> + event<SessionAccepted> =
            state<AwaitingControl>,
        state<AwaitingControl> + event<ControlOpened> =
            state<AwaitingSessionId>,
        state<AwaitingSessionId> + event<SessionIdReceived> =
            state<SessionIdKnown>,
        state<SessionIdKnown> + event<CalibrationStarted> = state<Calibrating>,
        state<Calibrating> + event<ActiveSessionStarted> / report_deferred =
            state<Active>,

        state<AwaitingSessionId> + event<ControlFailed> / end_stream,
        state<SessionIdKnown> + event<ControlFailed> / defer_first,
        state<Calibrating> + event<ControlFailed> / defer_first,
        state<Active> + event<ControlFailed> / unchanged,

        state<Idle> + event<RouteReleased> / forget = state<Released>,
        state<RequestingSession> + event<RouteReleased> / forget =
            state<Released>,
        state<AwaitingControl> + event<RouteReleased> / forget =
            state<Released>,
        state<AwaitingSessionId> + event<RouteReleased> / forget =
            state<Released>,
        state<SessionIdKnown> + event<RouteReleased> / forget = state<Released>,
        state<Calibrating> + event<RouteReleased> / forget = state<Released>,
        state<Active> + event<RouteReleased> / forget = state<Released>,
        state<Released> + event<RouteReleased> / unchanged,

        *state<PinNotAsked> + event<PinRequested> / ask_first = state<PinAsked>,
        state<PinAsked> + event<PinRequested> / unchanged,
        state<PinAsked> + event<PinSent> = state<PinAnswered>,
        state<PinAnswered> + event<PinRequested> / ask_again = state<PinAsked>,

        *state<ControlStarting> + event<SessionIdWithoutControl> =
            state<ControlOwesSessionId>,
        state<ControlStarting> + event<ControlAnswer> = state<ControlAnswered>,
        state<ControlOwesSessionId> + event<ControlAnswer> / publish_late =
            state<ControlAnswered>);
  }
};

/// @brief The progress of one home link's start and the effects it asks for.
///
/// Owned by the home link and used on its thread; not synchronised. Not
/// copyable.
class Progress {
 public:
  /// @brief Builds the machine in its initial states with no effects.
  Progress() : machine_(context_) {}
  /// @brief Not copyable; the machine refers to its own context.
  Progress(const Progress&) = delete;
  /// @brief Not copy-assignable; the machine refers to its own context.
  auto operator=(const Progress&) -> Progress& = delete;

  /// @brief Feeds an event to the machine.
  ///
  /// @tparam Event One of the event types above.
  /// @param event The event.
  /// @return `true` when a transition took it; `false`, counted, when no state
  ///     takes it.
  template <class Event>
  auto process(const Event& event) -> bool {
    if (machine_.process_event(event)) return true;
    ++context_.unexpected_events;
    return false;
  }

  /// @brief Takes the oldest effect.
  ///
  /// @return The effect; empty when none waits.
  auto take_effect() -> std::optional<Effect> {
    if (context_.effects.empty()) return std::nullopt;
    auto effect = std::move(context_.effects.front());
    context_.effects.pop_front();
    return effect;
  }

  /// @brief Takes the oldest effect of one kind, leaving the others.
  ///
  /// @tparam Wanted One of the Effect alternatives.
  /// @return The effect; empty when none of that kind waits.
  template <class Wanted>
  auto take() -> std::optional<Wanted> {
    const auto found =
        std::ranges::find_if(context_.effects, [](const Effect& effect) {
          return std::holds_alternative<Wanted>(effect);
        });
    if (found == context_.effects.end()) return std::nullopt;
    auto wanted = std::get<Wanted>(std::move(*found));
    context_.effects.erase(found);
    return wanted;
  }

  /// @brief Reads the start stage.
  ///
  /// @return The stage of the start region.
  auto stage() const noexcept -> Stage {
    using boost::sml::state;
    if (machine_.is(state<RequestingSession>)) return Stage::requesting_session;
    if (machine_.is(state<AwaitingControl>)) return Stage::awaiting_control;
    if (machine_.is(state<AwaitingSessionId>))
      return Stage::awaiting_session_id;
    if (machine_.is(state<SessionIdKnown>)) return Stage::session_id_known;
    if (machine_.is(state<Calibrating>)) return Stage::calibrating;
    if (machine_.is(state<Active>)) return Stage::active;
    if (machine_.is(state<Released>)) return Stage::released;
    return Stage::idle;
  }

  /// @brief Reports whether the session id is known.
  ///
  /// @return `true` once known, through calibration and the active session.
  auto knows_session_id() const noexcept -> bool {
    using boost::sml::state;
    return machine_.is(state<SessionIdKnown>) ||
           machine_.is(state<Calibrating>) || machine_.is(state<Active>);
  }

  /// @brief Reports whether the player is being asked for a login code.
  ///
  /// @return `true` from the request until the code is sent.
  auto asking_for_pin() const noexcept -> bool {
    return machine_.is(boost::sml::state<PinAsked>);
  }

  /// @brief Reports whether the control channel answered.
  ///
  /// @return `true` once answered.
  auto control_answered() const noexcept -> bool {
    return machine_.is(boost::sml::state<ControlAnswered>);
  }

  /// @brief Counts the events no state took.
  ///
  /// @return The count.
  auto unexpected_events() const noexcept -> std::size_t {
    return context_.unexpected_events;
  }

 private:
  /// @brief What the machine's actions write.
  Context context_;
  /// @brief The Boost.SML machine over the context.
  boost::sml::sm<Machine> machine_;
};

}  // namespace libchiaking::play::home_link_progress
