#include "current_thread.hpp"
#include "src/core/utils/timing/saturating.hpp"
#include "standard.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::platform {

class StopSource::Flag final : public StopState {
 public:
  bool stop_requested() const noexcept override {
    return stopped_.load(std::memory_order_acquire);
  }

  bool listen(StopListener& listener) const noexcept override {
    const std::lock_guard lock(listeners_mutex_);
    try {
      listeners_.push_back(&listener);
      return true;
    } catch (const std::bad_alloc&) {
      return false;
    }
  }

  void ignore(StopListener& listener) const noexcept override {
    const std::lock_guard lock(listeners_mutex_);
    std::erase(listeners_, &listener);
  }

  auto request() noexcept -> bool {
    const std::lock_guard lock(listeners_mutex_);
    if (stopped_.exchange(true, std::memory_order_acq_rel)) return false;
    for (auto* listener : listeners_) listener->on_stop_requested();
    return true;
  }

 private:
  std::atomic<bool> stopped_{false};
  mutable std::mutex listeners_mutex_;
  mutable std::vector<StopListener*> listeners_;
};

StopSource::StopSource() : flag_(std::make_shared<Flag>()) {}

auto StopSource::token() const noexcept -> StopToken {
  return StopToken{flag_};
}

auto StopSource::request_stop() noexcept -> bool { return flag_->request(); }

auto StopSource::stop_requested() const noexcept -> bool {
  return flag_->stop_requested();
}

auto StandardThreading::deadline_after(
    std::chrono::nanoseconds timeout) noexcept -> Clock::time_point {
  return timing::saturating_add(
      Clock::now(), std::chrono::duration_cast<Clock::duration>(timeout));
}

class StandardThreading::ProvidedStopSource final : public native::StopSource {
 public:
  explicit ProvidedStopSource(
      std::shared_ptr<DefaultThreadingProvider::Ledger> ledger) noexcept
      : entry_(std::move(ledger)) {}

  StopToken token() const noexcept override { return source_.token(); }

  [[nodiscard]] bool request_stop() noexcept override {
    return source_.request_stop();
  }

 private:
  Entry entry_;
  platform::StopSource source_;
};

class StandardThreading::StandardMutex final : public native::Mutex {
 public:
  StandardMutex(const ThreadingProvider& owner, MutexKind kind,
                std::shared_ptr<DefaultThreadingProvider::Ledger> ledger)
      : native::Mutex(owner),
        entry_(std::move(ledger)),
        native_(native_of(kind)) {}

  [[nodiscard]] auto lock() -> Expected<void, PlatformError> override {
    try {
      acquire();
      return {};
    } catch (const std::system_error&) {
      return unexpected(PlatformError::system_failure);
    }
  }

  [[nodiscard]] auto try_lock() -> Expected<bool, PlatformError> override {
    try {
      return std::visit([](auto& native) { return native.try_lock(); },
                        native_);
    } catch (const std::system_error&) {
      return unexpected(PlatformError::system_failure);
    }
  }

  [[nodiscard]] auto unlock() -> Expected<void, PlatformError> override {
    release();
    return {};
  }

  void acquire() {
    std::visit([](auto& native) { native.lock(); }, native_);
  }

  void release() noexcept {
    std::visit([](auto& native) { native.unlock(); }, native_);
  }

 private:
  using Native = std::variant<std::mutex, std::recursive_mutex>;

  static auto native_of(MutexKind kind) -> Native {
    if (kind == MutexKind::recursive)
      return Native{std::in_place_type<std::recursive_mutex>};
    return Native{std::in_place_type<std::mutex>};
  }

  Entry entry_;
  Native native_;
};

