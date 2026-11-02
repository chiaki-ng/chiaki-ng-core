#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/manual_clock.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>

namespace {

using namespace libchiaking::platform;
using namespace std::chrono_literals;

class ThreadingHandles : public ::testing::Test {
 protected:
  DefaultThreadingProvider threading;
  SteadyClock clock;
};

TEST_F(ThreadingHandles, LockExcludesOtherThreads) {
  Mutex mutex{threading};
  ConditionVariable changed{threading, clock};
  int inside = 0;
  int most_inside = 0;
  int finished = 0;
  auto body = [&] {
    for (int round = 0; round < 1000; ++round) {
      Lock lock{mutex};
      ++inside;
      most_inside = std::max(most_inside, inside);
      --inside;
    }
    Lock lock{mutex};
    ++finished;
    changed.notify_all();
  };
  auto first = Thread::start(threading, "first", ThreadRole::session, body);
  auto second = Thread::start(threading, "second", ThreadRole::session, body);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  Lock lock{mutex};
  changed.wait(lock, [&] { return finished == 2; });
  EXPECT_EQ(most_inside, 1);
}

TEST_F(ThreadingHandles, DeferredAndTryLocksReportOwnership) {
  Mutex mutex{threading};
  auto deferred = Lock::deferred(mutex);
  EXPECT_FALSE(deferred.owns_lock());
  deferred.lock();
  EXPECT_TRUE(deferred.owns_lock());
  std::atomic<bool> taken{true};
  auto other = Thread::start(threading, "probe", ThreadRole::session, [&] {
    taken.store(Lock::try_to_take(mutex).owns_lock());
  });
  ASSERT_TRUE(other);
  ASSERT_TRUE(other->join());
  EXPECT_FALSE(taken.load());
  deferred.unlock();
  EXPECT_TRUE(Lock::try_to_take(mutex).owns_lock());
}

TEST_F(ThreadingHandles, MovedLockReleasesOnce) {
  Mutex mutex{threading};
  {
    Lock first{mutex};
    Lock second{std::move(first)};
    EXPECT_FALSE(first.owns_lock());
    EXPECT_TRUE(second.owns_lock());
  }
  EXPECT_TRUE(Lock::try_to_take(mutex).owns_lock());
}

TEST_F(ThreadingHandles, TimedWaitGivesUp) {
  Mutex mutex{threading};
  ConditionVariable never{threading, clock};
  Lock lock{mutex};
  const auto begun = clock.now_microseconds();
  const bool ready = never.wait_for(lock, 20ms, [] { return false; });
  const auto waited = clock.now_microseconds() - begun;
  EXPECT_FALSE(ready);
  EXPECT_GE(waited, 15'000U);
  EXPECT_TRUE(lock.owns_lock());
}

TEST_F(ThreadingHandles, TimedWaitEndsOnAClockThatNeverMoves) {
  libchiaking::testing::ManualClock stopped;
  Mutex mutex{threading};
  ConditionVariable never{threading, stopped};
  Lock lock{mutex};
  EXPECT_FALSE(never.wait_for(lock, 10ms, [] { return false; }));
}

TEST_F(ThreadingHandles, StopEndsAWait) {
  Mutex mutex{threading};
  ConditionVariable never{threading, clock};
  StopSource stop;
  auto stopper = Thread::start(threading, "stopper", ThreadRole::session,
                               [&] { stop.request_stop(); });
  ASSERT_TRUE(stopper);
  Lock lock{mutex};
  EXPECT_FALSE(never.wait(lock, stop.token(), [] { return false; }));
}

TEST_F(ThreadingHandles, ThreadRunsOwnedWorkAndStopsOnDestruction) {
  Mutex mutex{threading};
  ConditionVariable changed{threading, clock};
  bool started = false;
  std::atomic<bool> saw_stop{false};
  auto owned = std::make_unique<int>(7);
  {
    auto running = Thread::start(
        threading, "worker", ThreadRole::transport,
        [&, owned = std::move(owned)](StopToken token) {
          {
            Lock lock{mutex};
            started = *owned == 7;
            changed.notify_all();
          }
          Lock lock{mutex};
          saw_stop.store(!changed.wait(lock, token, [] { return false; }));
        });
    ASSERT_TRUE(running);
    Lock lock{mutex};
    changed.wait(lock, [&] { return started; });
  }
  EXPECT_TRUE(saw_stop.load());
}

TEST_F(ThreadingHandles, EmptyAndMovedHandlesRunNothing) {
  Thread empty;
  EXPECT_FALSE(empty.joinable());
  EXPECT_FALSE(empty.join());
  std::atomic<int> runs{0};
  auto started = Thread::start(threading, "moved", ThreadRole::session,
                               [&] { runs.fetch_add(1); });
  ASSERT_TRUE(started);
  Thread moved = std::move(*started);
  EXPECT_FALSE(started->joinable());
  EXPECT_TRUE(moved.join());
  EXPECT_EQ(runs.load(), 1);
  EXPECT_FALSE(moved.joinable());
}

TEST_F(ThreadingHandles, SleeperPausesAndStopsEarly) {
  Sleeper sleeper{threading, clock};
  const auto begun = clock.now_microseconds();
  EXPECT_TRUE(sleeper.sleep_for(10ms));
  EXPECT_GE(clock.now_microseconds() - begun, 8'000U);
  StopSource stop;
  stop.request_stop();
  EXPECT_FALSE(sleeper.sleep_for(10s, stop.token()));
}

TEST_F(ThreadingHandles, IdentitiesOutliveTheirThreads) {
  for (int round = 0; round < 200; ++round) {
    ThreadIdentity ended;
    auto worker = Thread::start(threading, "ended", ThreadRole::session,
                                [&] { ended = ThreadIdentity::current(); });
    ASSERT_TRUE(worker);
    ASSERT_TRUE(worker->join());
    ASSERT_TRUE(ended) << "round " << round;
    ThreadIdentity later;
    auto next = Thread::start(threading, "later", ThreadRole::session,
                              [&] { later = ThreadIdentity::current(); });
    ASSERT_TRUE(next);
    ASSERT_TRUE(next->join());
    ASSERT_FALSE(later == ended) << "round " << round;
    ASSERT_FALSE(ThreadIdentity::current() == ended) << "round " << round;
  }
  EXPECT_TRUE(ThreadIdentity::current() == ThreadIdentity::current());
  EXPECT_FALSE(ThreadIdentity{});
}

}  // namespace
