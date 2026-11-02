#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/internet_link_rig.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

namespace play_test {

using test_support::LiveProviders;
namespace {

namespace platform = libchiaking::platform;
using DataRouteTask = libchiaking::play::InternetStart::DataRouteTask;
using RouteFailure = libchiaking::play::RouteFailure;

TEST(InternetLinkDataRoute, CancelsARunningWorkerAndReportsItsCompletion) {
  platform::DefaultThreadingProvider threading;
  std::atomic<bool> entered{};
  std::atomic<bool> cancelled{};
  DataRouteTask task{
      7, {}, [&](platform::StopToken stop) -> DataRouteTask::Result {
        entered.store(true, std::memory_order_release);
        while (!stop.stop_requested())
          LiveProviders::nap(std::chrono::milliseconds{1});
        cancelled.store(true, std::memory_order_release);
        return libchiaking::unexpected(RouteFailure{RouteFailure::Cancelled{}});
      }};
  ASSERT_TRUE(task.start(threading));
  const auto deadline = LiveProviders::steady_us() + 2'000'000;
  while (!entered.load(std::memory_order_acquire) && LiveProviders::steady_us() < deadline)
    LiveProviders::nap(std::chrono::milliseconds{1});
  EXPECT_TRUE(entered.load(std::memory_order_acquire));
  EXPECT_FALSE(task.take_completion());
  task.cancel_and_join();
  EXPECT_LT(LiveProviders::steady_us(), deadline);
  EXPECT_TRUE(cancelled.load(std::memory_order_acquire));
  auto completion = task.take_completion();
  ASSERT_TRUE(completion);
  EXPECT_EQ(completion->attempt_id, 7U);
  ASSERT_FALSE(completion->result);
  EXPECT_TRUE(completion->result.error().is<RouteFailure::Cancelled>());
  EXPECT_FALSE(task.take_completion());
}

TEST(InternetLinkDataRoute, ReportsAWorkerThatThrowsAsASystemFailure) {
  platform::DefaultThreadingProvider threading;
  DataRouteTask throwing{
      8, {}, [](platform::StopToken) -> DataRouteTask::Result { throw 1; }};
  ASSERT_TRUE(throwing.start(threading));
  throwing.cancel_and_join();
  const auto failure = throwing.take_completion();
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->worker_error, platform::PlatformError::system_failure);
}

}  // namespace
}  // namespace play_test
