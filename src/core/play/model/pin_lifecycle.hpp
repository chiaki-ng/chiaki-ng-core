/// @file
/// @brief Declares PinLifecycle, where a remote start stands with the
/// console's request for a login PIN: whether the console asked and is
/// waiting for one, and whether the player was already prompted.
#pragma once

#include <boost/sml.hpp>

namespace libchiaking::play::pin_lifecycle {

/// @brief Boost.SML, which the machine is written in.
namespace sml = boost::sml;

/// @brief Notice state: no PIN request is outstanding. The initial notice
/// state.
struct Unasked {};
/// @brief Notice state: the console asked for a PIN and waits for it.
struct Asked {};
/// @brief Prompt state: the player has not been prompted on this control
/// connection. The initial prompt state.
struct Unprompted {};
/// @brief Prompt state: the player was prompted on this control connection.
struct Prompted {};

/// @brief Event: the console asked for a PIN.
struct Ask {};
/// @brief Event: the PIN was sent or the route was released.
struct Withdraw {};
/// @brief Event: the player is prompted.
struct Prompt {};
/// @brief Event: a new control connection starts the prompts over.
struct Rearm {};

/// @brief The transition table of the PIN exchange.
///
/// Two orthogonal regions. The notice region goes from Unasked to Asked on
/// Ask and back on Withdraw. The prompt region goes from Unprompted to
/// Prompted on Prompt and back on Rearm. Repeats are accepted and change
/// nothing.
struct PinFsm {
  /// @brief Builds the transition table for Boost.SML.
  /// @return The table of both regions.
  auto operator()() const noexcept {
    using namespace sml;
    const auto unchanged = [] {};
    return make_transition_table(
        *state<Unasked> + sml::event<Ask> = state<Asked>,
        state<Unasked> + sml::event<Withdraw> / unchanged,
        state<Asked> + sml::event<Ask> / unchanged,
        state<Asked> + sml::event<Withdraw> = state<Unasked>,

        *state<Unprompted> + sml::event<Prompt> = state<Prompted>,
        state<Unprompted> + sml::event<Rearm> / unchanged,
        state<Prompted> + sml::event<Prompt> / unchanged,
        state<Prompted> + sml::event<Rearm> = state<Unprompted>);
  }
};

}  // namespace libchiaking::play::pin_lifecycle

namespace libchiaking::play {

/// @brief Where a remote start stands with the console's PIN request.
///
/// Owned by one internet link and driven from its pump; not thread-safe.
class PinLifecycle {
 public:
  /// @brief Records that the console asked for a PIN.
  void ask() noexcept { apply(pin_lifecycle::Ask{}); }
  /// @brief Records that no PIN request is outstanding any more.
  void withdraw() noexcept { apply(pin_lifecycle::Withdraw{}); }
  /// @brief Prompts the player.
  /// @return Whether the player was already prompted on this connection.
  auto prompt() noexcept -> bool {
    const bool again =
        machine_.is(pin_lifecycle::sml::state<pin_lifecycle::Prompted>);
    apply(pin_lifecycle::Prompt{});
    return again;
  }
  /// @brief Starts the prompts over for a new control connection.
  void rearm() noexcept { apply(pin_lifecycle::Rearm{}); }
  /// @brief Tells whether the console waits for a PIN.
  /// @return True while asked.
  auto asked() const noexcept -> bool {
    return machine_.is(pin_lifecycle::sml::state<pin_lifecycle::Asked>);
  }

 private:
  /// @brief Feeds an event; the table accepts every event in every state.
  /// @tparam Event The event type.
  /// @param event The event.
  template <class Event>
  void apply(const Event& event) noexcept {
    static_cast<void>(machine_.process_event(event));
  }

  /// @brief The SML machine.
  pin_lifecycle::sml::sm<pin_lifecycle::PinFsm> machine_;
};

}  // namespace libchiaking::play
