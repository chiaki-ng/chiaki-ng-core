#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/play/adapters/cloud_launch/datacenter_launch_pick.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
namespace play = libchiaking::play;

using Submit = play::DatacenterLaunchPick::Submit;
using play::LaunchPickMiss;

class DatacenterLaunchPick : public ::testing::Test {
 protected:
  auto steady_us() const -> std::uint64_t { return steady_.now_microseconds(); }

  void nap(std::chrono::microseconds duration) const {
    platform::Sleeper{threading_, steady_}.sleep_for(duration);
  }

  auto clock() const {
    return [this] { return steady_us(); };
  }

  auto napping() const {
    return [this](std::chrono::microseconds remaining) {
      nap(std::min(remaining, std::chrono::microseconds{1000}));
    };
  }

  auto threading() const -> platform::DefaultThreadingProvider& {
    return threading_;
  }

 private:
  platform::DefaultThreadingProvider& threading_ =
      test_support::process_threading();
  const platform::SteadyClock steady_;
};

TEST_F(DatacenterLaunchPick, AcceptsOnlyOfferedNamesWhileOpen) {
  play::DatacenterLaunchPick pick{threading()};
  EXPECT_EQ(pick.submit("dc1"), Submit::not_waiting);
  pick.open({"dc1", "dc2"});
  EXPECT_TRUE(pick.waiting());
  EXPECT_EQ(pick.submit("nowhere"), Submit::not_offered);
  EXPECT_TRUE(pick.waiting());
  EXPECT_FALSE(pick.take());
  EXPECT_EQ(pick.submit("Auto"), Submit::accepted);
  EXPECT_EQ(pick.submit("dc2"), Submit::accepted);
  EXPECT_EQ(pick.take(), std::optional<std::string>{"dc2"});
  EXPECT_FALSE(pick.waiting());
  EXPECT_EQ(pick.submit("dc1"), Submit::not_waiting);
}

TEST_F(DatacenterLaunchPick, EndsTheWaitWhenTheUserPicksFromAnotherThread) {
  play::DatacenterLaunchPick pick{threading()};
  pick.open({"dc1", "dc2"});
  const platform::StopToken running;
  auto chooser = platform::Thread::start(
      threading(), "chooser", platform::ThreadRole::unspecified, [&] {
        nap(std::chrono::milliseconds{20});
        EXPECT_EQ(pick.submit("dc9"), Submit::not_offered);
        EXPECT_EQ(pick.submit("dc2"), Submit::accepted);
      });
  ASSERT_TRUE(chooser);
  const auto began = steady_us();
  auto outcome =
      pick.await(std::chrono::seconds{10}, running, clock(), napping());
  static_cast<void>(chooser.value().join());
  EXPECT_EQ(outcome, "dc2");
  EXPECT_LT(steady_us() - began, 5'000'000u);
}

TEST_F(DatacenterLaunchPick, TimesOutAndCloses) {
  play::DatacenterLaunchPick pick{threading()};
  pick.open({"dc1"});
  const platform::StopToken running;
  std::uint64_t now = 1'000;
  std::vector<std::chrono::microseconds> waits;
  auto outcome = pick.await(
      std::chrono::milliseconds{15000}, running, [&] { return now; },
      [&](std::chrono::microseconds remaining) {
        waits.push_back(remaining);
        now += 5'000'000;
      });
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error(), LaunchPickMiss::timed_out);
  ASSERT_EQ(waits.size(), 3u);
  EXPECT_EQ(waits.front(), std::chrono::microseconds{15'000'000});
  EXPECT_FALSE(pick.waiting());
  EXPECT_EQ(pick.submit("dc1"), Submit::not_waiting);
}

TEST_F(DatacenterLaunchPick, HonoursAPickRacingTheDeadline) {
  play::DatacenterLaunchPick pick{threading()};
  pick.open({"dc1"});
  const platform::StopToken running;
  std::uint64_t calls = 0;
  auto outcome = pick.await(
      std::chrono::milliseconds{10}, running,
      [&] {
        if (++calls == 3) {
          EXPECT_EQ(pick.submit("dc1"), Submit::accepted);
        }
        return calls >= 3 ? std::uint64_t{20'000} : std::uint64_t{0};
      },
      [&](std::chrono::microseconds) {});
  EXPECT_EQ(outcome, "dc1");
}

TEST_F(DatacenterLaunchPick, CancelsTheWaitOnStop) {
  play::DatacenterLaunchPick pick{threading()};
  pick.open({"dc1"});
  auto flag = std::make_shared<test_support::AtomicStop>();
  const platform::StopToken stop{flag};
  auto canceller = platform::Thread::start(
      threading(), "canceller", platform::ThreadRole::unspecified, [&] {
        nap(std::chrono::milliseconds{20});
        flag->stopped.store(true, std::memory_order_release);
      });
  ASSERT_TRUE(canceller);
  auto outcome = pick.await(std::chrono::seconds{10}, stop, clock(), napping());
  static_cast<void>(canceller.value().join());
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error(), LaunchPickMiss::cancelled);
  EXPECT_EQ(pick.submit("dc1"), Submit::not_waiting);
}

}  // namespace
}  // namespace play_test
