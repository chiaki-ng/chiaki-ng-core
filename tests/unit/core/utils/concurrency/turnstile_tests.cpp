#include "src/core/utils/concurrency/turnstile.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace concurrency_test {
namespace {

namespace platform = libchiaking::platform;
using libchiaking::platform::StopSource;

class Turnstile : public ::testing::Test {
 protected:
  using Gate = libchiaking::concurrency::Turnstile;
  using Entry = libchiaking::Expected<Gate::Pass, Gate::Refusal>;

  auto started(auto work) -> platform::Thread {
    return platform::Thread::start(threading, "turnstile test",
                                   platform::ThreadRole::unspecified,
                                   std::move(work))
        .value();
  }

  auto wait_until(const auto& done) -> testing::AssertionResult {
    platform::Sleeper sleeper{threading, monotonic};
    const auto deadline = monotonic.now_microseconds() + 5'000'000;
    while (!done()) {
      if (monotonic.now_microseconds() >= deadline)
        return testing::AssertionFailure()
               << "the turnstile did not settle within five seconds";
      sleeper.sleep_for(std::chrono::milliseconds{1});
    }
    return testing::AssertionSuccess();
  }

  platform::DefaultThreadingProvider& threading =
      test_support::process_threading();
  platform::SteadyClock monotonic;
};

TEST_F(Turnstile, LetsLateCallersInInTheOrderTheyCame) {
  Gate turnstile{threading, monotonic, 2, 8};
  auto first = turnstile.enter({});
  auto second = turnstile.enter({});
  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
  EXPECT_EQ(turnstile.inside(), 2U);
  platform::Mutex mutex{threading};
  std::vector<int> order;
  std::vector<platform::Thread> late;
  for (int at = 0; at < 3; ++at) {
    late.push_back(started([&, at] {
      auto pass = turnstile.enter({});
      EXPECT_TRUE(pass);
      const platform::Lock lock{mutex};
      order.push_back(at);
    }));
    ASSERT_TRUE(wait_until([&] {
      return turnstile.waiting() == static_cast<std::size_t>(at + 1);
    }));
  }
  EXPECT_EQ(turnstile.inside(), 2U);
  {
    auto gone = std::move(first);
  }
  late.clear();
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2}));
  EXPECT_EQ(turnstile.inside(), 1U);
  EXPECT_EQ(turnstile.waiting(), 0U);
}

TEST_F(Turnstile, LetsAStoppedWaiterLeaveTheLineWithoutEntering) {
  Gate turnstile{threading, monotonic, 1, 8};
  auto held = turnstile.enter({});
  ASSERT_TRUE(held);
  StopSource scrolled_away;
  std::optional<Entry> left;
  auto waiter =
      started([&] { left.emplace(turnstile.enter(scrolled_away.token())); });
  ASSERT_TRUE(wait_until([&] { return turnstile.waiting() == 1; }));
  scrolled_away.request_stop();
  EXPECT_TRUE(waiter.join());
  ASSERT_TRUE(left.has_value());
  ASSERT_FALSE(*left);
  EXPECT_EQ(left->error(), Gate::Refusal::stopped);
  EXPECT_EQ(turnstile.waiting(), 0U);
  EXPECT_EQ(turnstile.inside(), 1U);
}

TEST_F(Turnstile, TurnsAwayACallerThatIsAlreadyStopped) {
  Gate turnstile{threading, monotonic, 1, 8};
  auto held = turnstile.enter({});
  ASSERT_TRUE(held);
  StopSource already;
  already.request_stop();
  const auto never = turnstile.enter(already.token());
  ASSERT_FALSE(never);
  EXPECT_EQ(never.error(), Gate::Refusal::stopped);
  {
    auto gone = std::move(*held);
  }
  const auto free = turnstile.enter(already.token());
  EXPECT_FALSE(free);
  EXPECT_EQ(turnstile.inside(), 0U);
}

TEST_F(Turnstile, TurnsAwayTheNextCallerWhenTheLineIsFull) {
  Gate turnstile{threading, monotonic, 1, 2};
  auto held = turnstile.enter({});
  StopSource done;
  std::vector<platform::Thread> queued;
  for (int at = 0; at < 2; ++at)
    queued.push_back(started([&] { (void)turnstile.enter(done.token()); }));
  ASSERT_TRUE(wait_until([&] { return turnstile.waiting() == 2; }));
  const auto crowded = turnstile.enter({});
  ASSERT_FALSE(crowded);
  EXPECT_EQ(crowded.error(), Gate::Refusal::crowded);
  done.request_stop();
  queued.clear();
  EXPECT_EQ(turnstile.waiting(), 0U);
}

TEST_F(Turnstile, NeverLetsManyCallersPastTheLimit) {
  static constexpr std::size_t limit = 4;
  Gate turnstile{threading, monotonic, limit, 64};
  std::atomic<std::size_t> inside{};
  std::atomic<std::size_t> most{};
  std::atomic<int> entered{};
  {
    std::vector<platform::Thread> callers;
    for (int at = 0; at < 50; ++at)
      callers.push_back(started([&] {
        auto pass = turnstile.enter({});
        EXPECT_TRUE(pass);
        const auto now = ++inside;
        for (auto seen = most.load();
             now > seen && !most.compare_exchange_weak(seen, now);) {
        }
        platform::Sleeper{threading, monotonic}.sleep_for(
            std::chrono::milliseconds{2});
        --inside;
        ++entered;
      }));
  }
  EXPECT_EQ(entered.load(), 50);
  EXPECT_LE(most.load(), limit);
  EXPECT_GE(most.load(), 2U);
  EXPECT_EQ(turnstile.inside(), 0U);
  EXPECT_EQ(turnstile.waiting(), 0U);
}

}  // namespace
}  // namespace concurrency_test
