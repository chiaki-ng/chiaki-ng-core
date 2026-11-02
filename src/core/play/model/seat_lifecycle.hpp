/// @file
/// @brief Declares SeatLifecycle, where one pad's seat stands on the
/// console's side of the couch protocol.
#pragma once

#include <boost/sml.hpp>

namespace libchiaking::play::seat_lifecycle {

/// @brief Boost.SML, which the machine is written in.
namespace sml = boost::sml;

/// @brief Nobody is in the seat. The initial state.
struct Empty {};
/// @brief A join was sent and the console has not answered.
struct JoinRequested {};
/// @brief The console asked for the seat's passcode.
struct EnteringPasscode {};
/// @brief The pad is admitted and streams input.
struct Joined {};

/// @brief Event: a join is sent, first or after a passcode.
struct RequestJoin {};
/// @brief Event: the console asked for the passcode.
struct AskPasscode {};
/// @brief Event: the pad is admitted.
struct Admit {};

/// @brief The transition table of a seat.
///
/// RequestJoin leads to JoinRequested from Empty or EnteringPasscode, and is
/// accepted again while requested. AskPasscode moves a requested join to
/// EnteringPasscode. Admit leads to Joined from every state. A seat is
/// emptied by replacing it with a fresh one.
struct SeatFsm {
  /// @brief Builds the transition table for Boost.SML.
  /// @return The table.
  auto operator()() const noexcept {
    using namespace sml;
    const auto unchanged = [] {};
    return make_transition_table(
        *state<Empty> + sml::event<RequestJoin> = state<JoinRequested>,
        state<EnteringPasscode> + sml::event<RequestJoin> =
            state<JoinRequested>,
        state<JoinRequested> + sml::event<RequestJoin> / unchanged,
        state<JoinRequested> + sml::event<AskPasscode> =
            state<EnteringPasscode>,
        state<Empty> + sml::event<Admit> = state<Joined>,
        state<JoinRequested> + sml::event<Admit> = state<Joined>,
        state<EnteringPasscode> + sml::event<Admit> = state<Joined>,
        state<Joined> + sml::event<Admit> / unchanged);
  }
};

}  // namespace libchiaking::play::seat_lifecycle

namespace libchiaking::play {

/// @brief Where one pad's seat stands on the console's side.
///
/// A value type held per pad; not thread-safe. Events the table does not
/// allow change nothing.
class SeatLifecycle {
 public:
  /// @brief Records that a join was sent.
  void request_join() noexcept { apply(seat_lifecycle::RequestJoin{}); }
  /// @brief Records that the console asked for the passcode.
  void ask_passcode() noexcept { apply(seat_lifecycle::AskPasscode{}); }
  /// @brief Records that the pad was admitted.
  void admit() noexcept { apply(seat_lifecycle::Admit{}); }

  /// @brief Tells whether a join awaits the console's answer.
  /// @return True while requested.
  auto join_requested() const noexcept -> bool {
    return in<seat_lifecycle::JoinRequested>();
  }
  /// @brief Tells whether the console waits for the passcode.
  /// @return True while entering the passcode.
  auto entering_passcode() const noexcept -> bool {
    return in<seat_lifecycle::EnteringPasscode>();
  }
  /// @brief Tells whether the pad is admitted.
  /// @return True once joined.
  auto joined() const noexcept -> bool { return in<seat_lifecycle::Joined>(); }

 private:
  /// @brief Tells whether the machine is in a state.
  /// @tparam State One of the seat states.
  /// @return True when it is.
  template <class State>
  auto in() const noexcept -> bool {
    return machine_.is(seat_lifecycle::sml::state<State>);
  }
  /// @brief Feeds an event.
  /// @tparam Event The event type.
  /// @param event The event.
  template <class Event>
  void apply(const Event& event) noexcept {
    static_cast<void>(machine_.process_event(event));
  }

  /// @brief The SML machine.
  seat_lifecycle::sml::sm<seat_lifecycle::SeatFsm> machine_;
};

}  // namespace libchiaking::play
