/// @file
/// @brief Declares PendingCall, a blocking call moved onto another thread so
/// that its caller can stop waiting for it, for system calls that cannot be
/// cancelled, such as `getaddrinfo`.
#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace libchiaking::platform::runtime {

/// @brief Why PendingCall::await() returned without the call's result.
enum class CallUnfinished : std::uint8_t {
  /// @brief The caller's stop token fired; the call carries on unobserved.
  stopped,
  /// @brief The timeout passed; the call carries on unobserved.
  expired,
  /// @brief The call threw, or was destroyed without running.
  broken,
};

/// @brief The future result of work running elsewhere, awaited with a stop
/// token and a timeout.
///
/// Giving up on a call does not stop it: the work runs to completion and its
/// result is discarded. Work must therefore own everything it uses,
/// capturing by value. A call started on its own thread owns that thread and
/// joins it when destroyed; abandon() hands the thread to an owner that can
/// join it later instead. Move-only. Await from one thread at a time.
///
/// @tparam Result What the work returns.
template <class Result>
class PendingCall {
 public:
  /// @brief Starts work on a new thread that the call owns.
  ///
  /// @tparam Work A callable taking no arguments and returning Result.
  /// @param threading Starts the thread and makes the call's lock and
  ///     condition variable; must outlive the call.
  /// @param clock The monotonic clock await() measures its timeout on; must
  ///     outlive the call.
  /// @param work The work; moved onto the thread.
  /// @return The pending result.
  /// @retval PlatformError The thread could not be started.
  template <class Work>
  [[nodiscard]] static auto on_own_thread(ThreadingProvider& threading,
                                          const ClockProvider& clock, Work work)
      -> Expected<PendingCall, PlatformError> {
    auto slot = std::make_shared<Slot>(threading, clock);
    auto runner = std::make_shared<Runner<Work>>(slot, std::move(work));
    auto thread =
        Thread::start(threading, "pending call", ThreadRole::unspecified,
                      [runner = std::move(runner)] { runner->run(); });
    if (!thread) return unexpected(thread.error());
    return PendingCall{std::move(slot), std::move(thread).value()};
  }

  /// @brief Hands work to an executor, such as a thread pool.
  ///
  /// @tparam Work A callable taking no arguments and returning Result.
  /// @tparam Post A callable taking `std::function<void ()>` and returning
  ///     whether it accepted it.
  /// @param threading Makes the call's lock and condition variable; must
  ///     outlive the call.
  /// @param clock The monotonic clock await() measures its timeout on; must
  ///     outlive the call.
  /// @param work The work.
  /// @param post Queues the job that runs the work. A job it drops without
  ///     running makes the call broken.
  /// @return The pending result, or nothing when `post` refused the job.
  template <class Work, class Post>
  [[nodiscard]] static auto posted(ThreadingProvider& threading,
                                   const ClockProvider& clock, Work work,
                                   Post&& post) -> std::optional<PendingCall> {
    auto slot = std::make_shared<Slot>(threading, clock);
    auto runner = std::make_shared<Runner<Work>>(slot, std::move(work));
    if (!std::forward<Post>(post)(std::function<void()>{
            [runner = std::move(runner)] { runner->run(); }}))
      return std::nullopt;
    return PendingCall{std::move(slot), Thread{}};
  }

  /// @brief Waits for the result until it arrives, the stop token fires or
  /// the timeout passes.
  ///
  /// Blocks until then on a condition variable that the stop token also
  /// ends. A stop takes precedence over a result that arrived at the same
  /// time. After stopped or expired it may be awaited again; once a result
  /// or broken has been returned it must not.
  ///
  /// @param stop Ends the wait early.
  /// @param timeout The longest to wait.
  /// @return The work's result.
  /// @retval CallUnfinished::stopped `stop` fired.
  /// @retval CallUnfinished::expired `timeout` passed first.
  /// @retval CallUnfinished::broken The work threw, or never ran.
  [[nodiscard]] auto await(const StopToken& stop,
                           std::chrono::nanoseconds timeout)
      -> std::expected<Result, CallUnfinished> {
    return slot_->await(stop, timeout);
  }

