/// @file
/// @brief Header-only checks that a PlatformFactory's threading, clock and
/// random providers behave as the library requires.
#pragma once

#include <libchiaking/platform/factory.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace libchiaking::platform::conformance {

/// @brief Identifies one runtime conformance check; the value is its bit in
/// Report::failed_mask.
enum class Check : std::uint32_t {
  /// @brief A started thread runs its entry, joins, and is no longer
  /// joinable.
  thread_start_join = 1u << 0,
  /// @brief A thread parked in a stop-aware wait returns after its stop
  /// source is triggered.
  thread_stop = 1u << 1,
  /// @brief Four threads incrementing under one mutex never overlap or lose
  /// an update.
  mutex_exclusion = 1u << 2,
  /// @brief try_lock neither blocks nor fails on a held mutex and acquires a
  /// free one.
  mutex_try_lock = 1u << 3,
  /// @brief notify_one wakes a timed waiter with
  /// ConditionWaitStatus::signaled.
  condition_notify = 1u << 4,
  /// @brief A 50 ms wait with no notification reports timed_out after roughly
  /// that long.
  condition_timeout = 1u << 5,
  /// @brief Triggering the stop source ends a stop-aware timed wait with
  /// stop_requested, without any notification.
  condition_cancellation = 1u << 6,
  /// @brief A wait releases the mutex while parked and holds it again when it
  /// returns.
  condition_atomic_unlock_relock = 1u << 7,
  /// @brief Waiting on a condition variable with another provider's mutex
  /// fails with PlatformError::wrong_provider.
  provider_mixing = 1u << 8,
  /// @brief The clock never reads zero and never goes backwards.
  monotonic_clock = 1u << 9,
  /// @brief Two 64-byte fills succeed and differ.
  random_fill = 1u << 10,
  /// @brief PlatformInfo has a name, the current ABI version and the three
  /// mandatory runtime flags.
  capability_consistency = 1u << 11,
  /// @brief A joined thread reports that it is not joinable.
  no_leaked_joinable_thread = 1u << 12,
  /// @brief notify_one wakes an untimed stop-aware waiter with signaled while
  /// no stop was requested.
  condition_unbounded_signaled = 1u << 13,
};

/// @brief Outcome of run().
struct Report {
  /// @brief True when every check that ran passed.
  bool ok{true};
  /// @brief Bitwise OR of the Check values that failed.
  std::uint32_t failed_mask{0};
};

namespace detail {

/// @brief Longest time a check waits for another thread before failing.
inline constexpr auto deadline = std::chrono::seconds{2};
/// @brief Polling interval used while waiting for another thread.
inline constexpr auto tick = std::chrono::milliseconds{1};

/// @brief One-way flag a check thread raises for the thread running the checks.
struct SpinFlag {
  /// @brief True once set() has been called.
  std::atomic<bool> value{false};

  /// @brief Raises the flag.
  void set() noexcept { value.store(true, std::memory_order_release); }
  /// @brief Whether the flag has been raised.
  bool get() const noexcept { return value.load(std::memory_order_acquire); }
};

/// @brief Polls a predicate once per tick until it holds or deadline passes.
///
/// @tparam Predicate Callable returning something convertible to bool.
/// @param predicate Condition to wait for.
/// @return True when the predicate held in time, false on timeout.
template <class Predicate>
inline bool spin_until(Predicate&& predicate) noexcept {
  const auto start = std::chrono::steady_clock::now();
  while (!predicate()) {
    if (std::chrono::steady_clock::now() - start > deadline) return false;
    std::this_thread::sleep_for(tick);
  }
  return true;
}

/// @brief Marks a check as failed in the report.
///
/// @param report Report to update.
/// @param check Check that failed.
inline void fail(Report& report, Check check) noexcept {
  report.ok = false;
  report.failed_mask |= static_cast<std::uint32_t>(check);
}

/// @brief Runs Check::thread_start_join.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_thread_start_join(PlatformFactory& factory, Report& report) {
  struct Context {
    SpinFlag ran;
  } context;
  const auto thread = factory.threading().start_thread(
      {[](void* user_context) noexcept {
         static_cast<Context*>(user_context)->ran.set();
       },
       &context});
  if (!thread) {
    fail(report, Check::thread_start_join);
    return false;
  }
  if (!spin_until([&] { return context.ran.get(); }) ||
      !thread.value()->join() || thread.value()->joinable()) {
    fail(report, Check::thread_start_join);
    return false;
  }
  return true;
}

