/// @file
/// @brief Declares the threading port: threads, mutexes, condition variables
/// and cooperative stop signals, and DefaultThreadingProvider, the stock
/// ThreadingProvider.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/error.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/stop_token.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace libchiaking::platform {

class ThreadingProvider;

/// @brief Why a condition variable wait returned.
enum class ConditionWaitStatus : std::uint8_t {
  /// @brief A notification was observed, or the wait woke spuriously. The
  /// caller must re-check its predicate.
  signaled,
  /// @brief The timeout elapsed with no notification.
  timed_out,
  /// @brief The stop token passed to the wait was triggered.
  stop_requested,
};

/// @brief Whether a mutex may be locked again by the thread that holds it.
enum class MutexKind : std::uint8_t {
  /// @brief Not recursive; a thread must not lock it twice.
  plain,
  /// @brief The owning thread may lock it repeatedly and must unlock it as
  /// many times.
  recursive,
};

/// @brief Label describing the work a library thread does.
///
/// It is passed to a ThreadAffinityCallback so a host can choose a core or
/// priority for each kind of thread. It does not change what the thread runs.
enum class ThreadRole : std::uint8_t {
  /// @brief No particular role; the default.
  unspecified,
  /// @brief Control and signalling channels. The PlayStation Network
  /// signalling thread carries this role.
  control,
  /// @brief Congestion control.
  congestion,
  /// @brief Console discovery.
  discovery,
  /// @brief The long-running discovery service.
  discovery_service,
  /// @brief Stream transport.
  transport,
  /// @brief The sending side of stream transport.
  transport_send,
  /// @brief The sending side of reliable UDP.
  rudp_send,
  /// @brief Internet route establishment. The hole-punching thread carries
  /// this role.
  holepunch,
  /// @brief Controller feedback.
  feedback,
  /// @brief Session management.
  session,
  /// @brief Console registration.
  registration,
  /// @brief Cryptographic work.
  crypto,
};

/// @brief Hook a provider runs on a new thread before the thread's entry
/// function.
///
/// The stock provider calls it once, on the new thread, after naming the
/// thread. It must not throw.
///
/// @param role The role recorded in the thread's native::ThreadEntry.
/// @param user The native::ThreadEntry::affinity_user pointer, passed back
///     unchanged.
using ThreadAffinityCallback = void (*)(ThreadRole role, void* user) noexcept;

namespace native {

/// @brief Everything a ThreadingProvider needs to start one thread.
///
/// The library fills one in and passes it by value to
/// ThreadingProvider::start_thread. The context pointers are borrowed: whoever
/// starts the thread keeps them alive until the thread has finished.
struct ThreadEntry {
  /// @brief Describes a thread that runs a function returning nothing.
  ///
  /// @param entry_function Function the thread runs.
  /// @param entry_context Pointer handed to that function.
  constexpr ThreadEntry(void (*entry_function)(void*) noexcept,
                        void* entry_context) noexcept
      : function(entry_function), context(entry_context) {}

