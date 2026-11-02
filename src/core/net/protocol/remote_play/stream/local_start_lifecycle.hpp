/// @file
/// @brief The state machine of a stream pipeline from calibration to the
/// active session: which route to fall back to when Senkusha fails, and how
/// a stop unwinds each phase.
#pragma once

#include <boost/sml.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace libchiaking::net::protocol::remote_play {

/// @brief The pipeline's phase as its owner sees it.
enum class LocalPipelineState : std::uint8_t {
  /// @brief Calibrating the route, or opening the route the stream will use.
  calibrating,
  /// @brief The active session is running.
  active,
  /// @brief Stopping or stopped on request, or closed by the console.
  stopped,
  /// @brief Failed; nothing more will happen.
  failed,
};

}  // namespace libchiaking::net::protocol::remote_play

namespace libchiaking::net::protocol::remote_play::local_pipeline_lifecycle {
namespace sml = boost::sml;

/// @brief State tag: Senkusha is calibrating.
struct Calibrating {};
/// @brief State tag: opening the route and starting the active session.
struct OpeningRoute {};
/// @brief State tag: the active session runs.
struct Active {};
/// @brief State tag: releasing on request.
struct Stopping {};
/// @brief State tag: stopped.
struct Stopped {};
/// @brief State tag: failed.
struct Failed {};

/// @brief Event: calibration finished with a route.
struct CalibrationSucceeded {};
/// @brief Event: calibration failed.
struct CalibrationFailed {
  /// @brief Whether it failed because it was cancelled.
  bool cancelled{};
  /// @brief Whether Senkusha handed its route back.
  bool route_retained{};
};
/// @brief Event: the calibration client is gone.
struct CalibrationLost {};
/// @brief Event: calibration was skipped for a reference fallback.
struct CalibrationSkipped {};
/// @brief Event: the active session started.
struct ActiveStarted {};
/// @brief Event: opening the route or starting the session failed.
struct OpeningFailed {};
/// @brief Event: the active session failed.
struct ActiveFailed {};
/// @brief Event: the active session closed.
struct ActiveClosed {};
/// @brief Event: the owner asked to stop.
struct StopRequested {};
/// @brief Event: everything was released.
struct Released {};
/// @brief Event: releasing failed.
struct ReleaseFailed {};

/// @brief Effect: run the stream on the route Senkusha handed back, with
/// the retained calibration.
struct OpenRetainedRoute {};
/// @brief Effect: open a fresh route with the reference calibration.
struct OpenFreshRoute {};
/// @brief Effect: cancel Senkusha.
struct CancelCalibration {};
/// @brief Effect: stop the active session.
struct StopActive {};
/// @brief Effect: drop everything without stopping cleanly.
struct DiscardAll {};

/// @brief What the owner must do after an event.
using Effect = std::variant<OpenRetainedRoute, OpenFreshRoute,
                            CancelCalibration, StopActive, DiscardAll>;

/// @brief The machine's data.
struct Context {
  /// @brief Whether a route Senkusha hands back may be used after a failure.
  bool retained_route_fallback{};
  /// @brief Whether a fresh route may be opened after a failure.
  bool fresh_route_fallback{};
  /// @brief The effect of the last event.
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
    const auto retained_route_usable = [](const CalibrationFailed& e,
                                          const Context& c) {
      return !e.cancelled && e.route_retained && c.retained_route_fallback;
    };
    const auto fresh_route_allowed = [](const CalibrationFailed& e,
                                        const Context& c) {
      return !e.cancelled && c.fresh_route_fallback;
    };
    const auto open_retained_route = [](Context& c) {
      c.effect = OpenRetainedRoute{};
    };
    const auto open_fresh_route = [](Context& c) {
      c.effect = OpenFreshRoute{};
    };
    const auto cancel_calibration = [](Context& c) {
      c.effect = CancelCalibration{};
    };
    const auto stop_active = [](Context& c) { c.effect = StopActive{}; };
    const auto discard_all = [](Context& c) { c.effect = DiscardAll{}; };

    return make_transition_table(
        *state<Calibrating> + sml::event<CalibrationSucceeded> =
            state<OpeningRoute>,
        state<Calibrating> +
            sml::event<CalibrationFailed>[retained_route_usable] /
                open_retained_route = state<OpeningRoute>,
        state<Calibrating> +
            sml::event<CalibrationFailed>[fresh_route_allowed] /
                open_fresh_route = state<OpeningRoute>,
        state<Calibrating> + sml::event<CalibrationFailed> = state<Failed>,
        state<Calibrating> + sml::event<CalibrationLost> = state<Failed>,
        state<Calibrating> + sml::event<CalibrationSkipped> =
            state<OpeningRoute>,
        state<Calibrating> + sml::event<StopRequested> / cancel_calibration =
            state<Stopping>,

        state<OpeningRoute> + sml::event<ActiveStarted> = state<Active>,
        state<OpeningRoute> + sml::event<OpeningFailed> = state<Failed>,

        state<Active> + sml::event<ActiveFailed> = state<Failed>,
        state<Active> + sml::event<ActiveClosed> = state<Stopped>,
        state<Active> + sml::event<StopRequested> / stop_active =
            state<Stopping>,

        state<Stopping> + sml::event<Released> = state<Stopped>,
        state<Stopping> + sml::event<ReleaseFailed> = state<Failed>,

        state<Failed> + sml::event<StopRequested> / discard_all,
        state<Stopped> + sml::event<StopRequested> = state<Stopped>);
  }
};

/// @brief The pipeline's state machine with its context.
///
/// Not thread-safe; the pipeline drives it.
class Lifecycle {
 public:
  /// @brief Makes a machine in the calibrating state.
  ///
  /// @param retained_route_fallback Whether a handed-back route may be used.
  /// @param fresh_route_fallback Whether a fresh route may be opened.
  Lifecycle(bool retained_route_fallback, bool fresh_route_fallback)
      : context_{.retained_route_fallback = retained_route_fallback,
                 .fresh_route_fallback = fresh_route_fallback},
        machine_(context_) {}
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
  LocalPipelineState state() const noexcept {
    if (machine_.is(sml::state<Calibrating>) ||
        machine_.is(sml::state<OpeningRoute>))
      return LocalPipelineState::calibrating;
    if (machine_.is(sml::state<Active>)) return LocalPipelineState::active;
    if (machine_.is(sml::state<Stopping>) || machine_.is(sml::state<Stopped>))
      return LocalPipelineState::stopped;
    return LocalPipelineState::failed;
  }

 private:
  /// @brief The context.
  Context context_;
  /// @brief The machine.
  sml::sm<Fsm> machine_;
};

}  // namespace
   // libchiaking::net::protocol::remote_play::local_pipeline_lifecycle
