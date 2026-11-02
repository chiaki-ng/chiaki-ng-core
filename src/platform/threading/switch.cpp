#include "current_thread.hpp"
#include "libchiaking/platform/runtime/horizon_threading.hpp"
#include "standard.hpp"

#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::platform::threading {

void CurrentThread::rename(std::string_view) noexcept {}

}  // namespace libchiaking::platform::threading

namespace libchiaking::platform::horizon {

auto ThreadPlacement::make(std::uint8_t core, std::uint8_t priority,
                           std::size_t stack_size) noexcept
    -> std::optional<ThreadPlacement> {
  if (core > last_core || priority > lowest_priority ||
      stack_size < minimum_stack_size || stack_size % stack_alignment != 0)
    return std::nullopt;
  return ThreadPlacement{core, priority, stack_size};
}

ThreadPlacementPolicy::ThreadPlacementPolicy() noexcept
    : placements_([] {
        constexpr std::uint8_t priority = 0x2c;
        constexpr std::size_t stack_size = 0x40000;
        constexpr ThreadPlacement network{0, priority, stack_size};
        constexpr ThreadPlacement feedback{1, priority, stack_size};
        constexpr ThreadPlacement session{2, priority, stack_size};
        return std::array<ThreadPlacement, role_count>{
            session, network, network,  network, network, network, network,
            network, network, feedback, session, session, session,
        };
      }()) {}

auto ThreadPlacementPolicy::place(ThreadRole role,
                                  ThreadPlacement placement) noexcept
    -> ThreadPlacementPolicy& {
  placements_[static_cast<std::size_t>(role)] = placement;
  return *this;
}

auto ThreadPlacementPolicy::placement(ThreadRole role) const noexcept
    -> ThreadPlacement {
  return placements_[static_cast<std::size_t>(role)];
}

class ThreadingProvider::Shared {
 public:
  class Run {
   public:
    explicit Run(native::ThreadEntry entry) noexcept : entry_(entry) {}
    Run(const Run&) = delete;
    auto operator=(const Run&) -> Run& = delete;
    ~Run() {
      if (created_) (void)::threadClose(&thread_);
    }

    [[nodiscard]] auto start(const ThreadPlacement& placement,
                             std::uint8_t core)
        -> Expected<void, PlatformError> {
      const auto created =
          ::threadCreate(&thread_, &Run::body, this, nullptr,
                         placement.stack_size(), placement.priority(), core);
      if (R_FAILED(created)) {
        if (R_VALUE(created) ==
            MAKERESULT(Module_Libnx, LibnxError_OutOfMemory))
          return unexpected(PlatformError::out_of_memory);
        return unexpected(PlatformError::thread_failure);
      }
      created_ = true;
      if (R_FAILED(::threadStart(&thread_)))
        return unexpected(PlatformError::thread_failure);
      return {};
    }

    [[nodiscard]] auto wait_for(std::chrono::nanoseconds timeout) const noexcept
        -> Expected<bool, PlatformError> {
      const auto bounded = std::max(timeout, std::chrono::nanoseconds::zero());
      const auto waited = ::waitSingleHandle(
          thread_.handle, static_cast<std::uint64_t>(bounded.count()));
      if (R_SUCCEEDED(waited)) return true;
      if (R_VALUE(waited) == KERNELRESULT(TimedOut)) return false;
      return unexpected(PlatformError::system_failure);
    }

    [[nodiscard]] auto wait() noexcept -> Expected<void, PlatformError> {
      if (R_FAILED(::threadWaitForExit(&thread_)))
        return unexpected(PlatformError::thread_failure);
      return {};
    }