  /// @brief Function the thread runs when result_function is null.
  void (*function)(void* context) noexcept;
  /// @brief Pointer handed to function or result_function.
  void* context;
  /// @brief Optional function that runs in place of function and whose return
  /// value is kept for Thread::result.
  void* (*result_function)(void* context) noexcept {};
  /// @brief Name to give the thread for debuggers and profilers; empty for
  /// none. The provider must copy it before start_thread returns and may
  /// shorten it to what the system allows.
  std::string_view name;
  /// @brief Kind of work the thread does, forwarded to affinity_callback.
  ThreadRole role{ThreadRole::unspecified};
  /// @brief Optional hook run on the new thread before the entry function.
  ThreadAffinityCallback affinity_callback{};
  /// @brief Pointer handed back to affinity_callback.
  void* affinity_user{};
};

/// @brief Handle to a thread started by a ThreadingProvider.
///
/// Implemented by the provider. The library owns the handle and may use it from
/// a different thread than the one that started it.
class Thread {
 public:
  /// @brief Releases the handle.
  ///
  /// @warning With the stock provider this joins the thread, so it blocks
  ///     until the entry function returns. Use join_for() first when teardown
  ///     must not wait indefinitely.
  virtual ~Thread() = default;
  /// @brief Whether the thread still has to be joined.
  ///
  /// @return True from creation until a join succeeds, false afterwards.
  virtual bool joinable() const noexcept = 0;
  /// @brief Blocks until the thread's entry function has returned.
  ///
  /// @return Nothing once the thread has been joined; joinable() is then
  ///     false.
  /// @retval PlatformError::thread_failure The call was made from the thread
  ///     itself, the thread was already joined or is being joined elsewhere,
  ///     or the system reported an error.
  [[nodiscard]] virtual auto join() -> Expected<void, PlatformError> = 0;
  /// @brief Waits a bounded time for the thread to finish, and joins it if it
  /// does.
  ///
  /// @param timeout Longest time to wait. Zero or negative checks without
  ///     waiting.
  /// @return True when the thread finished and was joined. False when the
  ///     timeout elapsed; the thread is still running and still joinable.
  /// @retval PlatformError::thread_failure The call was made from the thread
  ///     itself, the thread was already joined or is being joined elsewhere,
  ///     or the system reported an error.
  /// @retval PlatformError::system_failure Waiting for completion failed.
  [[nodiscard]] virtual auto join_for(std::chrono::nanoseconds timeout)
      -> Expected<bool, PlatformError> = 0;
  /// @brief The value the thread's entry produced.
  ///
  /// @return The pointer returned by ThreadEntry::result_function, or
  ///     nullptr when the thread ran ThreadEntry::function.
  /// @retval PlatformError::thread_failure The entry function has not
  ///     returned yet.
  [[nodiscard]] virtual auto result() const
      -> Expected<void*, PlatformError> = 0;
};

/// @brief A mutual exclusion lock created by a ThreadingProvider.
///
/// Implemented by the provider. Every operation reports failure through its
/// return value and never throws. Library code holds one through Mutex and
/// Lock.
class Mutex {
 public:
  /// @brief Destroys the mutex.
  ///
  /// @pre No thread holds or waits for it.
  virtual ~Mutex() = default;
  /// @brief The provider that created this mutex.
  ///
  /// A condition variable compares it with its own owner to reject a mutex
  /// from another provider.
  ///
  /// @return The provider passed at construction; never changes.
  auto owner() const noexcept -> const ThreadingProvider& { return *owner_; }
  /// @brief Blocks until the calling thread holds the mutex.
  ///
  /// @return Nothing once the mutex is held.
  /// @retval PlatformError::system_failure The system refused the lock; the
  ///     mutex is not held.
  [[nodiscard]] virtual auto lock() -> Expected<void, PlatformError> = 0;
  /// @brief Takes the mutex only if that needs no waiting.
  ///
  /// @return True when the calling thread now holds the mutex, false when
  ///     another holder has it. A busy mutex is an answer, not an error.
  /// @retval PlatformError::system_failure The system reported an error.
  [[nodiscard]] virtual auto try_lock() -> Expected<bool, PlatformError> = 0;
  /// @brief Releases the mutex.
  ///
  /// @pre The calling thread holds it.
  /// @return Nothing once released.
  /// @retval PlatformError::system_failure The system refused, for example
  ///     because the caller is not the holder.
  [[nodiscard]] virtual auto unlock() -> Expected<void, PlatformError> = 0;

 protected:
  /// @brief Records the creating provider; called by a provider's mutex type.
  ///
  /// @param owner Provider that creates the mutex. It must outlive the mutex.
  explicit Mutex(const ThreadingProvider& owner) noexcept : owner_(&owner) {}

  /// @brief The creating provider; never null.
  const ThreadingProvider* owner_;
};

/// @brief A condition variable created by a ThreadingProvider.
///
/// Implemented by the provider. Every wait must be entered with the mutex held
/// by the caller, releases it while parked, and holds it again on return,
/// whatever the outcome. The mutex must come from the same provider instance.
class ConditionVariable {
 public:
  /// @brief Destroys the condition variable.
  ///
  /// @pre No thread is waiting on it.
  virtual ~ConditionVariable() = default;
  /// @brief The provider that created this condition variable.
  ///
  /// @return The provider passed at construction; never changes.
  auto owner() const noexcept -> const ThreadingProvider& { return *owner_; }
  /// @brief Wakes one thread waiting on this condition variable, if there is
  /// one.
  virtual void notify_one() noexcept = 0;
  /// @brief Wakes every thread waiting on this condition variable.
  virtual void notify_all() noexcept = 0;
  /// @brief Waits until notified, with no timeout and no cancellation.
  ///
  /// @param mutex Mutex the caller holds; from the same provider.
  /// @return Nothing when woken. The wake-up may be spurious, so the caller
  ///     re-checks its predicate and waits again if needed.
  /// @retval PlatformError::wrong_provider @p mutex belongs to another
  ///     provider.
  /// @retval PlatformError::system_failure The system reported an error.
  [[nodiscard]] virtual auto wait(Mutex& mutex)
      -> Expected<void, PlatformError> = 0;
  /// @brief Waits until notified or until a stop is requested.
  ///
  /// @param mutex Mutex the caller holds; from the same provider.
  /// @param stop_token Token whose stop request ends the wait without a
  ///     notification.
  /// @return ConditionWaitStatus::signaled after a notification, or
  ///     ConditionWaitStatus::stop_requested once the token is triggered,
  ///     including when it already was on entry.
  /// @retval PlatformError::wrong_provider @p mutex belongs to another
  ///     provider.
  /// @retval PlatformError::system_failure The system reported an error.
  [[nodiscard]] virtual auto wait(Mutex& mutex, const StopToken& stop_token)
      -> Expected<ConditionWaitStatus, PlatformError> = 0;
  /// @brief Waits until notified or until a timeout elapses.
  ///
  /// @param mutex Mutex the caller holds; from the same provider.
  /// @param timeout Longest time to wait, measured on a monotonic clock. Zero
  ///     or negative times out at once.
  /// @return ConditionWaitStatus::signaled or ConditionWaitStatus::timed_out.
  /// @retval PlatformError::wrong_provider @p mutex belongs to another
  ///     provider.
  /// @retval PlatformError::system_failure The system reported an error.
  [[nodiscard]] virtual auto wait_for(Mutex& mutex,
                                      std::chrono::nanoseconds timeout)
      -> Expected<ConditionWaitStatus, PlatformError> = 0;
  /// @brief Waits until notified, until a timeout elapses, or until a stop is
  /// requested.
  ///
  /// @param mutex Mutex the caller holds; from the same provider.
  /// @param timeout Longest time to wait, measured on a monotonic clock.
  /// @param stop_token Token whose stop request ends the wait without a
  ///     notification.
  /// @return Whichever of the three ConditionWaitStatus outcomes ended the
  ///     wait. A token already triggered on entry gives
  ///     ConditionWaitStatus::stop_requested at once.
  /// @retval PlatformError::wrong_provider @p mutex belongs to another
  ///     provider.
  /// @retval PlatformError::system_failure The system reported an error.
  [[nodiscard]] virtual auto wait_for(Mutex& mutex,
                                      std::chrono::nanoseconds timeout,
                                      const StopToken& stop_token)
      -> Expected<ConditionWaitStatus, PlatformError> = 0;

