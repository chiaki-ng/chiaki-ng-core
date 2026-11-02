/// @file
/// @brief Declares TwoSignalBarrier, a state machine that is ready once two
/// distinct signals have both arrived, in either order, unless it was stopped
/// first.
#pragma once
#include <boost/sml.hpp>

namespace libchiaking::psn::remote_play {
/// @brief The events, states and transition table of TwoSignalBarrier.
namespace two_signal_detail {
/// @brief The first signal arrived.
struct First {};
/// @brief The second signal arrived.
struct Second {};
/// @brief The wait was abandoned.
struct Stop {};
/// @brief Neither signal has arrived.
struct Empty {};
/// @brief Only the first signal has arrived.
struct HaveFirst {};
/// @brief Only the second signal has arrived.
struct HaveSecond {};
/// @brief Both signals have arrived; final.
struct Ready {};
/// @brief Stopped before both arrived; final.
struct Failed {};
/// @brief The transition table.
struct Definition {
  /// @brief Builds the table: a repeated signal changes nothing, a stop
  /// before both fails, and anything after Ready or Failed is ignored.
  ///
  /// @return The boost::sml transition table.
  auto operator()() const {
    using namespace boost::sml;
    return make_transition_table(
        *state<Empty> + event<First> = state<HaveFirst>,
        state<Empty> + event<Second> = state<HaveSecond>,
        state<HaveFirst> + event<Second> = state<Ready>,
        state<HaveSecond> + event<First> = state<Ready>,
        state<HaveFirst> + event<First> = state<HaveFirst>,
        state<HaveSecond> + event<Second> = state<HaveSecond>,
        state<Empty> + event<Stop> = state<Failed>,
        state<HaveFirst> + event<Stop> = state<Failed>,
        state<HaveSecond> + event<Stop> = state<two_signal_detail::Failed>);
  }
};
}  // namespace two_signal_detail
/// @brief Two signals awaited together. Not thread-safe; owned by a
/// SignalBarrier.
class TwoSignalBarrier {
 public:
  /// @brief Records the first signal.
  void first() { machine_.process_event(two_signal_detail::First{}); }
  /// @brief Records the second signal.
  void second() { machine_.process_event(two_signal_detail::Second{}); }
  /// @brief Abandons the wait, unless it is already ready.
  void stop() { machine_.process_event(two_signal_detail::Stop{}); }
  /// @brief Tells whether both signals arrived.
  ///
  /// @return True once ready.
  bool ready() const {
    return machine_.is(boost::sml::state<two_signal_detail::Ready>);
  }
  /// @brief Tells whether the wait was abandoned first.
  ///
  /// @return True once failed.
  bool failed() const {
    return machine_.is(boost::sml::state<two_signal_detail::Failed>);
  }

 private:
  /// @brief The state machine.
  boost::sml::sm<two_signal_detail::Definition> machine_;
};
}  // namespace libchiaking::psn::remote_play
