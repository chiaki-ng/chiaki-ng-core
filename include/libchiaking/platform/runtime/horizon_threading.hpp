/// @file
/// @brief Declares the Horizon threading provider, which starts every thread
/// on a chosen core with a chosen priority and stack size.
#pragma once

#include <libchiaking/platform/runtime/threading.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace libchiaking::platform::horizon {

/// @brief Where and how one thread runs: its core, its priority and the size
/// of its stack.
///
/// Always valid: make() refuses values the kernel would reject.
class ThreadPlacement {
 public:
  /// @brief The highest core number a placement may name.
  static constexpr std::uint8_t last_core = 3;
  /// @brief The lowest scheduling priority a placement may name; larger
  /// numbers run later.
  static constexpr std::uint8_t lowest_priority = 0x3f;
  /// @brief The smallest stack a placement may ask for.
  static constexpr std::size_t minimum_stack_size = 0x4000;
  /// @brief The granularity every stack size is a multiple of.
  static constexpr std::size_t stack_alignment = 0x1000;

  /// @brief Builds a placement after checking each value.
  ///
  /// @param core The core the thread runs on, from 0 to last_core.
  /// @param priority The kernel priority, from 0 to lowest_priority.
  /// @param stack_size The stack size in bytes: at least minimum_stack_size
  ///     and a multiple of stack_alignment.
  /// @return The placement, or nothing when any value is out of range.
  static auto make(std::uint8_t core, std::uint8_t priority,
                   std::size_t stack_size) noexcept
      -> std::optional<ThreadPlacement>;

  /// @brief The core the thread runs on.
  ///
  /// @return A core number from 0 to last_core.
  auto core() const noexcept -> std::uint8_t { return core_; }
  /// @brief The thread's kernel priority.
  ///
  /// @return A priority from 0 to lowest_priority.
  auto priority() const noexcept -> std::uint8_t { return priority_; }
  /// @brief The thread's stack size.
  ///
  /// @return The size in bytes.
  auto stack_size() const noexcept -> std::size_t { return stack_size_; }

  /// @brief Compares every field.
  ///
  /// @param other The placement to compare with.
  /// @return True when core, priority and stack size all match.
  auto operator==(const ThreadPlacement& other) const noexcept
      -> bool = default;

 private:
  constexpr ThreadPlacement(std::uint8_t core, std::uint8_t priority,
                            std::size_t stack_size) noexcept
      : core_(core), priority_(priority), stack_size_(stack_size) {}

  std::uint8_t core_;
  std::uint8_t priority_;
  std::size_t stack_size_;

  friend class ThreadPlacementPolicy;
};

/// @brief The placement each ThreadRole gets.
///
/// The standard policy keeps the system's shared core free: network roles
/// (control, congestion, discovery, the discovery service, transport, its
/// sending side, reliable UDP sending and hole punching) run on core 0,
/// feedback on core 1, and session, registration, crypto and unspecified work
/// on core 2. Every role runs at priority 0x2c with a 256 KiB stack. Core 3 is
/// only used for a role the host places there itself.
class ThreadPlacementPolicy {
 public:
  /// @brief The number of roles a policy places.
  static constexpr std::size_t role_count =
      static_cast<std::size_t>(ThreadRole::crypto) + 1;

  /// @brief Builds the standard policy described above.
  ThreadPlacementPolicy() noexcept;

  /// @brief Places one role somewhere else.
  ///
  /// @param role The role to move.
  /// @param placement Where threads with that role run from now on.
  /// @return This policy, for chaining.
  auto place(ThreadRole role, ThreadPlacement placement) noexcept
      -> ThreadPlacementPolicy&;

  /// @brief The placement for a role.
  ///
  /// @param role The role of the thread about to start.
  /// @return Where that thread runs.
  auto placement(ThreadRole role) const noexcept -> ThreadPlacement;

 private:
  std::array<ThreadPlacement, role_count> placements_;
};