 protected:
  /// @brief Records the creating provider; called by a provider's condition
  /// variable type.
  ///
  /// @param owner Provider that creates the condition variable. It must
  ///     outlive it.
  explicit ConditionVariable(const ThreadingProvider& owner) noexcept
      : owner_(&owner) {}

  /// @brief The creating provider; never null.
  const ThreadingProvider* owner_;
};

}  // namespace native

/// @brief Creates the threads and synchronisation objects the library runs on.
///
/// Implemented by the host frontend, or by DefaultThreadingProvider. The
/// library reaches it through PlatformFactory::threading and borrows it for as
/// long as anything built from that factory exists. Its methods are called from
/// the frontend's threads and from threads the provider itself started, so they
/// must be safe to call concurrently. Each object returned is owned by the
/// caller, and the provider must outlive all of them.
///
/// conformance::run() checks an implementation against what the library relies
/// on.
///
/// Library code reaches it through Thread, Mutex, Lock, ConditionVariable and
/// Sleeper, so every library thread, lock and pause comes from this
/// provider.
/// @ingroup api_platform
class ThreadingProvider {
 public:
  /// @brief Destroys the provider. The library never deletes a provider it
  /// was given.
  ///
  /// @pre Every thread, mutex, condition variable and stop source it created
  ///     has been destroyed.
  virtual ~ThreadingProvider() = default;
  /// @brief Starts a thread running the given entry.
  ///
  /// The new thread runs native::ThreadEntry::affinity_callback if set, then
  /// native::ThreadEntry::result_function if set, otherwise
  /// native::ThreadEntry::function.
  ///
  /// @param entry What to run. The provider copies what it needs before
  ///     returning.
  /// @return A handle the caller uses to join the thread.
  /// @retval PlatformError::thread_failure The thread could not be created.
  /// @retval PlatformError::out_of_memory Memory or thread resources were
  ///     exhausted.
  [[nodiscard]] virtual auto start_thread(native::ThreadEntry entry)
      -> Expected<std::unique_ptr<native::Thread>, PlatformError> = 0;
  /// @brief Creates a plain, non-recursive mutex.
  ///
  /// @return The mutex, or the error create_mutex(MutexKind) reports.
  [[nodiscard]] auto create_mutex()
      -> Expected<std::unique_ptr<native::Mutex>, PlatformError> {
    return create_mutex(MutexKind::plain);
  }
  /// @brief Creates a mutex of the requested kind.
  ///
  /// @param kind Plain or recursive.
  /// @return The mutex, owned by the caller, with native::Mutex::owner set to
  ///     this provider.
  /// @retval PlatformError::out_of_memory Allocation failed.
  /// @retval PlatformError::system_failure The system could not create it.
  [[nodiscard]] virtual auto create_mutex(MutexKind kind)
      -> Expected<std::unique_ptr<native::Mutex>, PlatformError> = 0;
  /// @brief Creates a condition variable for use with this provider's
  /// mutexes.
  ///
  /// @return The condition variable, owned by the caller.
  /// @retval PlatformError::out_of_memory Allocation failed.
  /// @retval PlatformError::system_failure The system could not create it.
  [[nodiscard]] virtual auto create_condition_variable()
      -> Expected<std::unique_ptr<native::ConditionVariable>,
                  PlatformError> = 0;
  /// @brief Creates a stop source whose tokens this provider's waits honour.
  ///
  /// @return The source, owned by the caller, with no stop requested.
  /// @retval PlatformError::out_of_memory Allocation failed.
  [[nodiscard]] virtual auto create_stop_source()
      -> Expected<std::unique_ptr<native::StopSource>, PlatformError> = 0;
};

