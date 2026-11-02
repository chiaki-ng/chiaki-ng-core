#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

namespace {

using namespace libchiaking::platform;
using namespace std::chrono_literals;

class ThreadingProvider : public ::testing::Test {
 protected:
  struct Context {
    std::atomic<bool> release{false};
    std::atomic<bool> affinity_called{false};
    std::atomic<ThreadRole> role{ThreadRole::unspecified};
    int result_value{42};
  };

  struct SelfJoinContext {
    std::atomic<native::Thread*> thread{nullptr};
    std::atomic<bool> rejected{false};
  };

  struct ConditionContext {
    native::Mutex* mutex{};
    native::ConditionVariable* condition{};
    std::atomic<bool> waiting{false};
    std::atomic<bool> release{false};
    std::atomic<bool> woke{false};
  };

  static void* result_entry(void* opaque) noexcept {
    auto& context = *static_cast<Context*>(opaque);
    while (!context.release.load(std::memory_order_acquire))
      std::this_thread::yield();
    return &context.result_value;
  }

  static void* null_result_entry(void* opaque) noexcept {
    auto& context = *static_cast<Context*>(opaque);
    while (!context.release.load(std::memory_order_acquire))
      std::this_thread::yield();
    return nullptr;
  }

  static void affinity(ThreadRole role, void* opaque) noexcept {
    auto& context = *static_cast<Context*>(opaque);
    context.role.store(role, std::memory_order_release);
    context.affinity_called.store(true, std::memory_order_release);
  }

  static void condition_entry(void* opaque) noexcept {
    auto& context = *static_cast<ConditionContext*>(opaque);
    if (!context.mutex->lock()) return;
    context.waiting.store(true, std::memory_order_release);
    while (!context.release.load(std::memory_order_acquire)) {
      if (!context.condition->wait(*context.mutex)) {
        static_cast<void>(context.mutex->unlock());
        return;
      }
    }
    context.woke.store(true, std::memory_order_release);
    static_cast<void>(context.mutex->unlock());
  }

  static void self_join_entry(void* opaque) noexcept {
    auto& context = *static_cast<SelfJoinContext*>(opaque);
    native::Thread* thread = nullptr;
    while ((thread = context.thread.load(std::memory_order_acquire)) == nullptr)
      std::this_thread::yield();
    const auto joined = thread->join();
    context.rejected.store(
        !joined && joined.error() == PlatformError::thread_failure,
        std::memory_order_release);
  }

  [[nodiscard]] auto now() const -> std::chrono::microseconds {
    return std::chrono::microseconds{
        static_cast<std::int64_t>(clock.now_microseconds())};
  }

  void pause(std::chrono::nanoseconds duration) {
    static_cast<void>(sleeper.sleep_for(duration));
  }

  template <class Work>
  [[nodiscard]] auto start(Work work) -> Thread {
    auto started = Thread::start(harness_threading, "harness",
                                 ThreadRole::unspecified, std::move(work));
    if (!started) return Thread{};
    return std::move(started).value();
  }

  DefaultThreadingProvider harness_threading;
  SteadyClock clock;
  Sleeper sleeper{harness_threading, clock};
  DefaultThreadingProvider provider;
};

TEST_F(ThreadingProvider, RecursiveMutexLocksTwiceAndUnlocksTwice) {
  const auto recursive = provider.create_mutex(MutexKind::recursive);
  ASSERT_TRUE(recursive);
  EXPECT_TRUE(recursive.value()->lock());
  EXPECT_TRUE(recursive.value()->lock());
  EXPECT_TRUE(recursive.value()->unlock());
  EXPECT_TRUE(recursive.value()->unlock());
}

TEST_F(ThreadingProvider, ThreadHandsBackItsResultAndRunsTheAffinityCallback) {
  Context context;
  native::ThreadEntry entry{nullptr, &context};
  entry.result_function = result_entry;
  entry.name = "chiaki-test";
  entry.role = ThreadRole::transport;
  entry.affinity_callback = affinity;
  entry.affinity_user = &context;
  auto thread = provider.start_thread(entry);
  ASSERT_TRUE(thread);
  const auto timed_out = thread.value()->join_for(1ms);
  ASSERT_TRUE(timed_out);
  EXPECT_FALSE(timed_out.value());
  EXPECT_TRUE(thread.value()->joinable());
  context.release.store(true, std::memory_order_release);
  const auto joined = thread.value()->join_for(2s);
  ASSERT_TRUE(joined);
  EXPECT_TRUE(joined.value());
  EXPECT_FALSE(thread.value()->joinable());
  const auto result = thread.value()->result();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), &context.result_value);
  EXPECT_TRUE(context.affinity_called.load(std::memory_order_acquire));
  EXPECT_EQ(context.role.load(std::memory_order_acquire),
            ThreadRole::transport);
  EXPECT_FALSE(thread.value()->join());
}

