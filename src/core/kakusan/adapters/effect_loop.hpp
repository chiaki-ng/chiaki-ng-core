/// @file
/// @brief Declares EffectLoop, which feeds events to a per-player state
/// machine one at a time while the effects of earlier events are applied.
#pragma once

#include <deque>
#include <utility>

namespace libchiaking::kakusan {

/// @brief Runs a player machine's event-to-effects loop without reentering
/// it.
///
/// Applying an effect often produces a new event for the same machine (a
/// link opening, a timer, a message sent). EffectLoop queues such events and
/// handles them only after the current batch of effects has been applied, so
/// the machine always sees events in order and is never called from inside
/// its own effect handling.
///
/// One loop belongs to one machine: an InvitedPlayer inside the host's
/// SharedSession::Core, or the VisitingPlayer inside GuestVisit::Core. It is
/// not thread-safe; its owner's lock must be held for every call.
///
/// @tparam Player The machine. It must provide the types `Event` and
///     `Effects` (a range of effects) and `auto handle (Event) -> Effects`.
template <class Player>
class EffectLoop {
 public:
  /// @brief What the machine is fed.
  using Event = typename Player::Event;
  /// @brief What the machine answers with: the effects to apply, in order.
  using Effects = typename Player::Effects;

  /// @brief Hands one event to the machine and applies the effects.
  ///
  /// When called from inside an `apply`, the event is queued and handled by
  /// the loop already running once the current effects are done.
  ///
  /// @tparam Apply Callable taking one effect by lvalue reference.
  /// @param player The machine; the same one on every call.
  /// @param event The event.
  /// @param apply Carries out one effect; may call feed() again.
  template <class Apply>
  void feed(Player& player, Event event, Apply&& apply) {
    if (running_) {
      pending_.push_back(std::move(event));
      return;
    }
    run(player, player.handle(std::move(event)), apply);
  }

  /// @brief Applies effects the machine already produced, then handles every
  /// event queued meanwhile until none is left.
  ///
  /// Used when a machine call other than handle() returned effects, such as
  /// starting the machine.
  ///
  /// @tparam Apply Callable taking one effect by lvalue reference.
  /// @param player The machine; the same one on every call.
  /// @param effects The effects to apply first.
  /// @param apply Carries out one effect; may call feed().
  template <class Apply>
  void run(Player& player, Effects effects, Apply&& apply) {
    running_ = true;
    for (auto& effect : effects) apply(effect);
    while (!pending_.empty()) {
      auto event = std::move(pending_.front());
      pending_.pop_front();
      for (auto& effect : player.handle(std::move(event))) apply(effect);
    }
    running_ = false;
  }

  /// @brief Forgets every queued event, for when the machine is torn down.
  void clear() noexcept { pending_.clear(); }

 private:
  /// @brief Events fed while effects were being applied, oldest first.
  std::deque<Event> pending_;
  /// @brief True while run() is applying effects.
  bool running_{};
};

}  // namespace libchiaking::kakusan