/// @brief Names one thread so code can ask whether it runs on a thread it
/// recorded earlier.
///
/// Each thread receives a number the first time it asks, counted up from one
/// for the whole process, so an identity is never given to another thread,
/// even after its own thread ends. A default-constructed identity names no
/// thread. Trivially copyable, so it fits in a `std::atomic`.
class ThreadIdentity {
 public:
  /// @brief Makes an identity that names no thread.
  constexpr ThreadIdentity() noexcept = default;

  /// @brief Names the calling thread.
  ///
  /// @return The calling thread's identity; the same on every call from one
  ///     thread.
  [[nodiscard]] static auto current() noexcept -> ThreadIdentity {
    static std::atomic<std::uint64_t> issued{0};
    static thread_local const std::uint64_t mine =
        issued.fetch_add(1, std::memory_order_relaxed) + 1;
    return ThreadIdentity{mine};
  }

  /// @brief Compares two identities.
  ///
  /// @return `true` when both name the same thread, or both name none.
  constexpr auto operator==(const ThreadIdentity&) const noexcept
      -> bool = default;

  /// @brief Reports whether this identity names a thread.
  ///
  /// @return `true` unless default-constructed.
  constexpr explicit operator bool() const noexcept { return number_ != 0; }

 private:
  /// @brief Wraps a thread's number.
  ///
  /// @param number The number, never zero.
  constexpr explicit ThreadIdentity(std::uint64_t number) noexcept
      : number_(number) {}

  /// @brief The thread's number; zero for no thread.
  std::uint64_t number_{};
};

/// @brief A mutex created by a ThreadingProvider and owned for its lifetime.
///
/// Library code holds one of these instead of a standard mutex, so every lock
/// runs on the host's provider. Lock is the way to hold it. A provider that
/// cannot create or lock it has failed in a way no caller can act on, the same
/// as an allocation failure, so the process ends through std::terminate.
///
/// Not copyable or movable, like the standard mutex it replaces. An owner that
/// must move puts it behind a std::unique_ptr.
class Mutex {
 public:
  /// @brief Creates the mutex through the provider.
  ///
  /// @param threading Provider to create it with. It must outlive the mutex.
  /// @param kind Plain or recursive.
  explicit Mutex(ThreadingProvider& threading,
                 MutexKind kind = MutexKind::plain)
      : native_(required(threading.create_mutex(kind))) {}

  /// @brief Not copyable; a mutex has one identity.
  Mutex(const Mutex&) = delete;
  /// @brief Not copy-assignable; a mutex has one identity.
  auto operator=(const Mutex&) -> Mutex& = delete;
  /// @brief Not movable; a waiter may hold its address.
  Mutex(Mutex&&) = delete;
  /// @brief Not move-assignable; a waiter may hold its address.
  auto operator=(Mutex&&) -> Mutex& = delete;
  /// @brief Destroys the mutex.
  ///
  /// @pre No thread holds or waits for it.
  ~Mutex() = default;

  /// @brief Blocks until the calling thread holds the mutex.
  void lock() { required(native_->lock()); }
  /// @brief Takes the mutex only if that needs no waiting.
  ///
  /// @return True when the calling thread now holds the mutex.
  [[nodiscard]] auto try_lock() -> bool {
    return required(native_->try_lock());
  }
  /// @brief Releases the mutex.
  ///
  /// @pre The calling thread holds it.
  void unlock() { required(native_->unlock()); }

  /// @brief The provider's mutex, for ConditionVariable.
  ///
  /// @return The mutex created at construction.
  auto native() noexcept -> native::Mutex& { return *native_; }

 private:
  template <class Value>
  static auto required(Expected<Value, PlatformError> made) -> Value {
    if (!made) std::terminate();
    if constexpr (!std::is_void_v<Value>) return std::move(made).value();
  }

  std::unique_ptr<native::Mutex> native_;
};

/// @brief Holds a Mutex and releases it on destruction.
///
/// The only lock guard library code uses. It holds the mutex from construction
/// unless made with deferred() or try_to_take(), and can release and retake it
/// within its scope, which ConditionVariable relies on while it waits. Using it
/// against its preconditions ends the process through std::terminate.
class Lock {
 public:
  /// @brief Locks the mutex, blocking until it is available.
  ///
  /// @param mutex Mutex to hold. It must outlive the lock.
  explicit Lock(Mutex& mutex) : mutex_(&mutex) {
    mutex_->lock();
    owns_ = true;
  }

  /// @brief A lock on the mutex that does not hold it yet.
  ///
  /// @param mutex Mutex to hold later through lock(). It must outlive the
  ///     lock.
  /// @return A lock with owns_lock() false.
  [[nodiscard]] static auto deferred(Mutex& mutex) noexcept -> Lock {
    return Lock{mutex, false};
  }

  /// @brief Takes the mutex only if that needs no waiting.
  ///
  /// @param mutex Mutex to hold. It must outlive the lock.
  /// @return A lock whose owns_lock() says whether the mutex was taken.
  [[nodiscard]] static auto try_to_take(Mutex& mutex) -> Lock {
    return Lock{mutex, mutex.try_lock()};
  }

