/// @file
/// @brief Declares LockedSessionLifecycle, which shares a session's
/// SessionLifecycle between its run loop and the frontend's threads.
#pragma once

#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/play/model/session_lifecycle.hpp"

namespace libchiaking::play {

/// @brief A SessionLifecycle behind a mutex.
///
/// Owned by one Session::Core, which drives it from its run loop while
/// frontend threads read it and cancel or park the session. Every member locks
/// the mutex for one call on the machine, so it is safe to share between
/// those threads. Neither copyable nor movable.
class LockedSessionLifecycle {
 public:
  /// @brief The run state, as SessionLifecycle names it.
  using Phase = SessionLifecycle::Phase;

  /// @brief Makes a lifecycle in the created and staying states.
  ///
  /// @param threading Provider of the mutex guarding the machine; must
  ///     outlive the lifecycle.
  explicit LockedSessionLifecycle(platform::ThreadingProvider& threading)
      : mutex_(threading) {}

  /// @brief Reads the run state.
  ///
  /// @return As SessionLifecycle::phase.
  auto phase() const noexcept -> Phase {
    const platform::Lock lock(mutex_);
    return lifecycle_.phase();
  }

  /// @brief Reports whether the session has been asked to end for any reason.
  ///
  /// @return As SessionLifecycle::cancelled.
  auto cancelled() const noexcept -> bool {
    const platform::Lock lock(mutex_);
    return lifecycle_.cancelled();
  }

  /// @brief Reports whether the session is being parked for a host sleep.
  ///
  /// @return As SessionLifecycle::sleeping.
  auto sleeping() const noexcept -> bool {
    const platform::Lock lock(mutex_);
    return lifecycle_.sleeping();
  }

  /// @brief Reports whether the session may take commands for the console.
  ///
  /// @return As SessionLifecycle::accepting.
  auto accepting() const noexcept -> bool {
    const platform::Lock lock(mutex_);
    return lifecycle_.accepting();
  }

  /// @brief Records that the stream started.
  ///
  /// @return As SessionLifecycle::begin.
  auto begin() noexcept -> bool {
    const platform::Lock lock(mutex_);
    return lifecycle_.begin();
  }

  /// @brief Records that the session ended; repeating it is harmless.
  void finish() noexcept {
    const platform::Lock lock(mutex_);
    lifecycle_.finish();
  }

  /// @brief Records that the player asked to leave.
  void cancel() noexcept {
    const platform::Lock lock(mutex_);
    lifecycle_.cancel();
  }

  /// @brief Parks the session for a host sleep while the stream runs.
  ///
  /// @return As SessionLifecycle::sleep.
  auto sleep() noexcept -> bool {
    const platform::Lock lock(mutex_);
    return lifecycle_.sleep();
  }

  /// @brief Records that a rest-mode request to the console finished.
  ///
  /// @param for_sleep Whether the rest was asked for because the host is going
  ///     to sleep.
  void conclude(bool for_sleep) noexcept {
    const platform::Lock lock(mutex_);
    lifecycle_.conclude(for_sleep);
  }

 private:
  /// @brief Guards the machine; mutable so the readers can lock it.
  mutable platform::Mutex mutex_;
  /// @brief The machine.
  SessionLifecycle lifecycle_;
};

}  // namespace libchiaking::play