    [[nodiscard]] auto finished() const noexcept -> bool {
      return finished_.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto result() const noexcept -> void* { return result_; }

    [[nodiscard]] auto on_own_thread() const noexcept -> bool {
      return ::threadGetSelf() == &thread_;
    }

   private:
    static void body(void* argument) {
      auto& run = *static_cast<Run*>(argument);
      const auto entry = run.entry_;
      if (!entry.name.empty()) threading::CurrentThread::rename(entry.name);
      if (entry.affinity_callback != nullptr)
        entry.affinity_callback(entry.role, entry.affinity_user);
      void* result = nullptr;
      if (entry.result_function != nullptr)
        result = entry.result_function(entry.context);
      else
        entry.function(entry.context);
      run.result_ = result;
      run.finished_.store(true, std::memory_order_release);
    }

    native::ThreadEntry entry_;
    ::Thread thread_{};
    bool created_{false};
    void* result_{nullptr};
    std::atomic<bool> finished_{false};
  };

  Shared() : ledger_(std::make_shared<DefaultThreadingProvider::Ledger>()) {
    std::uint64_t mask = 0;
    if (R_FAILED(::svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
      mask = 0b0111;
    core_mask_ = mask;
  }

  Shared(const Shared&) = delete;
  auto operator=(const Shared&) -> Shared& = delete;

  ~Shared() {
    const std::lock_guard held{orphans_mutex_};
    for (auto& orphan : orphans_) (void)orphan->wait();
    orphans_.clear();
  }

  [[nodiscard]] auto ledger() const noexcept
      -> const std::shared_ptr<DefaultThreadingProvider::Ledger>& {
    return ledger_;
  }

  [[nodiscard]] auto core_for(const ThreadPlacement& placement) const noexcept
      -> std::optional<std::uint8_t> {
    if (usable(placement.core())) return placement.core();
    for (std::uint8_t core = 0; core < ThreadPlacement::last_core; ++core) {
      if (usable(core)) return core;
    }
    return std::nullopt;
  }

  [[nodiscard]] static auto threads_remaining() noexcept
      -> std::optional<std::int64_t> {
    std::uint64_t limit_handle = INVALID_HANDLE;
    if (R_FAILED(::svcGetInfo(&limit_handle, InfoType_ResourceLimit,
                              INVALID_HANDLE, 0)))
      return std::nullopt;
    std::int64_t current = 0;
    std::int64_t limit = 0;
    const auto handle = static_cast<Handle>(limit_handle);
    if (R_FAILED(::svcGetResourceLimitCurrentValue(
            &current, handle, LimitableResource_Threads)) ||
        R_FAILED(::svcGetResourceLimitLimitValue(&limit, handle,
                                                 LimitableResource_Threads)))
      return std::nullopt;
    return limit - current;
  }

  void adopt(std::unique_ptr<Run> run) noexcept {
    try {
      const std::lock_guard held{orphans_mutex_};
      orphans_.push_back(std::move(run));
    } catch (...) {
      (void)run.release();
    }
  }

  void reap() noexcept {
    const std::lock_guard held{orphans_mutex_};
    std::erase_if(orphans_, [](const std::unique_ptr<Run>& orphan) {
      const auto exited = orphan->wait_for(std::chrono::nanoseconds::zero());
      return exited && exited.value();
    });
  }

 private:
  [[nodiscard]] auto usable(std::uint8_t core) const noexcept -> bool {
    return ((core_mask_ >> core) & 1u) != 0;
  }

  std::shared_ptr<DefaultThreadingProvider::Ledger> ledger_;
  std::uint64_t core_mask_{0};
  std::mutex orphans_mutex_;
  std::vector<std::unique_ptr<Run>> orphans_;
};

class HorizonThread final : public native::Thread {
  using Run = ThreadingProvider::Shared::Run;

 public:
  HorizonThread(std::unique_ptr<Run> run,
                std::shared_ptr<ThreadingProvider::Shared> shared)
      : entry_(shared->ledger()),
        run_(std::move(run)),
        shared_(std::move(shared)) {
    state_.store(JoinState::joinable, std::memory_order_release);
  }

  ~HorizonThread() override {
    if (!claim()) return;
    if (run_->on_own_thread()) {
      shared_->adopt(std::move(run_));
      return;
    }
    (void)run_->wait();
    state_.store(JoinState::joined, std::memory_order_release);
  }

  HorizonThread(const HorizonThread&) = delete;
  auto operator=(const HorizonThread&) -> HorizonThread& = delete;

  bool joinable() const noexcept override {
    return state_.load(std::memory_order_acquire) != JoinState::joined;
  }

  [[nodiscard]] auto join() -> Expected<void, PlatformError> override {
    if (run_->on_own_thread() || !claim())
      return unexpected(PlatformError::thread_failure);
    return finish_join().transform([](bool) {});
  }

  [[nodiscard]] auto join_for(std::chrono::nanoseconds timeout)
      -> Expected<bool, PlatformError> override {
    if (run_->on_own_thread() || !claim())
      return unexpected(PlatformError::thread_failure);
    const auto exited = run_->wait_for(timeout);
    if (!exited || !exited.value()) {
      state_.store(JoinState::joinable, std::memory_order_release);
      return exited;
    }
    return finish_join();
  }

  [[nodiscard]] auto result() const -> Expected<void*, PlatformError> override {
    if (!run_->finished()) return unexpected(PlatformError::thread_failure);
    return run_->result();
  }

 private:
  enum class JoinState : std::uint8_t {
    joinable,
    joining,
    joined,
  };

  [[nodiscard]] bool claim() noexcept {
    auto expected = JoinState::joinable;
    return state_.compare_exchange_strong(expected, JoinState::joining,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire);
  }

  [[nodiscard]] auto finish_join() -> Expected<bool, PlatformError> {
    if (const auto waited = run_->wait(); !waited) {
      state_.store(JoinState::joinable, std::memory_order_release);
      return unexpected(waited.error());
    }
    state_.store(JoinState::joined, std::memory_order_release);
    return true;
  }

  DefaultThreadingProvider::Ledger::Entry entry_;
  std::unique_ptr<Run> run_;
  std::shared_ptr<ThreadingProvider::Shared> shared_;
  std::atomic<JoinState> state_{JoinState::joined};
};

ThreadingProvider::ThreadingProvider(ThreadPlacementPolicy policy)
    : policy_(policy), shared_(std::make_shared<Shared>()) {}

ThreadingProvider::~ThreadingProvider() {
  const auto ledger = shared_->ledger();
  shared_.reset();
  ledger->close();
}

auto ThreadingProvider::start_thread(native::ThreadEntry entry)
    -> Expected<std::unique_ptr<native::Thread>, PlatformError> {
  if (entry.function == nullptr && entry.result_function == nullptr)
    return unexpected(PlatformError::thread_failure);
  shared_->reap();
  const auto placement = policy_.placement(entry.role);
  const auto core = shared_->core_for(placement);
  if (!core) return unexpected(PlatformError::thread_failure);
  if (const auto remaining = Shared::threads_remaining();
      remaining && *remaining <= 1)
    return unexpected(PlatformError::out_of_memory);
  try {
    auto run = std::make_unique<Shared::Run>(entry);
    if (auto started = run->start(placement, *core); !started)
      return unexpected(started.error());
    return std::unique_ptr<native::Thread>{
        std::make_unique<HorizonThread>(std::move(run), shared_)};
  } catch (const std::bad_alloc&) {
    return unexpected(PlatformError::out_of_memory);
  }
}

auto ThreadingProvider::create_mutex(MutexKind kind)
    -> Expected<std::unique_ptr<native::Mutex>, PlatformError> {
  return StandardThreading::create_mutex(*this, kind, shared_->ledger());
}

auto ThreadingProvider::create_condition_variable()
    -> Expected<std::unique_ptr<native::ConditionVariable>, PlatformError> {
  return StandardThreading::create_condition_variable(*this, shared_->ledger());
}

auto ThreadingProvider::create_stop_source()
    -> Expected<std::unique_ptr<native::StopSource>, PlatformError> {
  return StandardThreading::create_stop_source(shared_->ledger());
}

auto ThreadingProvider::policy() const noexcept
    -> const ThreadPlacementPolicy& {
  return policy_;
}

auto ThreadingProvider::core_for(ThreadRole role) const noexcept
    -> std::optional<std::uint8_t> {
  return shared_->core_for(policy_.placement(role));
}

}  // namespace libchiaking::platform::horizon