  /// @brief Not copyable; a held lock has one owner.
  Lock(const Lock&) = delete;
  /// @brief Not copy-assignable; a held lock has one owner.
  auto operator=(const Lock&) -> Lock& = delete;

  /// @brief Takes over another lock; the other no longer releases the mutex.
  ///
  /// @param other Lock to take over.
  Lock(Lock&& other) noexcept
      : mutex_(other.mutex_), owns_(std::exchange(other.owns_, false)) {}

  /// @brief Releases any mutex this lock holds, then takes over another lock.
  ///
  /// @param other Lock to take over.
  /// @return This lock.
  auto operator=(Lock&& other) noexcept -> Lock& {
    if (this != &other) {
      release();
      mutex_ = other.mutex_;
      owns_ = std::exchange(other.owns_, false);
    }
    return *this;
  }

  /// @brief Releases the mutex if this lock holds it.
  ~Lock() { release(); }

  /// @brief Takes the mutex again, blocking until it is available.
  ///
  /// @pre This lock does not hold it.
  void lock() {
    if (owns_) std::terminate();
    mutex_->lock();
    owns_ = true;
  }

  /// @brief Releases the mutex before the lock goes out of scope.
  ///
  /// @pre This lock holds it.
  void unlock() {
    if (!owns_) std::terminate();
    owns_ = false;
    mutex_->unlock();
  }

  /// @brief Whether this lock holds its mutex.
  ///
  /// @return True from a successful lock until unlock or a move away.
  auto owns_lock() const noexcept -> bool { return owns_; }

  /// @brief The mutex this lock is for.
  ///
  /// @return The mutex given at construction.
  auto mutex() const noexcept -> Mutex& { return *mutex_; }

 private:
  Lock(Mutex& mutex, bool owns) noexcept : mutex_(&mutex), owns_(owns) {}

  void release() noexcept {
    if (std::exchange(owns_, false)) mutex_->unlock();
  }

  Mutex* mutex_;
  bool owns_{false};
};

/// @brief A condition variable created by a ThreadingProvider and owned for
/// its lifetime.
///
/// Library code holds one of these instead of a standard condition variable.
/// Every wait takes a Lock that holds its Mutex, releases it while parked and
/// holds it again on return. Waits that take a StopToken return as soon
/// as a stop is requested, without anyone notifying. A timed wait that
/// re-checks a condition ends when the provider's own timeout fires, and after
/// each earlier wake-up it waits only for what is left of the timeout as read
/// on the ClockProvider given at construction; a clock that does not move, such
/// as a test's manual clock, makes it wait longer but never forever. A provider
/// failure ends the process through std::terminate, as for Mutex.
class ConditionVariable {
 public:
  /// @brief Creates the condition variable through the provider.
  ///
  /// @param threading Provider to create it with; the mutexes it waits with
  ///     must come from the same provider. It must outlive the condition
  ///     variable.
  /// @param clock Monotonic clock that timed waits with a condition measure
  ///     the time left on. It must outlive the condition variable.
  ConditionVariable(ThreadingProvider& threading, const ClockProvider& clock)
      : native_(required(threading.create_condition_variable())),
        clock_(&clock) {}

  /// @brief Not copyable; waiters hold its address.
  ConditionVariable(const ConditionVariable&) = delete;
  /// @brief Not copy-assignable; waiters hold its address.
  auto operator=(const ConditionVariable&) -> ConditionVariable& = delete;
  /// @brief Not movable; waiters hold its address.
  ConditionVariable(ConditionVariable&&) = delete;
  /// @brief Not move-assignable; waiters hold its address.
  auto operator=(ConditionVariable&&) -> ConditionVariable& = delete;
  /// @brief Destroys the condition variable.
  ///
  /// @pre No thread is waiting on it.
  ~ConditionVariable() = default;

  /// @brief Wakes one waiting thread, if there is one.
  void notify_one() noexcept { native_->notify_one(); }
  /// @brief Wakes every waiting thread.
  void notify_all() noexcept { native_->notify_all(); }

  /// @brief Waits until notified. The wake-up may be spurious.
  ///
  /// @param lock Lock that holds the mutex.
  void wait(Lock& lock) { required(native_->wait(held(lock))); }

  /// @brief Waits until a condition holds.
  ///
  /// @param lock Lock that holds the mutex the condition is read under.
  /// @param ready Condition, called with the mutex held.
  template <class Ready>
  void wait(Lock& lock, Ready ready) {
    while (!ready()) wait(lock);
  }

  /// @brief Waits until notified or until a stop is requested.
  ///
  /// @param lock Lock that holds the mutex.
  /// @param stop Token whose stop request ends the wait.
  /// @return ConditionWaitStatus::signaled or
  ///     ConditionWaitStatus::stop_requested.
  [[nodiscard]] auto wait(Lock& lock, const StopToken& stop)
      -> ConditionWaitStatus {
    return required(native_->wait(held(lock), stop));
  }