/// @brief Runs Check::thread_stop.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_thread_stop(PlatformFactory& factory, Report& report) {
  struct Context {
    native::ConditionVariable* condition{};
    native::Mutex* mutex{};
    StopToken token;
    SpinFlag waiting;
    SpinFlag woke;
  };
  const auto mutex = factory.threading().create_mutex();
  const auto condition = factory.threading().create_condition_variable();
  const auto stop_source = factory.threading().create_stop_source();
  if (!mutex || !condition || !stop_source) {
    fail(report, Check::thread_stop);
    return false;
  }
  Context context{condition.value().get(),
                  mutex.value().get(),
                  stop_source.value()->token(),
                  {},
                  {}};
  const auto thread = factory.threading().start_thread(
      {[](void* user_context) noexcept {
         auto& state = *static_cast<Context*>(user_context);
         (void)state.mutex->lock();
         state.waiting.set();
         (void)state.condition->wait_for(*state.mutex, detail::deadline,
                                         state.token);
         state.woke.set();
         (void)state.mutex->unlock();
       },
       &context});
  (void)context;
  if (!thread) {
    fail(report, Check::thread_stop);
    return false;
  }
  const auto waited = spin_until([&] { return context.waiting.get(); });
  (void)stop_source.value()->request_stop();
  const auto joined = thread.value()->join();
  if (!waited || !context.woke.get() || !joined ||
      !stop_source.value()->token().stop_requested()) {
    fail(report, Check::thread_stop);
    return false;
  }
  return true;
}

/// @brief Runs Check::mutex_exclusion.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_mutex_exclusion(PlatformFactory& factory, Report& report) {
  struct Context {
    native::Mutex* mutex{};
    SpinFlag violation;
    int counter{0};
    bool in_critical{false};
  };
  const auto mutex = factory.threading().create_mutex();
  if (!mutex) {
    fail(report, Check::mutex_exclusion);
    return false;
  }
  Context context{mutex.value().get(), {}, {}, false};
  std::array<std::unique_ptr<native::Thread>, 4> threads;
  for (auto& thread : threads) {
    auto started = factory.threading().start_thread(
        {[](void* user_context) noexcept {
           auto& state = *static_cast<Context*>(user_context);
           for (int i = 0; i < 500; ++i) {
             if (!state.mutex->lock()) {
               state.violation.set();
               return;
             }
             if (state.in_critical) state.violation.set();
             state.in_critical = true;
             ++state.counter;
             state.in_critical = false;
             if (!state.mutex->unlock()) {
               state.violation.set();
               return;
             }
           }
         },
         &context});
    if (!started) {
      fail(report, Check::mutex_exclusion);
      for (auto& joined_thread : threads) {
        if (joined_thread) (void)joined_thread->join();
      }
      return false;
    }
    thread = std::move(started.value());
  }
  for (auto& thread : threads) {
    if (!thread->join()) {
      fail(report, Check::mutex_exclusion);
      return false;
    }
  }
  if (context.violation.get() || context.counter != 4 * 500) {
    fail(report, Check::mutex_exclusion);
    return false;
  }
  return true;
}

/// @brief State shared with the thread started by check_mutex_try_lock.
struct TryLockContext {
  /// @brief Mutex the thread tries to lock while the checker holds it.
  native::Mutex* mutex{};
  /// @brief Raised once the thread's try_lock call has returned.
  SpinFlag locked;
};

