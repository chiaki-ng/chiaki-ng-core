#include "src/core/utils/concurrency/single_flight.hpp"
#include "tests/support/process_threading.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace concurrency_test {
namespace {

namespace platform = libchiaking::platform;
using libchiaking::platform::StopSource;
using libchiaking::platform::StopToken;

class SingleFlight : public ::testing::Test {
 protected:
  template <class T>
  using Flight = libchiaking::concurrency::SingleFlight<T>;

  class Signal {
   public:
    Signal(platform::ThreadingProvider& threading,
           const platform::ClockProvider& clock)
        : mutex_(threading), changed_(threading, clock) {}

    void raise() {
      const platform::Lock lock{mutex_};
      raised_ = true;
      changed_.notify_all();
    }

    void wait() {
      platform::Lock lock{mutex_};
      changed_.wait(lock, [this] { return raised_; });
    }

   private:
    platform::Mutex mutex_;
    platform::ConditionVariable changed_;
    bool raised_{};
  };

  auto started(auto work) -> platform::Thread {
    return platform::Thread::start(threading, "single flight test",
                                   platform::ThreadRole::unspecified,
                                   std::move(work))
        .value();
  }

  platform::DefaultThreadingProvider& threading =
      test_support::process_threading();
  platform::SteadyClock monotonic;
  platform::Mutex mutex{threading};
};

TEST_F(SingleFlight, LetsALoneCallerLeadAndLandItsOutcome) {
  Flight<std::string> flight{threading, monotonic};
  platform::Lock lock{mutex};
  EXPECT_FALSE(flight.in_flight());
  EXPECT_FALSE(flight.landed());
  const auto answer = flight.lead(
      lock, [] { return std::string{"answer"}; },
      [](const std::string& landed) { return landed + "!"; });
  EXPECT_TRUE(lock.owns_lock());
  EXPECT_EQ(answer, "answer");
  EXPECT_EQ(flight.landed(), "answer!");
  EXPECT_FALSE(flight.in_flight());
  flight.forget();
  EXPECT_FALSE(flight.landed());
}

TEST_F(SingleFlight, SharesTheOutcomeInProgressWithAWaiter) {
  Flight<int> flight{threading, monotonic};
  Signal departed{threading, monotonic};
  Signal release{threading, monotonic};
  std::atomic<int> computations{};

  auto leader = started([&] {
    platform::Lock lock{mutex};
    static_cast<void>(flight.lead(
        lock,
        [&] {
          ++computations;
          departed.raise();
          release.wait();
          return 42;
        },
        [](int landed) { return landed; }));
  });

  departed.wait();
  platform::Lock lock{mutex};
  EXPECT_TRUE(flight.in_flight());
  auto releaser = started([&] {
    platform::Sleeper{threading, monotonic}.sleep_for(
        std::chrono::milliseconds{30});
    release.raise();
  });
  EXPECT_TRUE(flight.await_landing(lock, StopToken{}));
  EXPECT_FALSE(flight.in_flight());
  EXPECT_EQ(flight.landed(), 42);
  EXPECT_EQ(computations.load(), 1);
  lock.unlock();
  EXPECT_TRUE(leader.join());
  EXPECT_TRUE(releaser.join());
}

TEST_F(SingleFlight, LetsAStoppedWaiterGiveUpWithoutTheOutcome) {
  Flight<int> flight{threading, monotonic};
  Signal departed{threading, monotonic};
  Signal release{threading, monotonic};

  auto leader = started([&] {
    platform::Lock lock{mutex};
    static_cast<void>(flight.lead(
        lock,
        [&] {
          departed.raise();
          release.wait();
          return 1;
        },
        [](int landed) { return landed; }));
  });

  departed.wait();
  StopSource stop;
  stop.request_stop();
  {
    platform::Lock lock{mutex};
    EXPECT_FALSE(flight.await_landing(lock, stop.token()));
    EXPECT_TRUE(flight.in_flight());
    EXPECT_FALSE(flight.landed());
  }
  release.raise();
  EXPECT_TRUE(leader.join());
  EXPECT_EQ(flight.landed(), 1);
}

TEST_F(SingleFlight, LandsWhatTheLeaderChoseWhenAbandoned) {
  Flight<std::string> flight{threading, monotonic};
  platform::Lock lock{mutex};
  bool thrown = false;
  try {
    static_cast<void>(flight.lead(
        lock, []() -> std::string { throw std::runtime_error{"lost"}; },
        [](const std::string& landed) { return landed; },
        [] { return std::string{"abandoned"}; }));
  } catch (const std::runtime_error&) {
    thrown = true;
  }
  EXPECT_TRUE(thrown);
  EXPECT_TRUE(lock.owns_lock());
  EXPECT_FALSE(flight.in_flight());
  EXPECT_EQ(flight.landed(), "abandoned");
}

TEST_F(SingleFlight, KeepsTheLastOutcomeWhenAbandonedWithoutAChoice) {
  Flight<std::string> flight{threading, monotonic};
  platform::Lock lock{mutex};
  static_cast<void>(flight.lead(
      lock, [] { return std::string{"first"}; },
      [](const std::string& landed) { return landed; }));
  try {
    static_cast<void>(flight.lead(
        lock, []() -> std::string { throw std::runtime_error{"lost"}; },
        [](const std::string& landed) { return landed; }));
  } catch (const std::runtime_error&) {
  }
  EXPECT_TRUE(lock.owns_lock());
  EXPECT_FALSE(flight.in_flight());
  EXPECT_EQ(flight.landed(), "first");
}

}  // namespace
}  // namespace concurrency_test
