#include "tests/support/enum_printing.hpp"

#include "tests/support/stop_states.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

namespace {

using namespace libchiaking::platform;
using namespace std::chrono_literals;

class StopWait : public ::testing::Test {
 protected:
  auto now() const -> std::chrono::microseconds {
    return std::chrono::microseconds{
        static_cast<std::int64_t>(clock.now_microseconds())};
  }

  void nap(std::chrono::milliseconds duration) {
    static_cast<void>(pause.sleep_for(duration));
  }

  template <class Work>
  auto start(Work work) -> Thread {
    auto started = Thread::start(threading, "stop-wait-test",
                                 ThreadRole::unspecified, std::move(work));
    EXPECT_TRUE(started);
    if (!started) return Thread{};
    return std::move(started).value();
  }

  DefaultThreadingProvider threading;
  SteadyClock clock;
  Mutex mutex{threading};
  ConditionVariable condition{threading, clock};
  Sleeper pause{threading, clock};
};

TEST_F(StopWait, ATimedWaitWithAStopTokenWakesOnlyWhenItEnds) {
  StopSource stop;
  Lock lock{mutex};
  int evaluations = 0;
  const auto started = now();
  const bool ready = condition.wait_for(lock, 50ms, stop.token(), [&] {
    ++evaluations;
    return false;
  });
  EXPECT_FALSE(ready);
  EXPECT_GE(now() - started, 50ms);
  EXPECT_LE(evaluations, 3);
}

TEST_F(StopWait, ATimedWaitWithoutAStopTokenWakesOnlyWhenItEnds) {
  Lock lock{mutex};
  int evaluations = 0;
  const auto started = now();
  const bool ready = condition.wait_for(lock, 50ms, [&] {
    ++evaluations;
    return false;
  });
  EXPECT_FALSE(ready);
  EXPECT_GE(now() - started, 50ms);
  EXPECT_LE(evaluations, 3);
}

TEST_F(StopWait, AStopEndsALongWaitPromptly) {
  StopSource stop;
  std::atomic<bool> waiting{false};
  std::atomic<bool> finished{false};
  {
    auto waiter = start([&] {
      Lock lock{mutex};
      waiting.store(true, std::memory_order_release);
      const bool ready = condition.wait_for(lock, std::chrono::minutes{10},
                                            stop.token(), [] { return false; });
      EXPECT_FALSE(ready);
      finished.store(true, std::memory_order_release);
    });
    while (!waiting.load(std::memory_order_acquire)) nap(1ms);
    nap(20ms);
    const auto requested = now();
    EXPECT_TRUE(stop.request_stop());
    EXPECT_TRUE(waiter.join());
    EXPECT_LT(now() - requested, 1s);
  }
  EXPECT_TRUE(finished.load(std::memory_order_acquire));
}

TEST_F(StopWait, AStopRacingTheStartOfAWaitIsNeverLost) {
  for (int round = 0; round < 500; ++round) {
    StopSource stop;
    auto waiter = start([&] {
      Lock lock{mutex};
      const auto status = condition.wait(lock, stop.token());
      EXPECT_EQ(status, ConditionWaitStatus::stop_requested);
    });
    ASSERT_TRUE(stop.request_stop()) << "round " << round;
    const auto joined = waiter.join_for(5s);
    ASSERT_TRUE(joined) << "round " << round;
    ASSERT_TRUE(joined.value()) << "round " << round;
  }
}

TEST_F(StopWait, ANotificationStillEndsAWaitWithAStopToken) {
  StopSource stop;
  bool flag = false;
  auto notifier = start([&] {
    nap(10ms);
    Lock lock{mutex};
    flag = true;
    condition.notify_all();
  });
  {
    Lock lock{mutex};
    const auto started = now();
    const bool ready = condition.wait_for(lock, std::chrono::minutes{10},
                                          stop.token(), [&] { return flag; });
    EXPECT_TRUE(ready);
    EXPECT_LT(now() - started, 1s);
  }
  EXPECT_TRUE(notifier.join());
}

TEST_F(StopWait, AStopStateThatCanOnlyBePolledStillEndsAWait) {
  auto state = std::make_shared<test_support::AtomicStop>();
  const StopToken token{state};
  auto stopper = start([&] {
    nap(20ms);
    state->stopped.store(true, std::memory_order_release);
  });
  {
    Lock lock{mutex};
    const auto started = now();
    const bool ready = condition.wait_for(lock, std::chrono::minutes{10}, token,
                                          [] { return false; });
    EXPECT_FALSE(ready);
    EXPECT_LT(now() - started, 1s);
  }
  EXPECT_TRUE(stopper.join());
}

TEST_F(StopWait, ASleeperWaitsOutItsTimeAndAStopCutsItShort) {
  Sleeper sleeper{threading, clock};
  StopSource stop;
  const auto started = now();
  EXPECT_TRUE(sleeper.sleep_for(30ms, stop.token()));
  EXPECT_GE(now() - started, 30ms);
  auto stopper = start([&] {
    nap(20ms);
    static_cast<void>(stop.request_stop());
  });
  const auto cut = now();
  EXPECT_FALSE(sleeper.sleep_for(std::chrono::minutes{10}, stop.token()));
  EXPECT_LT(now() - cut, 1s);
  EXPECT_TRUE(stopper.join());
}

}  // namespace
