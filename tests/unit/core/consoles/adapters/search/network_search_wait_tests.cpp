#include "tests/unit/core/consoles/adapters/search/network_search_rig.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

namespace consoles_test {
namespace {

using libchiaking::consoles::NetworkSearch;
using libchiaking::testing::FakeConsole;
using libchiaking::testing::InMemoryNetwork;

TEST(NetworkSearchCancel, WakesTheOwnerWaitBeforeItStops) {
  ScriptedNetwork network;
  ScriptedClock clock;
  NetworkSearch search(network, SearchRig::threading(), clock,
                       NetworkSearchRig::ranges(), nullptr,
                       NetworkSearchRig::on_loopback());
  std::atomic<bool> waiting{false};
  std::atomic<bool> finished{false};
  auto owner =
      libchiaking::platform::Thread::start(
          SearchRig::threading(), "test-owner",
          libchiaking::platform::ThreadRole::unspecified,
          [&] {
            EXPECT_TRUE(search.start());
            EXPECT_TRUE(search.pump_once());
            waiting = true;
            search.wait(std::chrono::seconds{5});
            while (search.pump_once()) search.wait(std::chrono::seconds{5});
            const auto cancelled = search.pump_once();
            EXPECT_FALSE(cancelled);
            if (!cancelled) {
              EXPECT_EQ(cancelled.error(),
                        NetworkSearch::Failure::invalid_state);
            }
            const auto polled = search.poll();
            EXPECT_FALSE(polled);
            if (!polled) {
              EXPECT_EQ(polled.error(), NetworkSearch::Failure::invalid_state);
            }
            EXPECT_TRUE(search.stop());
            finished = true;
          })
          .value();
  while (!waiting.load()) SearchRig::pause(std::chrono::milliseconds{1});
  search.cancel();
  static_cast<void>(owner.join());
  EXPECT_TRUE(finished.load());
  search.cancel();
  const auto restarted = search.start();
  ASSERT_FALSE(restarted);
  EXPECT_EQ(restarted.error(), NetworkSearch::Failure::invalid_state);
}

class NetworkSearchWait : public ::testing::Test {
 protected:
  InMemoryNetwork network{SearchRig::threading(), SearchRig::clock()};
  FakeConsole peer{network};
  libchiaking::platform::SteadyClock clock;
  NetworkSearch search{network, SearchRig::threading(),
                       clock,   NetworkSearchRig::ranges(),
                       nullptr, NetworkSearchRig::on_loopback()};
};

TEST_F(NetworkSearchWait, EndsAtTheNextDeadline) {
  ASSERT_TRUE(search.start());
  ASSERT_TRUE(search.pump_once());
  const auto bounded = SearchRig::now();
  search.wait(std::chrono::milliseconds{5000});
  const auto until_deadline = SearchRig::now() - bounded;
  EXPECT_GE(until_deadline, std::chrono::milliseconds{80});
  EXPECT_LT(until_deadline, std::chrono::seconds{2});
  const auto immediate = SearchRig::now();
  search.wait(std::chrono::milliseconds{0});
  EXPECT_LT(SearchRig::now() - immediate, std::chrono::seconds{1});
  EXPECT_TRUE(search.stop());
}

TEST_F(NetworkSearchWait, EndsWhenAReplyArrives) {
  ASSERT_TRUE(search.start());
  ASSERT_TRUE(search.pump_once());
  const auto local = search.local_endpoint();
  ASSERT_TRUE(local);
  auto reply = libchiaking::platform::Thread::start(
                   SearchRig::threading(), "test-reply",
                   libchiaking::platform::ThreadRole::unspecified,
                   [&] {
                     SearchRig::pause(std::chrono::milliseconds{40});
                     peer.send_to(local.value().port, NetworkSearchRig::living);
                   })
                   .value();
  const auto started = SearchRig::now();
  search.wait(std::chrono::milliseconds{5000});
  const auto elapsed = SearchRig::now() - started;
  static_cast<void>(reply.join());
  EXPECT_TRUE(SearchRig::within_wake_window(elapsed));
  ASSERT_TRUE(search.pump_once());
  ASSERT_EQ(search.replies().size(), 1U);
  EXPECT_EQ(search.replies().front().hardware_id().text(), "aabbccddeeff");
  EXPECT_TRUE(search.stop());
}

TEST_F(NetworkSearchWait, EndsOnAWake) {
  ASSERT_TRUE(search.start());
  ASSERT_TRUE(search.pump_once());
  EXPECT_TRUE(SearchRig::wake_interrupts_wait(search));
  EXPECT_TRUE(search.stop());
}

TEST_F(NetworkSearchWait, EndsOnAWakeBeforeTheSearchStarts) {
  EXPECT_TRUE(SearchRig::wake_interrupts_wait(search));
}

}  // namespace
}  // namespace consoles_test