/// @brief Runs Check::mutex_try_lock.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_mutex_try_lock(PlatformFactory& factory, Report& report) {
  const auto mutex = factory.threading().create_mutex();
  if (!mutex) {
    fail(report, Check::mutex_try_lock);
    return false;
  }
  native::Mutex& native = *mutex.value();
  if (!native.lock()) {
    fail(report, Check::mutex_try_lock);
    return false;
  }
  TryLockContext context{mutex.value().get(), {}};
  const auto thread = factory.threading().start_thread(
      {[](void* user_context) noexcept {
         auto& state = *static_cast<TryLockContext*>(user_context);
         const auto attempt = state.mutex->try_lock();
         state.locked.set();
         if (attempt && attempt.value()) (void)state.mutex->unlock();
       },
       &context});
  if (!thread) {
    fail(report, Check::mutex_try_lock);
    return false;
  }
  if (!spin_until([&] { return context.locked.get(); })) {
    (void)thread.value()->join();
    fail(report, Check::mutex_try_lock);
    return false;
  }
  if (!thread.value()->join()) {
    fail(report, Check::mutex_try_lock);
    return false;
  }
  if (!native.unlock()) {
    fail(report, Check::mutex_try_lock);
    return false;
  }
  const auto acquired = native.try_lock();
  const auto released = native.unlock();
  if (!acquired || !acquired.value() || !released) {
    fail(report, Check::mutex_try_lock);
    return false;
  }
  return true;
}

/// @brief Runs Check::condition_notify.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_condition_notify(PlatformFactory& factory, Report& report) {
  struct Context {
    native::ConditionVariable* condition{};
    native::Mutex* mutex{};
    SpinFlag waiting;
    ConditionWaitStatus status{ConditionWaitStatus::timed_out};
    SpinFlag done;
  };
  const auto mutex = factory.threading().create_mutex();
  const auto condition = factory.threading().create_condition_variable();
  if (!mutex || !condition) {
    fail(report, Check::condition_notify);
    return false;
  }
  Context context{condition.value().get(), mutex.value().get(), {}, {}, {}};
  const auto thread = factory.threading().start_thread(
      {[](void* user_context) noexcept {
         auto& state = *static_cast<Context*>(user_context);
         (void)state.mutex->lock();
         state.waiting.set();
         const auto result =
             state.condition->wait_for(*state.mutex, detail::deadline);
         if (result) state.status = result.value();
         (void)state.mutex->unlock();
         state.done.set();
       },
       &context});
  if (!thread) {
    fail(report, Check::condition_notify);
    return false;
  }
  if (!spin_until([&] { return context.waiting.get(); })) {
    (void)thread.value()->join();
    fail(report, Check::condition_notify);
    return false;
  }
  condition.value()->notify_one();
  if (!spin_until([&] { return context.done.get(); }) ||
      !thread.value()->join() ||
      context.status != ConditionWaitStatus::signaled) {
    fail(report, Check::condition_notify);
    return false;
  }
  return true;
}

/// @brief Runs Check::condition_timeout.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_condition_timeout(PlatformFactory& factory, Report& report) {
  const auto mutex = factory.threading().create_mutex();
  const auto condition = factory.threading().create_condition_variable();
  if (!mutex || !condition) {
    fail(report, Check::condition_timeout);
    return false;
  }
  if (!mutex.value()->lock()) {
    fail(report, Check::condition_timeout);
    return false;
  }
  const auto start = std::chrono::steady_clock::now();
  const auto result = condition.value()->wait_for(
      *mutex.value(), std::chrono::milliseconds{50});
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto released = mutex.value()->unlock();
  if (!result || !released ||
      result.value() != ConditionWaitStatus::timed_out ||
      elapsed < std::chrono::milliseconds{45} ||
      elapsed > std::chrono::seconds{2}) {
    fail(report, Check::condition_timeout);
    return false;
  }
  return true;
}

/// @brief State shared with the thread started by check_condition_cancellation.
struct CancellationContext {
  /// @brief Condition variable the thread waits on.
  native::ConditionVariable* condition{};
  /// @brief Mutex the thread holds around its wait.
  native::Mutex* mutex{};
  /// @brief Source of the stop token passed to the wait.
  native::StopSource* stop_source{};
  /// @brief Raised just before the thread starts waiting.
  SpinFlag waiting;
  /// @brief Raised once the thread has finished and released the mutex.
  SpinFlag done;
  /// @brief Status the wait returned.
  ConditionWaitStatus status{ConditionWaitStatus::timed_out};
};