  /// @brief Waits until a condition holds or a stop is requested.
  ///
  /// @param lock Lock that holds the mutex the condition is read under.
  /// @param stop Token whose stop request ends the wait.
  /// @param ready Condition, called with the mutex held.
  /// @return True when the condition holds, false when a stop ended the wait
  ///     first.
  template <class Ready>
  [[nodiscard]] auto wait(Lock& lock, const StopToken& stop, Ready ready)
      -> bool {
    while (!ready()) {
      if (wait(lock, stop) == ConditionWaitStatus::stop_requested)
        return ready();
    }
    return true;
  }

  /// @brief Waits until notified or until a timeout elapses.
  ///
  /// @param lock Lock that holds the mutex.
  /// @param timeout Longest time to wait.
  /// @return ConditionWaitStatus::signaled or ConditionWaitStatus::timed_out.
  [[nodiscard]] auto wait_for(Lock& lock, std::chrono::nanoseconds timeout)
      -> ConditionWaitStatus {
    return required(native_->wait_for(held(lock), timeout));
  }

  /// @brief Waits until notified, until a timeout elapses, or until a stop is
  /// requested.
  ///
  /// @param lock Lock that holds the mutex.
  /// @param timeout Longest time to wait.
  /// @param stop Token whose stop request ends the wait.
  /// @return Whichever of the three outcomes ended the wait.
  [[nodiscard]] auto wait_for(Lock& lock, std::chrono::nanoseconds timeout,
                              const StopToken& stop) -> ConditionWaitStatus {
    return required(native_->wait_for(held(lock), timeout, stop));
  }

  /// @brief Waits until a condition holds or a timeout elapses.
  ///
  /// @param lock Lock that holds the mutex the condition is read under.
  /// @param timeout Longest time to wait in all, measured on the clock given
  ///     at construction.
  /// @param ready Condition, called with the mutex held.
  /// @return The condition's value when the wait ended.
  template <class Ready>
  [[nodiscard]] auto wait_for(Lock& lock, std::chrono::nanoseconds timeout,
                              Ready ready) -> bool {
    return wait_for(lock, timeout, StopToken{}, std::move(ready));
  }

  /// @brief Waits until a condition holds, a timeout elapses or a stop is
  /// requested.
  ///
  /// @param lock Lock that holds the mutex the condition is read under.
  /// @param timeout Longest time to wait in all, measured on the clock given
  ///     at construction.
  /// @param stop Token whose stop request ends the wait.
  /// @param ready Condition, called with the mutex held.
  /// @return The condition's value when the wait ended.
  template <class Ready>
  [[nodiscard]] auto wait_for(Lock& lock, std::chrono::nanoseconds timeout,
                              const StopToken& stop, Ready ready) -> bool {
    const auto started = clock_->now_microseconds();
    const auto budget =
        std::chrono::duration_cast<std::chrono::microseconds>(timeout);
    while (!ready()) {
      const auto spent = std::chrono::microseconds{
          static_cast<std::int64_t>(clock_->now_microseconds() - started)};
      if (spent >= budget) return false;
      if (wait_for(lock, budget - spent, stop) != ConditionWaitStatus::signaled)
        return ready();
    }
    return true;
  }

 private:
  template <class Value>
  static auto required(Expected<Value, PlatformError> made) -> Value {
    if (!made) std::terminate();
    if constexpr (!std::is_void_v<Value>) return std::move(made).value();
  }

  static auto held(Lock& lock) -> native::Mutex& {
    if (!lock.owns_lock()) std::terminate();
    return lock.mutex().native();
  }

  std::unique_ptr<native::ConditionVariable> native_;
  const ClockProvider* clock_;
};

/// @brief Pauses the calling thread on a ThreadingProvider, and wakes early
/// when asked to stop.
///
/// Library code pauses through this type instead of sleeping the thread
/// directly, so a pause runs on the host's provider and a stop request cuts it
/// short. One sleeper may be shared by any number of threads; each pause is
/// independent.
class Sleeper {
 public:
  /// @brief Makes a sleeper.
  ///
  /// @param threading Provider of the pause's mutex and condition variable.
  ///     It must outlive the sleeper.
  /// @param clock Monotonic clock a pause measures on. It must outlive the
  ///     sleeper.
  Sleeper(ThreadingProvider& threading, const ClockProvider& clock)
      : mutex_(threading), parked_(threading, clock) {}

  /// @brief Pauses for a duration, or until a stop is requested.
  ///
  /// @param duration How long to pause. Zero or negative returns at once.
  /// @param stop Token whose stop request ends the pause early.
  /// @return True when the whole duration passed, false when a stop ended
  ///     the pause first.
  auto sleep_for(std::chrono::nanoseconds duration, const StopToken& stop = {})
      -> bool {
    Lock lock{mutex_};
    return !parked_.wait_for(lock, duration, stop,
                             [&] { return stop.stop_requested(); });
  }

 private:
  Mutex mutex_;
  ConditionVariable parked_;
};

/// @brief A thread started by a ThreadingProvider, joined when the handle
/// goes.
///
/// Library code starts every thread through this type, so a host names, pins
/// and limits all of them through its provider. Like std::jthread, it carries
/// a stop source: the work receives its token, and destroying or reassigning
/// the handle requests a stop and then joins. A default-constructed handle
/// runs nothing.
class Thread {
 public:
  /// @brief A handle that runs nothing.
  Thread() noexcept = default;

