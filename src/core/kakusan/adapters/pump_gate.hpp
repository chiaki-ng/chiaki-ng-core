/// @file
/// @brief Declares PumpGate, which lets one thread at a time run a Kakusan
/// pump turn and deliver what it produced.
///
/// A shared session or guest visit works out its next steps under its state
/// lock, then hands them to the frontend's observer with that lock released so
/// the observer may call commands. PumpGate keeps two such turns from
/// interleaving their deliveries, refuses a turn started from inside a
/// delivery, and gives the owner's destructor something to wait on until a
/// delivery in progress has finished.
#pragma once

#include "libchiaking/platform/runtime/threading.hpp"

#include <atomic>

namespace libchiaking::kakusan {

/// @brief Serialises pump turns and their deliveries, and knows which thread
/// is inside one.
///
/// Owned by SharedSession::Hosting and GuestVisit::Visiting, one per shared
/// session or visit, and lives as long as they do. Its mutex is only ever
/// taken before the owner's state mutex, never after it. Every member may be
/// called from any thread.
class PumpGate {
 public:
  /// @brief Holds the gate for one turn and records the calling thread as
  /// its holder.
  ///
  /// Scoped: the gate is released and the holder cleared when the Turn is
  /// destroyed. Not copyable, and must be destroyed on the thread that made
  /// it.
  class Turn {
   public:
    /// @brief Takes the gate, blocking until any other thread's turn has
    /// finished.
    ///
    /// Must not be made by a thread that already holds the gate; that
    /// would deadlock. take_turn() checks held_by_this_thread() first.
    ///
    /// @param gate The gate to hold; must outlive the turn.
    explicit Turn(PumpGate& gate) : lock_(gate.mutex_), gate_(gate) {
      gate_.owner_.store(platform::ThreadIdentity::current());
    }
    /// @brief Clears the holder and releases the gate.
    ~Turn() { gate_.owner_.store(platform::ThreadIdentity{}); }
    /// @brief Not copyable; a turn is held once.
    Turn(const Turn&) = delete;
    /// @brief Not copy-assignable; a turn is held once.
    auto operator=(const Turn&) -> Turn& = delete;

   private:
    /// @brief Holds the gate's mutex for the life of the turn.
    platform::Lock lock_;
    /// @brief The gate held, whose holder is cleared on destruction.
    PumpGate& gate_;
  };

  /// @brief Makes an open gate.
  ///
  /// @param threading Provider of the gate's mutex; must outlive the gate.
  explicit PumpGate(platform::ThreadingProvider& threading)
      : mutex_(threading) {}
  /// @brief Not copyable; turns refer to the gate.
  PumpGate(const PumpGate&) = delete;
  /// @brief Not copy-assignable; turns refer to the gate.
  auto operator=(const PumpGate&) -> PumpGate& = delete;

  /// @brief Runs one pump turn: works out the steps under the state lock,
  /// then delivers them with only the gate held.
  ///
  /// Does nothing when the calling thread already holds the gate, which
  /// happens when an observer reached from a delivery triggers another turn;
  /// the steps it would have produced wait for the next turn. Otherwise
  /// blocks until any other thread's turn is over, for as long as that
  /// turn's deliveries take.
  ///
  /// @tparam Work Callable taking no arguments and returning a range of
  ///     steps, such as a std::deque; run with `state` locked.
  /// @tparam Deliver Callable taking one step by lvalue reference; run for
  ///     each step in order, with `state` unlocked.
  /// @param state The owner's state mutex. Must not be held by the caller.
  /// @param work Advances the machine and collects the steps to deliver.
  /// @param deliver Hands one step on, typically to the observer.
  template <class Work, class Deliver>
  void take_turn(platform::Mutex& state, Work&& work, Deliver&& deliver) {
    if (held_by_this_thread()) return;
    const Turn turn{*this};
    auto ready = [&] {
      const platform::Lock lock{state};
      return work();
    }();
    for (auto& step : ready) deliver(step);
  }

  /// @brief Tells whether the calling thread is inside a turn of this gate.
  ///
  /// @return True while the calling thread holds a Turn on this gate.
  auto held_by_this_thread() const noexcept -> bool {
    return owner_.load() == platform::ThreadIdentity::current();
  }

  /// @brief The gate's own mutex, for an owner's destructor that must wait
  /// until a delivery in progress has finished.
  ///
  /// @return The mutex every Turn holds; lock it before the owner's state
  ///     mutex, never after.
  auto mutex() noexcept -> platform::Mutex& { return mutex_; }

 private:
  /// @brief Held for the whole of a turn, deliveries included.
  platform::Mutex mutex_;
  /// @brief The thread holding the gate, or an identity naming no thread
  /// when none does.
  std::atomic<platform::ThreadIdentity> owner_{};
};

}  // namespace libchiaking::kakusan