class StandardThreading::StandardConditionVariable final
    : public native::ConditionVariable {
 public:
  StandardConditionVariable(
      const ThreadingProvider& owner,
      std::shared_ptr<DefaultThreadingProvider::Ledger> ledger) noexcept
      : native::ConditionVariable(owner), entry_(std::move(ledger)) {}

  void notify_one() noexcept override {
    const std::lock_guard lock(parking_);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    parked_.notify_one();
  }

  void notify_all() noexcept override {
    const std::lock_guard lock(parking_);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    parked_.notify_all();
  }

  [[nodiscard]] auto wait(native::Mutex& mutex)
      -> Expected<void, PlatformError> override {
    if (&mutex.owner() != owner_)
      return unexpected(PlatformError::wrong_provider);
    try {
      const Parking parking{*this, mutex};
      parked_.wait(parking.lock());
      return {};
    } catch (const std::system_error&) {
      return unexpected(PlatformError::system_failure);
    }
  }

  [[nodiscard]] auto wait(native::Mutex& mutex, const StopToken& stop_token)
      -> Expected<ConditionWaitStatus, PlatformError> override {
    return wait_for(mutex, std::chrono::nanoseconds::max(), stop_token);
  }

  [[nodiscard]] auto wait_for(native::Mutex& mutex,
                              std::chrono::nanoseconds timeout)
      -> Expected<ConditionWaitStatus, PlatformError> override {
    return wait_for(mutex, timeout, StopToken{});
  }

  [[nodiscard]] auto wait_for(native::Mutex& mutex,
                              std::chrono::nanoseconds timeout,
                              const StopToken& stop_token)
      -> Expected<ConditionWaitStatus, PlatformError> override {
    if (&mutex.owner() != owner_)
      return unexpected(PlatformError::wrong_provider);
    if (stop_token.stop_requested()) return ConditionWaitStatus::stop_requested;
    const auto deadline = deadline_after(timeout);
    const auto observed = generation_.load(std::memory_order_acquire);
    const Listening listening{*this, stop_token};
    if (!stop_token || listening.told())
      return park_until(mutex, deadline, observed, stop_token);
    while (!stop_token.stop_requested()) {
      const auto now = Clock::now();
      if (now >= deadline) return ConditionWaitStatus::timed_out;
      const auto waited = park_until(
          mutex,
          now + std::min<Clock::duration>(polled_quantum, deadline - now),
          observed, stop_token);
      if (!waited || waited.value() != ConditionWaitStatus::timed_out)
        return waited;
    }
    return ConditionWaitStatus::stop_requested;
  }

 private:
  class Parking {
   public:
    Parking(StandardConditionVariable& owner, native::Mutex& mutex)
        : mutex_(static_cast<StandardMutex&>(mutex)), parking_(owner.parking_) {
      mutex_.release();
    }

    ~Parking() {
      parking_.unlock();
      mutex_.acquire();
    }

    Parking(const Parking&) = delete;
    auto operator=(const Parking&) -> Parking& = delete;

    auto lock() const noexcept -> std::unique_lock<std::mutex>& {
      return parking_;
    }

   private:
    StandardMutex& mutex_;
    mutable std::unique_lock<std::mutex> parking_;
  };

  class Listening final : public StopListener {
   public:
    Listening(StandardConditionVariable& owner,
              const StopToken& stop_token) noexcept
        : owner_(owner),
          stop_token_(stop_token),
          told_(stop_token.listen(*this)) {}

    ~Listening() override {
      if (told_) stop_token_.ignore(*this);
    }

    Listening(const Listening&) = delete;
    auto operator=(const Listening&) -> Listening& = delete;

    void on_stop_requested() noexcept override {
      const std::lock_guard lock(owner_.parking_);
      owner_.parked_.notify_all();
    }

    auto told() const noexcept -> bool { return told_; }

   private:
    StandardConditionVariable& owner_;
    const StopToken& stop_token_;
    bool told_;
  };

  [[nodiscard]] auto park_until(native::Mutex& mutex,
                                Clock::time_point deadline,
                                std::uint64_t observed,
                                const StopToken& stop_token)
      -> Expected<ConditionWaitStatus, PlatformError> {
    try {
      const Parking parking{*this, mutex};
      for (;;) {
        if (stop_token.stop_requested())
          return ConditionWaitStatus::stop_requested;
        if (generation_.load(std::memory_order_acquire) != observed)
          return ConditionWaitStatus::signaled;
        const auto now = Clock::now();
        if (now >= deadline) return ConditionWaitStatus::timed_out;
        static_cast<void>(parked_.wait_until(
            parking.lock(),
            now + std::min<Clock::duration>(longest_park, deadline - now)));
      }
    } catch (const std::system_error&) {
      return unexpected(PlatformError::system_failure);
    }
  }

  static constexpr std::chrono::nanoseconds polled_quantum{
      std::chrono::milliseconds{1}};
  static constexpr std::chrono::hours longest_park{1};

  Entry entry_;
  std::mutex parking_;
  std::condition_variable parked_;
  std::atomic<std::uint64_t> generation_{0};
};

