/// @file
/// @brief Declares the standard library threading primitives that every
/// threading provider shipped with the library builds on, and the ledger that
/// counts what a provider made.
#pragma once

#include "libchiaking/platform/runtime/threading.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <utility>

namespace libchiaking::platform {

/// @brief Counts the threads, mutexes, condition variables and stop sources a
/// provider made that still exist.
class DefaultThreadingProvider::Ledger {
 public:
  /// @brief One counted object; it counts from construction to destruction.
  class Entry {
   public:
    /// @brief Counts one more live object in @p ledger.
    ///
    /// @param ledger The ledger of the provider that made the object.
    explicit Entry(std::shared_ptr<Ledger> ledger) noexcept
        : ledger_(std::move(ledger)) {
      ledger_->live_.fetch_add(1, std::memory_order_relaxed);
    }
    /// @brief Not copyable; each entry counts once.
    Entry(const Entry&) = delete;
    /// @brief Not copy-assignable; each entry counts once.
    auto operator=(const Entry&) -> Entry& = delete;
    /// @brief Counts the object as gone.
    ~Entry() { ledger_->live_.fetch_sub(1, std::memory_order_acq_rel); }

   private:
    /// @brief The ledger this entry counts against.
    std::shared_ptr<Ledger> ledger_;
  };

  /// @brief Ends the process when anything the provider made still exists.
  ///
  /// Called by the provider's destructor, so an object left borrowing a
  /// destroyed provider is reported where the mistake was made.
  void close() const noexcept {
    const auto live = live_.load(std::memory_order_acquire);
    if (live == 0) return;
    std::fprintf(stderr,
                 "libchiaking: threading provider destroyed while %llu "
                 "thread(s), mutex(es), condition variable(s) or stop "
                 "source(s) it made still exist\n",
                 static_cast<unsigned long long>(live));
    std::terminate();
  }

 private:
  /// @brief How many counted objects exist now.
  std::atomic<std::uint64_t> live_{0};
};

/// @brief Threads, mutexes, condition variables and stop sources built on the
/// standard library, shared by the providers that ship with the library.
class StandardThreading {
 public:
  /// @brief Starts a std::thread running @p entry.
  ///
  /// @param entry What to run.
  /// @param ledger The ledger the thread counts against.
  /// @return The handle, which joins the thread when destroyed.
  /// @retval PlatformError::thread_failure @p entry names no function, or the
  ///     system refused the thread.
  /// @retval PlatformError::out_of_memory Memory or thread resources ran out.
  [[nodiscard]] static auto start_thread(
      native::ThreadEntry entry,
      std::shared_ptr<DefaultThreadingProvider::Ledger> ledger)
      -> Expected<std::unique_ptr<native::Thread>, PlatformError>;

  /// @brief Creates a standard mutex.
  ///
  /// @param owner The provider reported as native::Mutex::owner.
  /// @param kind Plain or recursive.
  /// @param ledger The ledger the mutex counts against.
  /// @return The mutex.
  /// @retval PlatformError::out_of_memory Allocation failed.
  /// @retval PlatformError::system_failure The system could not create it.
  [[nodiscard]] static auto create_mutex(
      const ThreadingProvider& owner, MutexKind kind,
      std::shared_ptr<DefaultThreadingProvider::Ledger> ledger)
      -> Expected<std::unique_ptr<native::Mutex>, PlatformError>;

  /// @brief Creates a condition variable that waits on mutexes of @p owner.
  ///
  /// @param owner The provider whose mutexes it accepts.
  /// @param ledger The ledger the condition variable counts against.
  /// @return The condition variable.
  /// @retval PlatformError::out_of_memory Allocation failed.
  /// @retval PlatformError::system_failure The system could not create it.
  [[nodiscard]] static auto create_condition_variable(
      const ThreadingProvider& owner,
      std::shared_ptr<DefaultThreadingProvider::Ledger> ledger)
      -> Expected<std::unique_ptr<native::ConditionVariable>, PlatformError>;

  /// @brief Creates a stop source.
  ///
  /// @param ledger The ledger the source counts against.
  /// @return The source.
  /// @retval PlatformError::out_of_memory Allocation failed.
  [[nodiscard]] static auto create_stop_source(
      std::shared_ptr<DefaultThreadingProvider::Ledger> ledger)
      -> Expected<std::unique_ptr<native::StopSource>, PlatformError>;

 private:
  /// @brief The ledger entry every made object holds.
  using Entry = DefaultThreadingProvider::Ledger::Entry;
  /// @brief The clock timed waits are measured on.
  using Clock = std::chrono::steady_clock;

  class ProvidedStopSource;
  class StandardMutex;
  class StandardConditionVariable;
  class StandardThread;

  /// @brief The time @p timeout from now, saturating instead of overflowing.
  ///
  /// @param timeout How long from now.
  /// @return The deadline on Clock.
  static auto deadline_after(std::chrono::nanoseconds timeout) noexcept
      -> Clock::time_point;

  /// @brief Allocates a @p Made, reporting failure instead of throwing.
  ///
  /// @param arguments The constructor arguments.
  /// @return The object.
  /// @retval PlatformError::out_of_memory Allocation failed.
  /// @retval PlatformError::system_failure The constructor threw.
  template <class Made, class... Arguments>
  static auto make(Arguments&&... arguments)
      -> Expected<std::unique_ptr<Made>, PlatformError>;
};

}  // namespace libchiaking::platform
