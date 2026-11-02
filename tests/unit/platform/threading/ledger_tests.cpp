#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <exception>
#include <memory>

namespace {

using namespace libchiaking::platform;

TEST(ThreadingLedger, ReleasedObjectsLetTheProviderGo) {
  auto threading = std::make_unique<DefaultThreadingProvider>();
  SteadyClock clock;
  {
    Mutex mutex{*threading};
    ConditionVariable changed{*threading, clock};
    auto worker =
        Thread::start(*threading, "ledger", ThreadRole::session, [] {});
    ASSERT_TRUE(worker);
    ASSERT_TRUE(worker->join());
    auto stop = threading->create_stop_source();
    ASSERT_TRUE(stop);
  }
  threading.reset();
}

TEST(ThreadingLedgerDeathTest, AProviderOutlivedByItsMutexTerminates) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(
      {
        std::set_terminate([] { std::_Exit(EXIT_SUCCESS); });
        auto threading = std::make_unique<DefaultThreadingProvider>();
        auto outliving = std::make_unique<Mutex>(*threading);
        threading.reset();
        std::_Exit(EXIT_FAILURE);
      },
      ::testing::ExitedWithCode(EXIT_SUCCESS), "");
}

}  // namespace