class StandardThreading::StandardThread final : public native::Thread {
 public:
  static auto start(native::ThreadEntry entry,
                    std::shared_ptr<DefaultThreadingProvider::Ledger> ledger)
      -> Expected<std::unique_ptr<native::Thread>, PlatformError> {
    try {
      return std::unique_ptr<native::Thread>{
          new StandardThread(entry, std::move(ledger))};
    } catch (const std::system_error& failure) {
      if (failure.code() == std::errc::resource_unavailable_try_again)
        return unexpected(PlatformError::out_of_memory);
      return unexpected(PlatformError::thread_failure);
    } catch (const std::bad_alloc&) {
      return unexpected(PlatformError::out_of_memory);
    } catch (...) {
      return unexpected(PlatformError::thread_failure);
    }
  }

  ~StandardThread() override {
    if (!claim()) return;
    if (on_own_thread())
      thread_.detach();
    else
      thread_.join();
    state_.store(JoinState::joined, std::memory_order_release);
  }

  StandardThread(const StandardThread&) = delete;
  auto operator=(const StandardThread&) -> StandardThread& = delete;

  bool joinable() const noexcept override {
    return state_.load(std::memory_order_acquire) != JoinState::joined;
  }

  [[nodiscard]] auto join() -> Expected<void, PlatformError> override {
    if (on_own_thread() || !claim())
      return unexpected(PlatformError::thread_failure);
    return finish_join().transform([](bool) {});
  }

  [[nodiscard]] auto join_for(std::chrono::nanoseconds timeout)
      -> Expected<bool, PlatformError> override {
    if (on_own_thread() || !claim())
      return unexpected(PlatformError::thread_failure);
    if (finished_.wait_until(deadline_after(timeout)) !=
        std::future_status::ready) {
      state_.store(JoinState::joinable, std::memory_order_release);
      return false;
    }
    return finish_join();
  }

  [[nodiscard]] auto result() const -> Expected<void*, PlatformError> override {
    if (finished_.wait_for(std::chrono::seconds::zero()) !=
        std::future_status::ready)
      return unexpected(PlatformError::thread_failure);
    return finished_.get();
  }

 private:
  enum class JoinState : std::uint8_t {
    joinable,
    joining,
    joined,
  };

  StandardThread(native::ThreadEntry entry,
                 std::shared_ptr<DefaultThreadingProvider::Ledger> ledger)
      : entry_(std::move(ledger)) {
    std::promise<void*> finished;
    finished_ = finished.get_future().share();
    thread_ = std::thread{&StandardThread::run, entry, std::string{entry.name},
                          std::move(finished)};
    state_.store(JoinState::joinable, std::memory_order_release);
  }

  static void run(native::ThreadEntry entry, std::string name,
                  std::promise<void*> finished) noexcept {
    if (!name.empty()) threading::CurrentThread::rename(name);
    if (entry.affinity_callback != nullptr)
      entry.affinity_callback(entry.role, entry.affinity_user);
    void* result = nullptr;
    if (entry.result_function != nullptr)
      result = entry.result_function(entry.context);
    else
      entry.function(entry.context);
    finished.set_value(result);
  }