TEST_F(ThreadingProvider, ResultIsRefusedUntilTheThreadEndsAndMayBeNull) {
  Context null_context;
  native::ThreadEntry null_entry{nullptr, &null_context};
  null_entry.result_function = null_result_entry;
  auto null_thread = provider.start_thread(null_entry);
  ASSERT_TRUE(null_thread);
  const auto pending_result = null_thread.value()->result();
  const auto immediate_join = null_thread.value()->join_for(0ns);
  null_context.release.store(true, std::memory_order_release);
  const auto null_joined = null_thread.value()->join_for(2s);
  const auto null_result = null_thread.value()->result();
  ASSERT_FALSE(pending_result);
  EXPECT_EQ(pending_result.error(), PlatformError::thread_failure);
  ASSERT_TRUE(immediate_join);
  EXPECT_FALSE(immediate_join.value());
  ASSERT_TRUE(null_joined);
  EXPECT_TRUE(null_joined.value());
  ASSERT_TRUE(null_result);
  EXPECT_EQ(null_result.value(), nullptr);
}

TEST_F(ThreadingProvider, AThreadCannotJoinItself) {
  SelfJoinContext self_context;
  auto self_thread = provider.start_thread({self_join_entry, &self_context});
  ASSERT_TRUE(self_thread);
  self_context.thread.store(self_thread.value().get(),
                            std::memory_order_release);
  const auto self_join_deadline = now() + 2s;
  while (!self_context.rejected.load(std::memory_order_acquire) &&
         now() < self_join_deadline)
    pause(1ms);
  EXPECT_TRUE(self_context.rejected.load(std::memory_order_acquire));
  EXPECT_TRUE(self_thread.value()->join());
}

TEST_F(ThreadingProvider, AnEntryWithoutAFunctionIsRefused) {
  const auto invalid = provider.start_thread({nullptr, nullptr});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), PlatformError::thread_failure);
}

TEST_F(ThreadingProvider, OnlyOneOfTwoRacingStopRequestsWins) {
  const auto stop_source = provider.create_stop_source();
  ASSERT_TRUE(stop_source);
  std::atomic<int> stop_winners{0};
  auto stop_one = start([&] {
    if (stop_source.value()->request_stop())
      stop_winners.fetch_add(1, std::memory_order_relaxed);
  });
  auto stop_two = start([&] {
    if (stop_source.value()->request_stop())
      stop_winners.fetch_add(1, std::memory_order_relaxed);
  });
  EXPECT_TRUE(stop_one.join());
  EXPECT_TRUE(stop_two.join());
  EXPECT_EQ(stop_winners.load(std::memory_order_relaxed), 1);
}