/// @brief Runs Check::condition_cancellation.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_condition_cancellation(PlatformFactory& factory,
                                         Report& report) {
  const auto mutex = factory.threading().create_mutex();
  const auto condition = factory.threading().create_condition_variable();
  const auto stop_source = factory.threading().create_stop_source();
  if (!mutex || !condition || !stop_source) {
    fail(report, Check::condition_cancellation);
    return false;
  }
  CancellationContext context{condition.value().get(),
                              mutex.value().get(),
                              stop_source.value().get(),
                              {},
                              {},
                              {}};
  const auto thread = factory.threading().start_thread(
      {[](void* user_context) noexcept {
         auto& state = *static_cast<CancellationContext*>(user_context);
         (void)state.mutex->lock();
         state.waiting.set();
         const auto result = state.condition->wait_for(
             *state.mutex, detail::deadline, state.stop_source->token());
         if (result) state.status = result.value();
         (void)state.mutex->unlock();
         state.done.set();
       },
       &context});
  if (!thread) {
    fail(report, Check::condition_cancellation);
    return false;
  }
  if (!spin_until([&] { return context.waiting.get(); })) {
    (void)thread.value()->join();
    fail(report, Check::condition_cancellation);
    return false;
  }
  (void)stop_source.value()->request_stop();
  if (!spin_until([&] { return context.done.get(); }) ||
      !thread.value()->join() ||
      context.status != ConditionWaitStatus::stop_requested) {
    fail(report, Check::condition_cancellation);
    return false;
  }
  return true;
}

/// @brief State shared with the thread started by
/// check_condition_atomic_unlock_relock.
struct RelockContext {
  /// @brief Condition variable the thread waits on.
  native::ConditionVariable* condition{};
  /// @brief Mutex the thread holds around its wait.
  native::Mutex* mutex{};
  /// @brief Raised just before the thread starts waiting.
  SpinFlag waiting;
  /// @brief Raised when the wait has returned and the thread holds the mutex
  /// again.
  SpinFlag still_locked;
  /// @brief Raised by the checker to let the thread unlock and exit.
  SpinFlag release;
};

/// @brief Runs Check::condition_atomic_unlock_relock.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_condition_atomic_unlock_relock(PlatformFactory& factory,
                                                 Report& report) {
  const auto mutex = factory.threading().create_mutex();
  const auto condition = factory.threading().create_condition_variable();
  if (!mutex || !condition) {
    fail(report, Check::condition_atomic_unlock_relock);
    return false;
  }
  RelockContext context{
      condition.value().get(), mutex.value().get(), {}, {}, {}};
  const auto thread = factory.threading().start_thread(
      {[](void* user_context) noexcept {
         auto& state = *static_cast<RelockContext*>(user_context);
         (void)state.mutex->lock();
         state.waiting.set();
         const auto result =
             state.condition->wait_for(*state.mutex, detail::deadline);
         state.still_locked.set();
         while (!state.release.get()) std::this_thread::sleep_for(detail::tick);
         (void)state.mutex->unlock();
         (void)result;
       },
       &context});
  if (!thread) {
    fail(report, Check::condition_atomic_unlock_relock);
    return false;
  }
  const auto abandoned = [&] {
    context.release.set();
    (void)thread.value()->join();
  };
  if (!spin_until([&] { return context.waiting.get(); })) {
    abandoned();
    fail(report, Check::condition_atomic_unlock_relock);
    return false;
  }
  bool acquired_while_waiting = false;
  const auto polled = spin_until([&] {
    const auto attempt = mutex.value()->try_lock();
    if (attempt && attempt.value()) {
      acquired_while_waiting = true;
      return true;
    }
    return false;
  });
  if (!polled || !acquired_while_waiting) {
    abandoned();
    fail(report, Check::condition_atomic_unlock_relock);
    return false;
  }
  condition.value()->notify_one();
  if (!mutex.value()->unlock()) {
    abandoned();
    fail(report, Check::condition_atomic_unlock_relock);
    return false;
  }
  if (!spin_until([&] { return context.still_locked.get(); })) {
    abandoned();
    fail(report, Check::condition_atomic_unlock_relock);
    return false;
  }
  const auto relocked = mutex.value()->try_lock();
  const bool thread_held_lock = relocked && !relocked.value();
  context.release.set();
  if (!thread.value()->join() || !thread_held_lock) {
    fail(report, Check::condition_atomic_unlock_relock);
    return false;
  }
  return true;
}

