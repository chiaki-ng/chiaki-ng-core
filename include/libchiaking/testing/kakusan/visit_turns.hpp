/// @file
/// @brief Declares VisitTurns, which runs a guest visit one turn at a time.
#pragma once

#include "libchiaking/detail/kakusan/guest_visit.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <chrono>
#include <cstddef>
#include <optional>

namespace libchiaking::kakusan::testing {

/// @brief Stands in for a visit's GuestVisit::Pause so a test decides when the
/// visit moves.
///
/// A real pause sleeps between rounds of work. This one parks the visit each
/// time it asks to wait and holds it there until the test grants a turn, so a
/// test can interleave the guest with the host and with a hand-driven clock
/// without sleeping.
///
/// Pass the object as the pause in the GuestVisit::Ports given when joining,
/// call run() with the visit that comes back, then call turn() once for each
/// round of work the visit should do. Call finish() to collect the outcome.
///
/// @warning finish() and the destructor wait for GuestVisit::run to return. A
///     visit that has not ended must be cancelled first, and its remaining
///     turns granted or finish() called, or the wait never ends.
class VisitTurns final : public GuestVisit::Pause {
 public:
  /// @brief Creates a pause with no visit running.
  ///
  /// @param threading Starts the visit's thread and makes the lock and
  ///     condition the visit parks on; must outlive the object.
  /// @param clock The monotonic clock the parking condition is timed on;
  ///     must outlive the object.
  VisitTurns(platform::ThreadingProvider& threading,
             const platform::ClockProvider& clock);
  /// @brief Not copyable; it owns the thread the visit runs on.
  VisitTurns(const VisitTurns&) = delete;
  /// @brief Not assignable; it owns the thread the visit runs on.
  auto operator=(const VisitTurns&) -> VisitTurns& = delete;
  /// @brief Calls finish() and discards the outcome.
  ///
  /// @warning Blocks until the visit's run has returned.
  ~VisitTurns() override;

  /// @brief Parks the visit until the test grants a turn.
  ///
  /// Called by the visit on its own thread. Once finish() has been called it
  /// returns at once so the visit can wind down.
  ///
  /// @param limit Ignored. The real pause would return after this long; this
  ///     one waits for turn() however long that takes.
  void wait(std::chrono::milliseconds limit) override;
  /// @brief Does nothing.
  ///
  /// A real pause cuts its wait short here. A parked visit is released only
  /// by turn() or finish().
  void wake() noexcept override;

  /// @brief Starts the visit on a thread of its own.
  ///
  /// Calls GuestVisit::run on a new thread and returns once the visit is
  /// parked in its first wait or has already ended. A second call before
  /// finish() is ignored.
  ///
  /// @param visit The visit to run. It must stay alive until finish() has
  ///     returned.
  void run(GuestVisit& visit);
  /// @brief Lets the visit do one round of work.
  ///
  /// Releases the visit from its wait and blocks until it parks in the next
  /// one or ends.
  ///
  /// @return True when the visit is parked again and can be given another
  ///     turn. False when the visit has ended, when run() was never called,
  ///     or after finish().
  auto turn() -> bool;
  /// @brief Reports whether the visit's run has returned.
  ///
  /// @return True once GuestVisit::run has returned and its outcome is held.
  auto ended() const -> bool;
  /// @brief Stops holding the visit back and waits for it to end.
  ///
  /// From this call on wait() returns at once, so the visit runs freely. The
  /// call then joins the visit's thread. It may be called more than once.
  ///
  /// @return The outcome GuestVisit::run returned, or nothing when run() was
  ///     never called.
  [[nodiscard]] auto finish() -> std::optional<VisitOutcome>;

 private:
  platform::ThreadingProvider* threading_;
  mutable platform::Mutex mutex_;
  platform::ConditionVariable changed_;
  platform::Thread thread_;
  std::optional<VisitOutcome> outcome_;
  std::size_t turns_{};
  bool parked_{};
  bool released_{};
  bool over_{};
};

}  // namespace libchiaking::kakusan::testing