TEST_F(ThreadingProvider, OnlyOneOfTwoConcurrentJoinsSucceeds) {
  Context join_context;
  native::ThreadEntry join_entry{nullptr, &join_context};
  join_entry.result_function = result_entry;
  auto joined_once = provider.start_thread(join_entry);
  ASSERT_TRUE(joined_once);
  std::atomic<int> join_successes{0};
  std::atomic<int> join_errors{0};
  auto concurrent_join = [&] {
    const auto value = joined_once.value()->join_for(2s);
    if (value && value.value())
      join_successes.fetch_add(1, std::memory_order_relaxed);
    else if (!value && value.error() == PlatformError::thread_failure)
      join_errors.fetch_add(1, std::memory_order_relaxed);
  };
  auto join_one = start(concurrent_join);
  auto join_two = start(concurrent_join);
  join_context.release.store(true, std::memory_order_release);
  EXPECT_TRUE(join_one.join());
  EXPECT_TRUE(join_two.join());
  EXPECT_EQ(join_successes.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(join_errors.load(std::memory_order_relaxed), 1);
}

class ThreadingProviderConditions : public ThreadingProvider {
 protected:
  void SetUp() override {
    ASSERT_TRUE(condition_mutex);
    ASSERT_TRUE(condition);
  }

  decltype(std::declval<DefaultThreadingProvider&>()
               .create_mutex()) condition_mutex = provider.create_mutex();
  decltype(std::declval<DefaultThreadingProvider&>()
               .create_condition_variable()) condition =
      provider.create_condition_variable();
};

TEST_F(ThreadingProviderConditions, ANotificationWakesAWaitingThread) {
  ConditionContext condition_context{condition_mutex.value().get(),
                                     condition.value().get()};
  auto condition_thread =
      provider.start_thread({condition_entry, &condition_context});
  ASSERT_TRUE(condition_thread);
  while (!condition_context.waiting.load(std::memory_order_acquire)) pause(1ms);
  ASSERT_TRUE(condition_mutex.value()->lock());
  condition_context.release.store(true, std::memory_order_release);
  condition.value()->notify_one();
  EXPECT_TRUE(condition_mutex.value()->unlock());
  EXPECT_TRUE(condition_thread.value()->join());
  EXPECT_TRUE(condition_context.woke.load(std::memory_order_acquire));
}

TEST_F(ThreadingProviderConditions, ATimedWaitReportsItTimedOut) {
  ASSERT_TRUE(condition_mutex.value()->lock());
  const auto condition_timeout =
      condition.value()->wait_for(*condition_mutex.value(), 1ms);
  EXPECT_TRUE(condition_mutex.value()->unlock());
  ASSERT_TRUE(condition_timeout);
  EXPECT_EQ(condition_timeout.value(), ConditionWaitStatus::timed_out);
}

TEST_F(ThreadingProviderConditions, AStopEndsAnUntimedWait) {
  auto condition_stop = provider.create_stop_source();
  ASSERT_TRUE(condition_stop);
  ASSERT_TRUE(condition_mutex.value()->lock());
  auto stop_waiter = start([&] {
    pause(5ms);
    static_cast<void>(condition_stop.value()->request_stop());
  });
  const auto stopped_wait = condition.value()->wait(
      *condition_mutex.value(), condition_stop.value()->token());
  const auto condition_unlock = condition_mutex.value()->unlock();
  EXPECT_TRUE(stop_waiter.join());
  EXPECT_TRUE(condition_unlock);
  ASSERT_TRUE(stopped_wait);
  EXPECT_EQ(stopped_wait.value(), ConditionWaitStatus::stop_requested);
}

TEST_F(ThreadingProviderConditions, AWaitOnAnotherProvidersMutexIsRefused) {
  DefaultThreadingProvider other_provider;
  auto foreign_mutex = other_provider.create_mutex();
  ASSERT_TRUE(foreign_mutex);
  const auto foreign_wait =
      condition.value()->wait_for(*foreign_mutex.value(), 0ns);
  ASSERT_FALSE(foreign_wait);
  EXPECT_EQ(foreign_wait.error(), PlatformError::wrong_provider);
}

TEST_F(ThreadingProviderConditions, AStopEndsTheLongestTimedWait) {
  auto maximum_stop = provider.create_stop_source();
  ASSERT_TRUE(maximum_stop);
  ASSERT_TRUE(condition_mutex.value()->lock());
  auto maximum_stop_waiter = start([&] {
    pause(5ms);
    static_cast<void>(maximum_stop.value()->request_stop());
  });
  const auto maximum_wait = condition.value()->wait_for(
      *condition_mutex.value(), std::chrono::nanoseconds::max(),
      maximum_stop.value()->token());
  const auto maximum_unlock = condition_mutex.value()->unlock();
  EXPECT_TRUE(maximum_stop_waiter.join());
  EXPECT_TRUE(maximum_unlock);
  ASSERT_TRUE(maximum_wait);
  EXPECT_EQ(maximum_wait.value(), ConditionWaitStatus::stop_requested);
}

TEST_F(ThreadingProvider, ProbingAThreadThatIsBeingJoinedDoesNotBlock) {
  Context blocked_context;
  native::ThreadEntry blocked_entry{nullptr, &blocked_context};
  blocked_entry.result_function = result_entry;
  auto blocked = provider.start_thread(blocked_entry);
  ASSERT_TRUE(blocked);
  std::atomic<bool> blocked_joined{false};
  auto blocking_joiner = start([&] {
    blocked_joined.store(static_cast<bool>(blocked.value()->join()),
                         std::memory_order_release);
  });
  auto watchdog = start([&] {
    pause(500ms);
    blocked_context.release.store(true, std::memory_order_release);
  });
  pause(50ms);
  const auto probe_start = now();
  const bool still_joinable = blocked.value()->joinable();
  const auto probe_elapsed = now() - probe_start;
  const auto overlapping_join = blocked.value()->join_for(1ms);
  EXPECT_TRUE(watchdog.join());
  EXPECT_TRUE(blocking_joiner.join());
  EXPECT_TRUE(still_joinable);
  EXPECT_LE(probe_elapsed, 200ms);
  EXPECT_FALSE(overlapping_join && overlapping_join.value());
  EXPECT_TRUE(blocked_joined.load(std::memory_order_acquire));
  EXPECT_FALSE(blocked.value()->joinable());
}

}  // namespace