/// @brief Runs Check::provider_mixing.
///
/// @param factory Factory under test.
/// @param foreign A second factory whose threading provider is a different
///     object.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_provider_mixing(PlatformFactory& factory,
                                  PlatformFactory& foreign, Report& report) {
  const auto mutex = factory.threading().create_mutex();
  const auto foreign_condition =
      foreign.threading().create_condition_variable();
  const auto condition = factory.threading().create_condition_variable();
  if (!mutex || !foreign_condition || !condition) {
    fail(report, Check::provider_mixing);
    return false;
  }
  const auto mixed = foreign_condition.value()->wait_for(
      *mutex.value(), std::chrono::milliseconds{1});
  const auto locked = mutex.value()->lock();
  if (mixed || mixed.error() != PlatformError::wrong_provider || !locked) {
    fail(report, Check::provider_mixing);
    return false;
  }
  const auto matched =
      condition.value()->wait_for(*mutex.value(), std::chrono::milliseconds{1});
  const auto released = mutex.value()->unlock();
  if (!matched || !released) {
    fail(report, Check::provider_mixing);
    return false;
  }
  return true;
}

/// @brief Runs Check::monotonic_clock.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_monotonic_clock(PlatformFactory& factory, Report& report) {
  const auto first = factory.clock().now_microseconds();
  const auto second = factory.clock().now_microseconds();
  if (second < first || first == 0) {
    fail(report, Check::monotonic_clock);
    return false;
  }
  return true;
}

/// @brief Runs Check::random_fill.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_random_fill(PlatformFactory& factory, Report& report) {
  std::array<std::byte, 64> first{};
  std::array<std::byte, 64> second{};
  const auto filled_first = factory.random().fill_bytes(first);
  const auto filled_second = factory.random().fill_bytes(second);
  if (!filled_first || !filled_second) {
    fail(report, Check::random_fill);
    return false;
  }
  std::size_t differing = 0;
  for (std::size_t i = 0; i < first.size(); ++i) {
    if (first[i] != second[i]) ++differing;
  }
  if (differing == 0) {
    fail(report, Check::random_fill);
    return false;
  }
  return true;
}

/// @brief Runs Check::capability_consistency.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_capability_consistency(PlatformFactory& factory,
                                         Report& report) {
  const PlatformInfo info = factory.info();
  if (info.name == nullptr || info.abi_version != platform_abi_version ||
      (info.capabilities & PlatformCapability::threading) !=
          PlatformCapability::threading ||
      (info.capabilities & PlatformCapability::monotonic_clock) !=
          PlatformCapability::monotonic_clock ||
      (info.capabilities & PlatformCapability::secure_random) !=
          PlatformCapability::secure_random) {
    fail(report, Check::capability_consistency);
    return false;
  }
  return true;
}

/// @brief Runs Check::no_leaked_joinable_thread.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_no_leaked_joinable_thread(PlatformFactory& factory,
                                            Report& report) {
  struct Context {
    SpinFlag ran;
  } context;
  const auto thread = factory.threading().start_thread(
      {[](void* user_context) noexcept {
         static_cast<Context*>(user_context)->ran.set();
       },
       &context});
  if (!thread) {
    fail(report, Check::no_leaked_joinable_thread);
    return false;
  }
  (void)thread.value()->join();
  if (thread.value()->joinable() || !context.ran.get()) {
    fail(report, Check::no_leaked_joinable_thread);
    return false;
  }
  return true;
}