/// @brief A ThreadingProvider that starts each thread as a kernel thread
/// placed by its ThreadRole.
///
/// Threads are created on their placement's core with its priority and stack
/// size, so none ever runs on the creator's core first. A placement naming a
/// core the process may not use falls back to the lowest core it may use
/// other than core 3. Before each thread is created the process's thread
/// resource limit is checked, so exhausting it is reported rather than
/// failing inside the kernel. Mutexes, condition variables and stop sources
/// are the standard ones, which the C library implements on kernel
/// primitives.
///
/// A thread handle destroyed on its own thread cannot close itself. The
/// provider keeps it and closes it once the thread has finished, on a later
/// start_thread() or when the provider is destroyed.
class ThreadingProvider final : public platform::ThreadingProvider {
 public:
  /// @brief State shared by the provider and the threads it started.
  class Shared;

  /// @brief Builds a provider that places threads by the given policy.
  ///
  /// @param policy Where each role runs.
  explicit ThreadingProvider(ThreadPlacementPolicy policy = {});
  /// @brief Not copyable; what it made counts against this instance.
  ThreadingProvider(const ThreadingProvider&) = delete;
  /// @brief Not copy-assignable; what it made counts against this instance.
  auto operator=(const ThreadingProvider&) -> ThreadingProvider& = delete;
  /// @brief Waits for and closes threads whose handles were destroyed on
  /// their own thread, then destroys the provider.
  ///
  /// Anything else it made that still exists ends the process through
  /// std::terminate, as with DefaultThreadingProvider.
  ~ThreadingProvider() override;
  /// @brief Keeps the plain-mutex convenience overload visible alongside the
  /// override.
  ///
  /// @return A plain mutex, or the error create_mutex(MutexKind) reports.
  using platform::ThreadingProvider::create_mutex;

  /// @brief Starts a kernel thread placed by the entry's role. See
  /// platform::ThreadingProvider::start_thread.
  ///
  /// @param entry What to run.
  /// @return The thread handle. It joins the thread when destroyed.
  /// @retval PlatformError::thread_failure @p entry names no function, no
  ///     core is usable, or the kernel refused the thread.
  /// @retval PlatformError::out_of_memory The thread resource limit is
  ///     reached or the stack could not be allocated.
  [[nodiscard]] auto start_thread(native::ThreadEntry entry)
      -> Expected<std::unique_ptr<native::Thread>, PlatformError> override;
  /// @brief Creates a standard mutex owned by this provider. See
  /// platform::ThreadingProvider::create_mutex.
  ///
  /// @param kind Plain or recursive.
  /// @return The mutex.
  /// @retval PlatformError::out_of_memory Allocation failed.
  /// @retval PlatformError::system_failure The system could not create it.
  [[nodiscard]] auto create_mutex(MutexKind kind)
      -> Expected<std::unique_ptr<native::Mutex>, PlatformError> override;
  /// @brief Creates a standard condition variable owned by this provider. See
  /// platform::ThreadingProvider::create_condition_variable.
  ///
  /// @return The condition variable.
  /// @retval PlatformError::out_of_memory Allocation failed.
  /// @retval PlatformError::system_failure The system could not create it.
  [[nodiscard]] auto create_condition_variable()
      -> Expected<std::unique_ptr<native::ConditionVariable>,
                  PlatformError> override;
  /// @brief Creates a stop source. See
  /// platform::ThreadingProvider::create_stop_source.
  ///
  /// @return The source.
  /// @retval PlatformError::out_of_memory Allocation failed.
  [[nodiscard]] auto create_stop_source()
      -> Expected<std::unique_ptr<native::StopSource>, PlatformError> override;

  /// @brief The policy threads are placed by.
  ///
  /// @return The policy given at construction.
  auto policy() const noexcept -> const ThreadPlacementPolicy&;

  /// @brief The core a role's threads actually run on in this process.
  ///
  /// @param role The role to look up.
  /// @return The placement's core, or its fallback when the process may not
  ///     use that core; nothing when the process may use no core.
  auto core_for(ThreadRole role) const noexcept -> std::optional<std::uint8_t>;

 private:
  ThreadPlacementPolicy policy_;
  std::shared_ptr<Shared> shared_;
};

}  // namespace libchiaking::platform::horizon