  /// @brief Starts a thread running a callable.
  ///
  /// @param threading Provider to start it with. It must outlive the thread.
  /// @param name Name the provider gives the thread, for debuggers and
  ///     profilers.
  /// @param role Kind of work, which the provider may use to choose a core,
  ///     priority or stack size.
  /// @param work Callable run on the new thread with the handle's stop token.
  ///     It is moved into the thread and destroyed there when it returns.
  /// @return The running thread.
  /// @retval PlatformError::thread_failure The thread could not be created.
  /// @retval PlatformError::out_of_memory Memory or the process's thread
  ///     allowance ran out.
  template <class Work>
  [[nodiscard]] static auto start(ThreadingProvider& threading,
                                  std::string_view name, ThreadRole role,
                                  Work work)
      -> Expected<Thread, PlatformError> {
    auto shared =
        std::make_shared<Shared>(std::make_unique<Body<Work>>(std::move(work)));
    auto runner = std::make_unique<std::shared_ptr<Shared>>(shared);
    native::ThreadEntry entry{&Thread::run, runner.get()};
    entry.name = name;
    entry.role = role;
    auto started = threading.start_thread(entry);
    if (!started) return unexpected(started.error());
    static_cast<void>(runner.release());
    return Thread{std::move(shared), std::move(started).value()};
  }

  /// @brief Not copyable; a thread has one handle.
  Thread(const Thread&) = delete;
  /// @brief Not copy-assignable; a thread has one handle.
  auto operator=(const Thread&) -> Thread& = delete;
  /// @brief Takes over another handle, which then runs nothing.
  ///
  /// @param other Handle to take over.
  Thread(Thread&& other) noexcept = default;
  /// @brief Stops and joins any thread this handle runs, then takes over
  /// another handle.
  ///
  /// @param other Handle to take over.
  /// @return This handle.
  auto operator=(Thread&& other) noexcept -> Thread& {
    if (this != &other) {
      finish();
      shared_ = std::move(other.shared_);
      native_ = std::move(other.native_);
    }
    return *this;
  }
  /// @brief Requests a stop and joins the thread, if there is one.
  ///
  /// @warning Blocks until the work returns. Called on the thread itself, the
  ///     provider detaches it instead.
  ~Thread() { finish(); }

  /// @brief Asks the work to stop, through the token it was given.
  ///
  /// @return True only for the call that first requested the stop.
  auto request_stop() noexcept -> bool {
    return shared_ != nullptr && shared_->stop.request_stop();
  }

  /// @brief The token the work receives.
  ///
  /// @return The token, or one that never stops for an empty handle.
  auto stop_token() const noexcept -> StopToken {
    return shared_ != nullptr ? shared_->stop.token() : StopToken{};
  }

  /// @brief Whether there is a thread still to join.
  ///
  /// @return False for an empty handle and after a join.
  auto joinable() const noexcept -> bool {
    return native_ != nullptr && native_->joinable();
  }

  /// @brief Blocks until the work returns.
  ///
  /// @return Nothing once joined.
  /// @retval PlatformError::thread_failure There is no thread to join, the
  ///     call was made from the thread itself, or the system refused.
  [[nodiscard]] auto join() -> Expected<void, PlatformError> {
    if (native_ == nullptr) return unexpected(PlatformError::thread_failure);
    return native_->join();
  }

  /// @brief Waits a bounded time for the work to return, and joins if it
  /// does.
  ///
  /// @param timeout Longest time to wait.
  /// @return True when joined, false when the timeout elapsed first.
  /// @retval PlatformError::thread_failure There is no thread to join, the
  ///     call was made from the thread itself, or the system refused.
  /// @retval PlatformError::system_failure Waiting failed.
  [[nodiscard]] auto join_for(std::chrono::nanoseconds timeout)
      -> Expected<bool, PlatformError> {
    if (native_ == nullptr) return unexpected(PlatformError::thread_failure);
    return native_->join_for(timeout);
  }

 private:
  /// @brief The work a Thread runs, with its type erased.
  class Job {
   public:
    /// @brief Destroys the work.
    virtual ~Job() = default;
    /// @brief Runs the work on the new thread.
    ///
    /// @param stop Token of the Thread that runs it.
    virtual void run(StopToken stop) = 0;
  };

  /// @brief A Job holding one callable.
  ///
  /// @tparam Work The callable, taking a StopToken or nothing.
  template <class Work>
  class Body final : public Job {
   public:
    /// @brief Takes the callable.
    ///
    /// @param work Callable to run.
    explicit Body(Work work) : work_(std::move(work)) {}
    /// @brief Calls the callable, with the token if it takes one.
    ///
    /// @param stop Token of the Thread that runs it.
    void run(StopToken stop) override {
      if constexpr (std::is_invocable_v<Work&, StopToken>)
        work_(std::move(stop));
      else
        work_();
    }

   private:
    Work work_;
  };

