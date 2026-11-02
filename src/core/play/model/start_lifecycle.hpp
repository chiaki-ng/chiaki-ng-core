/// @file
/// @brief Declares StartLifecycle, where a start stands for StartPolicy:
/// starting, reconnecting after a drop or given up, and whether the console
/// was already woken.
#pragma once

#include <boost/sml.hpp>

namespace libchiaking::play::start_lifecycle {

/// @brief Boost.SML, which the machine is written in.
namespace sml = boost::sml;

/// @brief Phase state: the first attempts of the start. The initial phase.
struct Starting {};
/// @brief Phase state: a run of reconnections after an attempt that had
/// streamed dropped.
struct Reconnecting {};
/// @brief Phase state: the policy gave up; final.
struct Finished {};
/// @brief Wake state: the console has not been woken. The initial wake
/// state.
struct Unwoken {};
/// @brief Wake state: the console was woken during this session.
struct Woken {};

/// @brief Event: a run of reconnections starts, or starts over.
struct Reconnect {};
/// @brief Event: the policy gives up.
struct GiveUp {};
/// @brief Event: the console is being woken.
struct Wake {};

/// @brief The transition table of a start.
///
/// Two orthogonal regions. The phase goes from Starting to Reconnecting on
/// Reconnect, which Reconnecting accepts again when a run starts over, and
/// from either to Finished on GiveUp, which Finished accepts again. The wake
/// region leaves Unwoken on Wake and accepts later Wakes.
struct StartFsm {
  /// @brief Builds the transition table for Boost.SML.
  /// @return The table of both regions.
  auto operator()() const noexcept {
    using namespace sml;
    const auto unchanged = [] {};
    return make_transition_table(
        *state<Starting> + sml::event<Reconnect> = state<Reconnecting>,
        state<Reconnecting> + sml::event<Reconnect> / unchanged,
        state<Starting> + sml::event<GiveUp> = state<Finished>,
        state<Reconnecting> + sml::event<GiveUp> = state<Finished>,
        state<Finished> + sml::event<GiveUp> / unchanged,

        *state<Unwoken> + sml::event<Wake> = state<Woken>,
        state<Woken> + sml::event<Wake> / unchanged);
  }
};

}  // namespace libchiaking::play::start_lifecycle

namespace libchiaking::play {

/// @brief Where a start stands, for one StartPolicy.
///
/// A value type; not thread-safe.
class StartLifecycle {
 public:
  /// @brief Starts or restarts a run of reconnections.
  void reconnect() noexcept { apply(start_lifecycle::Reconnect{}); }
  /// @brief Gives up.
  void give_up() noexcept { apply(start_lifecycle::GiveUp{}); }
  /// @brief Records that the console is being woken.
  void wake() noexcept { apply(start_lifecycle::Wake{}); }

  /// @brief Tells whether a run of reconnections is under way.
  /// @return True while reconnecting.
  auto reconnecting() const noexcept -> bool {
    return in<start_lifecycle::Reconnecting>();
  }
  /// @brief Tells whether the policy gave up.
  /// @return True once finished.
  auto finished() const noexcept -> bool {
    return in<start_lifecycle::Finished>();
  }
  /// @brief Tells whether the console was already woken.
  /// @return True once woken.
  auto woken() const noexcept -> bool { return in<start_lifecycle::Woken>(); }

 private:
  /// @brief Tells whether some region is in a state.
  /// @tparam State One of the start states.
  /// @return True when it is.
  template <class State>
  auto in() const noexcept -> bool {
    return machine_.is(start_lifecycle::sml::state<State>);
  }
  /// @brief Feeds an event; one the table does not allow changes nothing.
  /// @tparam Event The event type.
  /// @param event The event.
  template <class Event>
  void apply(const Event& event) noexcept {
    static_cast<void>(machine_.process_event(event));
  }

  /// @brief The SML machine.
  start_lifecycle::sml::sm<start_lifecycle::StartFsm> machine_;
};

}  // namespace libchiaking::play