  /// @brief Gives up on the call, handing over the thread it runs on.
  ///
  /// The caller then owns the thread and joins it, or lets it be joined by
  /// destroying it, once the work has finished. The call must not be
  /// awaited afterwards.
  ///
  /// @return The thread, or a thread that runs nothing for posted work.
  [[nodiscard]] auto abandon() noexcept -> Thread { return std::move(thread_); }

 private:
  /// @brief Where the work leaves its result, and the wait for it.
  class Slot {
   public:
    /// @brief Makes an empty slot.
    ///
    /// @param threading Makes the lock and the condition variable.
    /// @param clock The monotonic clock the wait is measured on.
    Slot(ThreadingProvider& threading, const ClockProvider& clock)
        : mutex_(threading), finished_(threading, clock) {}

    /// @brief Stores the outcome and wakes the waiter.
    ///
    /// @param result The work's result, or nothing when it is broken.
    void finish(std::optional<Result> result) {
      {
        Lock lock{mutex_};
        result_ = std::move(result);
        done_ = true;
      }
      finished_.notify_all();
    }

    /// @brief Waits as PendingCall::await() describes.
    ///
    /// @param stop Ends the wait early.
    /// @param timeout The longest to wait.
    /// @return The result, or why there is none.
    [[nodiscard]] auto await(const StopToken& stop,
                             std::chrono::nanoseconds timeout)
        -> std::expected<Result, CallUnfinished> {
      if (stop.stop_requested())
        return std::unexpected(CallUnfinished::stopped);
      Lock lock{mutex_};
      const bool ready =
          finished_.wait_for(lock, timeout, stop, [this] { return done_; });
      if (stop.stop_requested())
        return std::unexpected(CallUnfinished::stopped);
      if (!ready) return std::unexpected(CallUnfinished::expired);
      if (!result_) return std::unexpected(CallUnfinished::broken);
      return std::move(*result_);
    }

   private:
    /// @brief Guards the outcome.
    Mutex mutex_;
    /// @brief Signalled when the outcome is stored.
    ConditionVariable finished_;
    /// @brief The result; empty when the work threw or never ran.
    std::optional<Result> result_;
    /// @brief Whether the outcome is stored.
    bool done_{};
  };

  /// @brief Runs the work once and stores its outcome; marks the call broken
  /// when destroyed without having run.
  ///
  /// @tparam Work The work's type.
  template <class Work>
  class Runner {
   public:
    /// @brief Holds the work until it runs.
    ///
    /// @param slot Where the outcome goes.
    /// @param work The work.
    Runner(std::shared_ptr<Slot> slot, Work work)
        : slot_(std::move(slot)), work_(std::move(work)) {}

    Runner(const Runner&) = delete;
    auto operator=(const Runner&) -> Runner& = delete;

    /// @brief Marks the call broken when the work never ran.
    ~Runner() {
      if (!ran_) slot_->finish(std::nullopt);
    }

    /// @brief Runs the work and stores its result, or nothing when it threw.
    void run() {
      if (ran_) return;
      ran_ = true;
      try {
        slot_->finish(work_());
      } catch (...) {
        slot_->finish(std::nullopt);
      }
    }

   private:
    /// @brief Where the outcome goes.
    std::shared_ptr<Slot> slot_;
    /// @brief The work.
    Work work_;
    /// @brief Whether run() has been called.
    bool ran_{};
  };

  /// @brief Wraps started work.
  ///
  /// @param slot Where its outcome arrives.
  /// @param thread The thread it runs on, or a thread that runs nothing.
  PendingCall(std::shared_ptr<Slot> slot, Thread thread) noexcept
      : slot_(std::move(slot)), thread_(std::move(thread)) {}

  /// @brief Where the work's outcome arrives.
  std::shared_ptr<Slot> slot_;
  /// @brief The thread the work runs on; joined when the call is destroyed.
  Thread thread_;
};

}  // namespace libchiaking::platform::runtime