/// @brief Runs Check::condition_unbounded_signaled.
///
/// @param factory Factory under test.
/// @param report Receives the failure, if any.
/// @return True when the check passed.
inline bool check_condition_unbounded_signaled(PlatformFactory& factory,
                                               Report& report) {
  struct Context {
    native::ConditionVariable* condition{};
    native::Mutex* mutex{};
    StopToken token;
    SpinFlag waiting;
    SpinFlag done;
    ConditionWaitStatus status{ConditionWaitStatus::timed_out};
  };
  const auto mutex = factory.threading().create_mutex();
  const auto condition = factory.threading().create_condition_variable();
  const auto stop_source = factory.threading().create_stop_source();
  if (!mutex || !condition || !stop_source) {
    fail(report, Check::condition_unbounded_signaled);
    return false;
  }
  Context context{condition.value().get(),
                  mutex.value().get(),
                  stop_source.value()->token(),
                  {},
                  {},
                  {}};
  const auto thread = factory.threading().start_thread(
      {[](void* user_context) noexcept {
         auto& state = *static_cast<Context*>(user_context);
         (void)state.mutex->lock();
         state.waiting.set();
         const auto result = state.condition->wait(*state.mutex, state.token);
         if (result) state.status = result.value();
         (void)state.mutex->unlock();
         state.done.set();
       },
       &context});
  if (!thread) {
    fail(report, Check::condition_unbounded_signaled);
    return false;
  }
  bool held = false;
  const auto abandon = [&] {
    (void)stop_source.value()->request_stop();
    condition.value()->notify_one();
    if (held) {
      (void)mutex.value()->unlock();
      held = false;
    }
    (void)thread.value()->join();
  };
  if (!spin_until([&] { return context.waiting.get(); })) {
    abandon();
    fail(report, Check::condition_unbounded_signaled);
    return false;
  }
  const auto parked = spin_until([&] {
    const auto attempt = mutex.value()->try_lock();
    if (attempt && attempt.value()) {
      held = true;
      return true;
    }
    return false;
  });
  if (!parked) {
    abandon();
    fail(report, Check::condition_unbounded_signaled);
    return false;
  }
  condition.value()->notify_one();
  if (!mutex.value()->unlock()) {
    held = false;
    (void)stop_source.value()->request_stop();
    condition.value()->notify_one();
    (void)thread.value()->join();
    fail(report, Check::condition_unbounded_signaled);
    return false;
  }
  held = false;
  if (!spin_until([&] { return context.done.get(); }) ||
      !thread.value()->join() ||
      context.status != ConditionWaitStatus::signaled ||
      stop_source.value()->token().stop_requested()) {
    (void)stop_source.value()->request_stop();
    condition.value()->notify_one();
    (void)thread.value()->join();
    fail(report, Check::condition_unbounded_signaled);
    return false;
  }
  return true;
}

}  // namespace detail

/// @brief Runs the runtime conformance checks against a factory.
///
/// A host that implements its own ThreadingProvider, ClockProvider or
/// SecureRandomProvider calls this from a test. The checks start real threads
/// and block the caller; the slowest paths wait up to two seconds each.
/// Checking stops at the first failure, so Report::failed_mask names at most
/// one check.
///
/// @param factory Factory under test.
/// @param foreign A second factory with a different threading provider
///     instance, used only to prove that mixing providers is rejected.
/// @return The report; Report::ok is true when every check passed.
inline Report run(PlatformFactory& factory, PlatformFactory& foreign) {
  Report report;
  if (!detail::check_capability_consistency(factory, report)) return report;
  if (!detail::check_thread_start_join(factory, report)) return report;
  if (!detail::check_no_leaked_joinable_thread(factory, report)) return report;
  if (!detail::check_condition_unbounded_signaled(factory, report))
    return report;
  if (!detail::check_mutex_try_lock(factory, report)) return report;
  if (!detail::check_mutex_exclusion(factory, report)) return report;
  if (!detail::check_condition_timeout(factory, report)) return report;
  if (!detail::check_condition_notify(factory, report)) return report;
  if (!detail::check_condition_cancellation(factory, report)) return report;
  if (!detail::check_condition_atomic_unlock_relock(factory, report))
    return report;
  if (!detail::check_thread_stop(factory, report)) return report;
  if (!detail::check_provider_mixing(factory, foreign, report)) return report;
  if (!detail::check_monotonic_clock(factory, report)) return report;
  if (!detail::check_random_fill(factory, report)) return report;
  return report;
}

}  // namespace libchiaking::platform::conformance
