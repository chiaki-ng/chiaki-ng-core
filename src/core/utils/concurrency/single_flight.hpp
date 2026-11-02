/// @file
/// @brief Declares SingleFlight, the in-flight marker that lets one caller
/// compute a value while others holding the same lock wait for it to land.
///
/// The PSN result cache and token cache keep one per slot, so that callers
/// missing the same cache entry at once cause one fetch or one token
/// exchange, and the others read what it left behind.
#pragma once

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace libchiaking::concurrency {

/// @brief Tracks one computation in flight under a lock the owner holds, and
/// keeps the outcome of the last one that landed.
///
/// Unlike SharedFlight it has no mutex of its own: it lives inside the
/// owner's guarded state, and every member must be called with the owner's
/// mutex locked through the platform::Lock passed to the waiting and
/// leading calls. Under that lock it is safe to use from any number of
/// threads; without it, it is not safe to use at all.
///
/// The usual shape: check in_flight(); if a flight is up, await_landing()
/// and read landed(); otherwise lead() the computation, which runs with the
/// lock released. Only one flight may be up at a time; lead() does not check
/// this, the caller must. Waiters are not served in any order: all wake when
/// the flight lands.
///
/// The landed outcome is kept until the next flight lands or forget() is
/// called. The object must outlive any flight and any waiter.
///
/// @tparam Outcome What a landed flight leaves behind for its waiters; must
///     be copy- or move-assignable.
template <class Outcome>
class SingleFlight {
 public:
  /// @brief Makes a marker with no flight up and no landed outcome.
  ///
  /// @param threading Provider of the condition variable waiters park on;
  ///     must outlive the marker and create the owner's mutex too.
  /// @param clock Monotonic clock the condition variable measures on; must
  ///     outlive the marker.
  SingleFlight(platform::ThreadingProvider& threading,
               const platform::ClockProvider& clock)
      : landing_(threading, clock) {}

  SingleFlight(const SingleFlight&) = delete;
  auto operator=(const SingleFlight&) -> SingleFlight& = delete;

  /// @brief Tells whether a computation is running now.
  ///
  /// The owner's mutex must be held.
  ///
  /// @return True from the moment lead() starts until its flight lands or
  ///     is abandoned.
  auto in_flight() const noexcept -> bool { return in_flight_; }

  /// @brief Returns the outcome the last flight left behind.
  ///
  /// The owner's mutex must be held, and the reference is valid only while
  /// it stays held.
  ///
  /// @return The last landed or abandoned outcome, or nothing when no
  ///     flight has landed since construction or the last forget().
  auto landed() const noexcept -> const std::optional<Outcome>& {
    return landed_;
  }

  /// @brief Drops the landed outcome, so that waiters arriving later do not
  /// read it. A flight in progress is not affected and still lands.
  ///
  /// The owner's mutex must be held.
  void forget() noexcept { landed_.reset(); }

  /// @brief Waits for the flight that is up now to land.
  ///
  /// Releases the lock while waiting, until the flight lands or `stop`
  /// fires, and holds it again on return. Waits only for the flight that
  /// was up on entry: if that one lands and another starts before this
  /// waiter wakes, it returns anyway. Returns at once when no flight is up.
  /// Blocks for as long as the leader's computation takes.
  ///
  /// @param lock The caller's lock on the owner's mutex; must be held.
  /// @param stop Cancels the wait.
  /// @return True when the flight landed or none was up; false when `stop`
  ///     fired first, the flight still being up.
  [[nodiscard]] auto await_landing(platform::Lock& lock,
                                   const platform::StopToken& stop) -> bool {
    const auto round = round_;
    return landing_.wait(lock, stop,
                         [&] { return !in_flight_ || round_ != round; });
  }

  /// @brief Leads a flight; if the computation throws, the previous landed
  /// outcome is kept.
  ///
  /// Same as the four-argument lead() with an abandon step that leaves
  /// landed() untouched.
  ///
  /// @tparam Compute A callable taking no arguments.
  /// @tparam Land A callable taking the computation's result as an lvalue
  ///     and returning Outcome.
  /// @param lock The caller's lock on the owner's mutex; held on entry and
  ///     on return.
  /// @param compute The computation, run with the lock released.
  /// @param land Turns the result into the landed outcome, run with the
  ///     lock held.
  /// @return The result of `compute`.
  /// @throws Whatever `compute` or `land` throws, the lock being held
  ///     again and the flight settled.
  template <class Compute, class Land>
  auto lead(platform::Lock& lock, Compute&& compute, Land&& land)
      -> std::invoke_result_t<Compute&> {
    return lead(lock, std::forward<Compute>(compute), std::forward<Land>(land),
                KeepLanded{});
  }

  /// @brief Puts a flight up, runs the computation with the lock released,
  /// and lands its outcome for the waiters.
  ///
  /// Marks the flight up and unlocks; runs `compute` on the calling thread;
  /// locks again, stores what `land` makes of the result as landed(),
  /// marks the flight down and wakes every waiter. When `compute` or `land`
  /// throws, it locks again, stores what `abandon` returns, marks the
  /// flight down and wakes the waiters, then lets the exception through.
  /// Blocks for as long as `compute` takes, plus the time to take the lock
  /// again.
  ///
  /// No flight may be up already; check in_flight() first.
  ///
  /// @tparam Compute A callable taking no arguments.
  /// @tparam Land A callable taking the computation's result as an lvalue
  ///     and returning Outcome.
  /// @tparam Abandon A callable taking no arguments and returning Outcome.
  /// @param lock The caller's lock on the owner's mutex; held on entry and
  ///     on return.
  /// @param compute The computation, run with the lock released.
  /// @param land Turns the result into the landed outcome, run with the
  ///     lock held; may also update the owner's state.
  /// @param abandon Makes the outcome left behind when the flight fails
  ///     with an exception, run with the lock held.
  /// @return The result of `compute`.
  /// @throws Whatever `compute` or `land` throws, the lock being held
  ///     again and the flight settled.
  template <class Compute, class Land, class Abandon>
  auto lead(platform::Lock& lock, Compute&& compute, Land&& land,
            Abandon&& abandon) -> std::invoke_result_t<Compute&> {
    Departure<Abandon> departure{*this, lock, abandon};
    auto result = std::invoke(compute);
    departure.land([&] { return std::invoke(land, result); });
    return result;
  }

 private:
  /// @brief The abandon step that keeps the previous landed outcome.
  struct KeepLanded {};

  /// @brief The leader's hold on its flight: puts it up on construction and
  /// settles it on destruction, abandoning it when it never landed.
  ///
  /// @tparam Abandon The abandon step's type, or KeepLanded.
  template <class Abandon>
  class Departure {
   public:
    /// @brief Puts the flight up and releases the lock.
    ///
    /// @param flight The marker the flight belongs to.
    /// @param lock The caller's lock on the owner's mutex; held on entry,
    ///     released on return.
    /// @param abandon The step run if the flight never lands; must
    ///     outlive the departure.
    Departure(SingleFlight& flight, platform::Lock& lock, Abandon& abandon)
        : flight_(flight), lock_(lock), abandon_(abandon) {
      flight_.in_flight_ = true;
      lock_.unlock();
    }

    Departure(const Departure&) = delete;
    auto operator=(const Departure&) -> Departure& = delete;

    /// @brief Abandons a flight that did not land: takes the lock if it
    /// is not held, stores the abandon step's outcome (none for
    /// KeepLanded) and settles the flight.
    ~Departure() {
      if (landed_) return;
      if (!lock_.owns_lock()) lock_.lock();
      if constexpr (!std::is_same_v<std::remove_cvref_t<Abandon>, KeepLanded>)
        flight_.landed_ = std::invoke(abandon_);
      flight_.settle();
    }

    /// @brief Takes the lock, stores the landed outcome and settles the
    /// flight; blocks until the owner's mutex is free.
    ///
    /// @tparam Land A callable taking no arguments and returning Outcome.
    /// @param land Makes the outcome, run with the lock held.
    template <class Land>
    void land(Land&& land) {
      lock_.lock();
      flight_.landed_ = std::invoke(land);
      landed_ = true;
      flight_.settle();
    }

   private:
    /// @brief The marker the flight belongs to.
    SingleFlight& flight_;
    /// @brief The leader's lock on the owner's mutex.
    platform::Lock& lock_;
    /// @brief The step run if the flight never lands.
    Abandon& abandon_;
    /// @brief Whether land() completed, so the destructor has nothing to
    /// do.
    bool landed_{};
  };

  /// @brief Marks the flight down, starts a new round and wakes every
  /// waiter; the owner's mutex must be held.
  void settle() noexcept {
    in_flight_ = false;
    ++round_;
    landing_.notify_all();
  }

  /// @brief Whether a flight is up.
  bool in_flight_{};
  /// @brief Counts settled flights, so that a waiter can tell that the
  /// flight it waited for has landed even when another is already up.
  std::uint64_t round_{};
  /// @brief The outcome the last flight left behind, if any.
  std::optional<Outcome> landed_;
  /// @brief Signalled whenever a flight settles.
  platform::ConditionVariable landing_;
};

}  // namespace libchiaking::concurrency
