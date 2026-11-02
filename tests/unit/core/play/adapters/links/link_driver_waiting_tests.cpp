#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/link_driver_rig.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

namespace play_test {

using test_support::LiveProviders;
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using link_drivers::Rig;
using remote_play::StreamState;

TEST(LinkDriverWaiting, LetsAnotherThreadPollStateAndDiagnostics) {
  Rig rig;
  std::atomic<bool> done{false};
  std::atomic<unsigned> polls{0};
  auto poller = LiveProviders::spawn("test", [&] {
    while (!done.load()) {
      const auto state = rig.driver().state();
      EXPECT_GE(state, StreamState::idle);
      EXPECT_LE(state, StreamState::failed);
      const auto diagnostics = rig.driver().diagnostics();
      EXPECT_LE(diagnostics.video_units, 1000000U);
      polls.fetch_add(1);
    }
  });
  while (polls.load() == 0) LiveProviders::nap(std::chrono::milliseconds{1});
  EXPECT_EQ(rig.driver().state(), StreamState::idle);
  EXPECT_EQ(rig.driver().diagnostics().video_units, 0U);
  EXPECT_TRUE(rig.driver().start());
  for (unsigned index = 0; index < 200; ++index) {
    EXPECT_TRUE(rig.driver().pump_once());
    EXPECT_TRUE(rig.driver().wait(std::chrono::milliseconds{0}));
  }
  EXPECT_EQ(rig.driver().state(), StreamState::establishing_route);
  EXPECT_TRUE(rig.driver().diagnostics().available);
  EXPECT_EQ(rig.driver().diagnostics().video_units, 200U);
  EXPECT_TRUE(rig.driver().stop());
  EXPECT_EQ(rig.driver().state(), StreamState::stopped);
  done = true;
  static_cast<void>(poller.join());
}

TEST(LinkDriverWaiting, EndsAWaitWhenWoken) {
  Rig rig;
  auto waker = LiveProviders::spawn("test", [&] {
    LiveProviders::nap(std::chrono::milliseconds{50});
    rig.driver().wake();
  });
  const auto began = LiveProviders::steady_us();
  EXPECT_TRUE(rig.driver().wait(std::chrono::milliseconds{5000}));
  const auto elapsed = Rig::since(began);
  static_cast<void>(waker.join());
  EXPECT_GE(elapsed, std::chrono::milliseconds{40});
  EXPECT_LT(elapsed, std::chrono::seconds{2});
}

TEST(LinkDriverWaiting, WaitsOutItsTimeoutWhenNothingHappens) {
  Rig rig;
  const auto again = LiveProviders::steady_us();
  EXPECT_TRUE(rig.driver().wait(std::chrono::milliseconds{60}));
  EXPECT_GE(Rig::since(again), std::chrono::milliseconds{50});
  EXPECT_EQ(rig.driver().state(), StreamState::idle);
}

TEST(LinkDriverWaiting, RaisesTheStopAndEndsAWaitWhenCancelled) {
  Rig rig;
  EXPECT_FALSE(rig.stop().stop_requested());
  const auto token = rig.stop().token();
  rig.driver().cancel();
  EXPECT_TRUE(rig.stop().stop_requested());
  EXPECT_TRUE(token.stop_requested());
  EXPECT_TRUE(rig.logged("session cancellation requested"));
  const auto cancelled = LiveProviders::steady_us();
  EXPECT_TRUE(rig.driver().wait(std::chrono::milliseconds{5000}));
  EXPECT_LT(Rig::since(cancelled), std::chrono::seconds{2});
}

}  // namespace
}  // namespace play_test
