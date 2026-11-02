#pragma once

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace play_test {

class ProviderWatch final : public libchiaking::platform::ThreadingProvider {
 public:
  enum class Reach : std::uint8_t { lock, wait, clock };

  class Clock final : public libchiaking::platform::ClockProvider {
   public:
    Clock(ProviderWatch& watch,
          const libchiaking::platform::ClockProvider& inner) noexcept
        : watch_(watch), inner_(inner) {}

    std::uint64_t now_microseconds() const noexcept override {
      watch_.note(Reach::clock);
      return inner_.now_microseconds() +
             skipped_.load(std::memory_order_acquire);
    }

    void skip(std::chrono::seconds ahead) noexcept {
      skipped_.fetch_add(static_cast<std::uint64_t>(ahead.count()) * 1'000'000U,
                         std::memory_order_acq_rel);
    }

   private:
    ProviderWatch& watch_;
    const libchiaking::platform::ClockProvider& inner_;
    std::atomic<std::uint64_t> skipped_{};
  };

  ProviderWatch() { adopt(); }

  void adopt() noexcept { marked_ = this; }

  auto threads() const noexcept -> std::size_t {
    return threads_.load(std::memory_order_acquire);
  }

  auto reached(Reach kind) const noexcept -> std::size_t {
    return reached_[index(kind)].load(std::memory_order_acquire);
  }

  auto foreign(Reach kind) const noexcept -> std::size_t {
    return foreign_[index(kind)].load(std::memory_order_acquire);
  }

  auto foreign() const noexcept -> std::size_t {
    return foreign(Reach::lock) + foreign(Reach::wait) + foreign(Reach::clock);
  }

  auto steady() noexcept -> Clock& { return steady_; }
  auto wall() noexcept -> Clock& { return wall_; }

  [[nodiscard]] auto start_thread(
      libchiaking::platform::native::ThreadEntry entry)
      -> libchiaking::Expected<
          std::unique_ptr<libchiaking::platform::native::Thread>,
          libchiaking::platform::PlatformError> override {
    auto launch = std::make_unique<Launch>(*this, entry);
    libchiaking::platform::native::ThreadEntry wrapped{
        entry.function != nullptr ? &Launch::run : nullptr, launch.get()};
    if (entry.result_function != nullptr)
      wrapped.result_function = &Launch::produce;
    wrapped.name = entry.name;
    wrapped.role = entry.role;
    wrapped.affinity_callback = entry.affinity_callback;
    wrapped.affinity_user = entry.affinity_user;
    auto started = inner_.start_thread(wrapped);
    if (!started) return libchiaking::unexpected(started.error());
    threads_.fetch_add(1, std::memory_order_acq_rel);
    return std::make_unique<Started>(std::move(launch),
                                     std::move(started.value()));
  }

  [[nodiscard]] auto create_mutex(libchiaking::platform::MutexKind kind)
      -> libchiaking::Expected<
          std::unique_ptr<libchiaking::platform::native::Mutex>,
          libchiaking::platform::PlatformError> override {
    auto made = inner_.create_mutex(kind);
    if (!made) return libchiaking::unexpected(made.error());
    return std::make_unique<WatchedMutex>(*this, std::move(made.value()));
  }

  [[nodiscard]] auto create_condition_variable() -> libchiaking::Expected<
      std::unique_ptr<libchiaking::platform::native::ConditionVariable>,
      libchiaking::platform::PlatformError> override {
    auto made = inner_.create_condition_variable();
    if (!made) return libchiaking::unexpected(made.error());
    return std::make_unique<WatchedCondition>(*this, std::move(made.value()));
  }

  [[nodiscard]] auto create_stop_source() -> libchiaking::Expected<
      std::unique_ptr<libchiaking::platform::native::StopSource>,
      libchiaking::platform::PlatformError> override {
    return inner_.create_stop_source();
  }

 private:
  static constexpr std::size_t reach_kinds = 3;

  class Launch {
   public:
    Launch(ProviderWatch& watch,
           libchiaking::platform::native::ThreadEntry entry) noexcept
        : watch_(watch), entry_(entry) {}

    static void run(void* self) noexcept {
      auto& launch = *static_cast<Launch*>(self);
      launch.watch_.adopt();
      launch.entry_.function(launch.entry_.context);
    }

    static auto produce(void* self) noexcept -> void* {
      auto& launch = *static_cast<Launch*>(self);
      launch.watch_.adopt();
      return launch.entry_.result_function(launch.entry_.context);
    }

   private:
    ProviderWatch& watch_;
    libchiaking::platform::native::ThreadEntry entry_;
  };

  class Started final : public libchiaking::platform::native::Thread {
   public:
    Started(
        std::unique_ptr<Launch> launch,
        std::unique_ptr<libchiaking::platform::native::Thread> inner) noexcept
        : launch_(std::move(launch)), inner_(std::move(inner)) {}

    bool joinable() const noexcept override { return inner_->joinable(); }

    [[nodiscard]] auto join() -> libchiaking::Expected<
        void, libchiaking::platform::PlatformError> override {
      return inner_->join();
    }

    [[nodiscard]] auto join_for(std::chrono::nanoseconds timeout)
        -> libchiaking::Expected<
            bool, libchiaking::platform::PlatformError> override {
      return inner_->join_for(timeout);
    }

    [[nodiscard]] auto result() const -> libchiaking::Expected<
        void*, libchiaking::platform::PlatformError> override {
      return inner_->result();
    }

   private:
    std::unique_ptr<Launch> launch_;
    std::unique_ptr<libchiaking::platform::native::Thread> inner_;
  };

  class WatchedMutex final : public libchiaking::platform::native::Mutex {
   public:
    WatchedMutex(ProviderWatch& watch,
                 std::unique_ptr<libchiaking::platform::native::Mutex> inner)
        : Mutex(watch), watch_(watch), inner_(std::move(inner)) {}

    auto inner() noexcept -> libchiaking::platform::native::Mutex& {
      return *inner_;
    }

    [[nodiscard]] auto lock() -> libchiaking::Expected<
        void, libchiaking::platform::PlatformError> override {
      watch_.note(Reach::lock);
      return inner_->lock();
    }

    [[nodiscard]] auto try_lock() -> libchiaking::Expected<
        bool, libchiaking::platform::PlatformError> override {
      watch_.note(Reach::lock);
      return inner_->try_lock();
    }

    [[nodiscard]] auto unlock() -> libchiaking::Expected<
        void, libchiaking::platform::PlatformError> override {
      return inner_->unlock();
    }

   private:
    ProviderWatch& watch_;
    std::unique_ptr<libchiaking::platform::native::Mutex> inner_;
  };

  class WatchedCondition final
      : public libchiaking::platform::native::ConditionVariable {
   public:
    WatchedCondition(
        ProviderWatch& watch,
        std::unique_ptr<libchiaking::platform::native::ConditionVariable> inner)
        : ConditionVariable(watch),
          watch_(watch),
          inner_(std::move(inner)) {}

    void notify_one() noexcept override { inner_->notify_one(); }
    void notify_all() noexcept override { inner_->notify_all(); }

    [[nodiscard]] auto wait(libchiaking::platform::native::Mutex& mutex)
        -> libchiaking::Expected<
            void, libchiaking::platform::PlatformError> override {
      watch_.note(Reach::wait);
      return inner_->wait(unwrap(mutex));
    }

    [[nodiscard]] auto wait(libchiaking::platform::native::Mutex& mutex,
                            const libchiaking::platform::StopToken& stop)
        -> libchiaking::Expected<
            libchiaking::platform::ConditionWaitStatus,
            libchiaking::platform::PlatformError> override {
      watch_.note(Reach::wait);
      return inner_->wait(unwrap(mutex), stop);
    }

    [[nodiscard]] auto wait_for(libchiaking::platform::native::Mutex& mutex,
                                std::chrono::nanoseconds timeout)
        -> libchiaking::Expected<
            libchiaking::platform::ConditionWaitStatus,
            libchiaking::platform::PlatformError> override {
      watch_.note(Reach::wait);
      return inner_->wait_for(unwrap(mutex), timeout);
    }

    [[nodiscard]] auto wait_for(libchiaking::platform::native::Mutex& mutex,
                                std::chrono::nanoseconds timeout,
                                const libchiaking::platform::StopToken& stop)
        -> libchiaking::Expected<
            libchiaking::platform::ConditionWaitStatus,
            libchiaking::platform::PlatformError> override {
      watch_.note(Reach::wait);
      return inner_->wait_for(unwrap(mutex), timeout, stop);
    }

   private:
    static auto unwrap(libchiaking::platform::native::Mutex& mutex)
        -> libchiaking::platform::native::Mutex& {
      return static_cast<WatchedMutex&>(mutex).inner();
    }

    ProviderWatch& watch_;
    std::unique_ptr<libchiaking::platform::native::ConditionVariable> inner_;
  };

  static constexpr auto index(Reach kind) noexcept -> std::size_t {
    return static_cast<std::size_t>(kind);
  }

  void note(Reach kind) noexcept {
    reached_[index(kind)].fetch_add(1, std::memory_order_acq_rel);
    if (marked_ != this)
      foreign_[index(kind)].fetch_add(1, std::memory_order_acq_rel);
  }

  static inline thread_local const ProviderWatch* marked_ = nullptr;

  libchiaking::platform::DefaultThreadingProvider inner_;
  libchiaking::platform::SteadyClock steady_source_;
  libchiaking::platform::SystemClock wall_source_;
  Clock steady_{*this, steady_source_};
  Clock wall_{*this, wall_source_};
  std::atomic<std::size_t> threads_{};
  std::array<std::atomic<std::size_t>, reach_kinds> reached_{};
  std::array<std::atomic<std::size_t>, reach_kinds> foreign_{};
};

}  // namespace play_test