  [[nodiscard]] bool claim() noexcept {
    auto expected = JoinState::joinable;
    return state_.compare_exchange_strong(expected, JoinState::joining,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire);
  }

  [[nodiscard]] auto on_own_thread() const noexcept -> bool {
    return thread_.get_id() == std::this_thread::get_id();
  }

  [[nodiscard]] auto finish_join() -> Expected<bool, PlatformError> {
    try {
      thread_.join();
    } catch (const std::system_error&) {
      state_.store(JoinState::joinable, std::memory_order_release);
      return unexpected(PlatformError::thread_failure);
    }
    state_.store(JoinState::joined, std::memory_order_release);
    return true;
  }

  Entry entry_;
  std::shared_future<void*> finished_;
  std::thread thread_;
  std::atomic<JoinState> state_{JoinState::joined};
};

template <class Made, class... Arguments>
auto StandardThreading::make(Arguments&&... arguments)
    -> Expected<std::unique_ptr<Made>, PlatformError> {
  try {
    return std::make_unique<Made>(std::forward<Arguments>(arguments)...);
  } catch (const std::bad_alloc&) {
    return unexpected(PlatformError::out_of_memory);
  } catch (...) {
    return unexpected(PlatformError::system_failure);
  }
}

auto StandardThreading::start_thread(
    native::ThreadEntry entry,
    std::shared_ptr<DefaultThreadingProvider::Ledger> ledger)
    -> Expected<std::unique_ptr<native::Thread>, PlatformError> {
  if (entry.function == nullptr && entry.result_function == nullptr)
    return unexpected(PlatformError::thread_failure);
  return StandardThread::start(entry, std::move(ledger));
}

auto StandardThreading::create_mutex(
    const ThreadingProvider& owner, MutexKind kind,
    std::shared_ptr<DefaultThreadingProvider::Ledger> ledger)
    -> Expected<std::unique_ptr<native::Mutex>, PlatformError> {
  return make<StandardMutex>(owner, kind, std::move(ledger));
}

auto StandardThreading::create_condition_variable(
    const ThreadingProvider& owner,
    std::shared_ptr<DefaultThreadingProvider::Ledger> ledger)
    -> Expected<std::unique_ptr<native::ConditionVariable>, PlatformError> {
  return make<StandardConditionVariable>(owner, std::move(ledger));
}

auto StandardThreading::create_stop_source(
    std::shared_ptr<DefaultThreadingProvider::Ledger> ledger)
    -> Expected<std::unique_ptr<native::StopSource>, PlatformError> {
  return make<ProvidedStopSource>(std::move(ledger));
}

DefaultThreadingProvider::DefaultThreadingProvider()
    : ledger_(std::make_shared<Ledger>()) {}

DefaultThreadingProvider::~DefaultThreadingProvider() { ledger_->close(); }

auto DefaultThreadingProvider::start_thread(native::ThreadEntry entry)
    -> Expected<std::unique_ptr<native::Thread>, PlatformError> {
  return StandardThreading::start_thread(entry, ledger_);
}

auto DefaultThreadingProvider::create_mutex(MutexKind kind)
    -> Expected<std::unique_ptr<native::Mutex>, PlatformError> {
  return StandardThreading::create_mutex(*this, kind, ledger_);
}

auto DefaultThreadingProvider::create_condition_variable()
    -> Expected<std::unique_ptr<native::ConditionVariable>, PlatformError> {
  return StandardThreading::create_condition_variable(*this, ledger_);
}

auto DefaultThreadingProvider::create_stop_source()
    -> Expected<std::unique_ptr<native::StopSource>, PlatformError> {
  return StandardThreading::create_stop_source(ledger_);
}

}  // namespace libchiaking::platform