  /// @brief What the handle and the running thread share: the work and the
  /// stop source. Kept alive by whichever of the two lasts longer.
  class Shared {
   public:
    /// @brief Takes the work.
    ///
    /// @param work Work to run.
    explicit Shared(std::unique_ptr<Job> work) : job(std::move(work)) {}

    /// @brief The work, released on the thread once it returns.
    std::unique_ptr<Job> job;
    /// @brief The source whose token the work receives.
    StopSource stop;
  };

  Thread(std::shared_ptr<Shared> shared,
         std::unique_ptr<native::Thread> native) noexcept
      : shared_(std::move(shared)), native_(std::move(native)) {}

  static void run(void* context) noexcept {
    const std::unique_ptr<std::shared_ptr<Shared>> runner{
        static_cast<std::shared_ptr<Shared>*>(context)};
    auto& shared = **runner;
    shared.job->run(shared.stop.token());
    shared.job.reset();
  }

  void finish() noexcept {
    if (native_ == nullptr) return;
    static_cast<void>(request_stop());
    native_.reset();
    shared_.reset();
  }

  std::shared_ptr<Shared> shared_;
  std::unique_ptr<native::Thread> native_;
};

/// @brief Stock ThreadingProvider built on the C++ standard library's threads.
///
/// Threads are std::thread, mutexes std::mutex or std::recursive_mutex, and
/// condition variables std::condition_variable_any; only naming a new thread
/// goes to the operating system. Its only state is a count of the threads,
/// mutexes, condition variables and stop sources it made that still exist, so
/// every method may be called from any thread at once. DefaultPlatformFactory
/// uses it unless
/// the host supplies another provider. Timeouts are measured on
/// std::chrono::steady_clock, and stop-aware waits check their token at least
/// once a millisecond.
class DefaultThreadingProvider final : public ThreadingProvider {
 public:
  /// @brief Counts what the provider made that still exists; defined with
  /// the provider's implementation.
  class Ledger;

  /// @brief Constructs the provider; there is nothing to configure.
  DefaultThreadingProvider();
  /// @brief Not copyable; what it made counts against this instance.
  DefaultThreadingProvider(const DefaultThreadingProvider&) = delete;
  /// @brief Not copy-assignable; what it made counts against this instance.
  auto operator=(const DefaultThreadingProvider&)
      -> DefaultThreadingProvider& = delete;
  /// @brief Destroys the provider.
  ///
  /// Anything it made that still exists would be left borrowing a provider
  /// that is gone, so that is treated as the programming error it is: the
  /// process ends through std::terminate, naming how many objects remain,
  /// instead of failing later wherever one of them is next used.
  ~DefaultThreadingProvider() override;
  /// @brief Keeps the plain-mutex convenience overload visible alongside the
  /// override.
  ///
  /// @return A plain mutex, or the error create_mutex(MutexKind) reports.
  using ThreadingProvider::create_mutex;

  /// @brief Starts a std::thread. See ThreadingProvider::start_thread.
  ///
  /// The entry's name is copied before the call returns, which does not wait
  /// for the new thread to run. The returned handle
  /// joins the thread when destroyed; a handle destroyed on its own thread
  /// detaches instead.
  ///
  /// @param entry What to run.
  /// @return The thread handle.
  /// @retval PlatformError::thread_failure @p entry names no function, or the
  ///     system refused to create the thread.
  /// @retval PlatformError::out_of_memory Memory or thread resources were
  ///     exhausted.
  [[nodiscard]] auto start_thread(native::ThreadEntry entry)
      -> Expected<std::unique_ptr<native::Thread>, PlatformError> override;
  /// @brief Creates a standard mutex owned by this provider. See
  /// ThreadingProvider::create_mutex.
  ///
  /// @param kind Plain or recursive.
  /// @return The mutex.
  /// @retval PlatformError::out_of_memory Allocation failed.
  /// @retval PlatformError::system_failure The system could not create the
  ///     mutex.
  [[nodiscard]] auto create_mutex(MutexKind kind)
      -> Expected<std::unique_ptr<native::Mutex>, PlatformError> override;
  /// @brief Creates a standard condition variable owned by this provider. See
  /// ThreadingProvider::create_condition_variable.
  ///
  /// @return The condition variable. Its waits fail with
  ///     PlatformError::wrong_provider when given a mutex from another
  ///     provider instance.
  /// @retval PlatformError::out_of_memory Allocation failed.
  /// @retval PlatformError::system_failure The system could not create it.
  [[nodiscard]] auto create_condition_variable()
      -> Expected<std::unique_ptr<native::ConditionVariable>,
                  PlatformError> override;
  /// @brief Creates a stop source. See ThreadingProvider::create_stop_source.
  ///
  /// @return The source. A stop requested through it ends this provider's
  ///     stop-aware waits without a notification.
  /// @retval PlatformError::out_of_memory Allocation failed.
  [[nodiscard]] auto create_stop_source()
      -> Expected<std::unique_ptr<native::StopSource>, PlatformError> override;

 private:
  std::shared_ptr<Ledger> ledger_;
};

}  // namespace libchiaking::platform
