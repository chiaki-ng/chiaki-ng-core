#include "tests/unit/core/consoles/adapters/search/network_search_rig.hpp"

#include <gtest/gtest.h>

namespace consoles_test {
namespace {

using libchiaking::consoles::Beacon;
using libchiaking::consoles::NetworkSearch;
using libchiaking::consoles::SearchRanges;
using libchiaking::platform::NetError;
using libchiaking::platform::SocketEndpoint;

class NetworkSearchSends : public ::testing::Test {
 protected:
  NetworkSearchSends() {
    network.memory.interface_list = {NetworkSearchRig::routed_interface()};
  }

  ScriptedNetwork network;
  ScriptedClock clock;
};

TEST_F(NetworkSearchSends, ContinuePastAnUnreachableNetwork) {
  network.send_error = NetError::network_unreachable;
  NetworkSearch unreachable(network, SearchRig::threading(), clock,
                            NetworkSearchRig::ranges(true, {"198.51.100.0/31"}),
                            nullptr, NetworkSearchRig::on_loopback());
  ASSERT_TRUE(unreachable.start());
  ASSERT_TRUE(unreachable.pump_once());
  EXPECT_EQ(network.sent.size(), 4U);
  EXPECT_EQ(NetworkSearchRig::progress_of(unreachable).visited, 0U);
  EXPECT_FALSE(NetworkSearchRig::progress_of(unreachable).permission_denied);
  EXPECT_TRUE(unreachable.stop());
}

TEST_F(NetworkSearchSends, PauseAndReportAPermissionDenial) {
  network.send_error = NetError::permission_denied;
  NetworkSearch denied(network, SearchRig::threading(), clock,
                       NetworkSearchRig::ranges(true, {"198.51.100.0/31"}),
                       nullptr, NetworkSearchRig::on_loopback());
  ASSERT_TRUE(denied.start());
  ASSERT_TRUE(denied.pump_once());
  ASSERT_EQ(network.sent.size(), 1U);
  EXPECT_EQ(network.sent.front(),
            (SocketEndpoint{SearchRig::ip(198, 51, 100, 0), Beacon::ps4_port}));
  EXPECT_TRUE(NetworkSearchRig::progress_of(denied).permission_denied);
  ASSERT_TRUE(denied.pump_once());
  EXPECT_EQ(network.sent.size(), 1U);
  EXPECT_TRUE(NetworkSearchRig::progress_of(denied).permission_denied);
  network.send_error.reset();
  clock.now = 15'000'000;
  ASSERT_TRUE(denied.pump_once());
  EXPECT_EQ(network.sent.size(), 5U);
  EXPECT_FALSE(NetworkSearchRig::progress_of(denied).permission_denied);
  EXPECT_TRUE(denied.stop());
}

TEST_F(NetworkSearchSends, StopAtTheDestinationWhereTheSearchWasCancelled) {
  NetworkSearch search(network, SearchRig::threading(), clock,
                       NetworkSearchRig::ranges(true, {"198.51.100.0/31"}),
                       nullptr, NetworkSearchRig::on_loopback());
  ASSERT_TRUE(search.start());
  network.on_send = [&] { search.cancel(); };
  const auto result = search.pump_once();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), NetworkSearch::Failure::invalid_state);
  EXPECT_TRUE(search.cancelled());
  ASSERT_EQ(network.sent.size(), 2U);
  EXPECT_EQ(network.sent[0].address, SearchRig::ip(198, 51, 100, 0));
  EXPECT_EQ(network.sent[1].address, SearchRig::ip(198, 51, 100, 0));
  EXPECT_TRUE(search.stop());
}

TEST(NetworkSearchRefusedSends, AreRetriedOnTheBroadcastCadence) {
  ScriptedNetwork network;
  network.unreachable = [](const SocketEndpoint&) { return true; };
  ScriptedClock clock;
  clock.now = 1'000'000;
  NetworkSearch search(network, SearchRig::threading(), clock, SearchRanges{});
  ASSERT_TRUE(search.start());
  ASSERT_TRUE(search.pump_once());
  EXPECT_EQ(network.refused_sends.size(), 2U);
  clock.now += 500'000;
  ASSERT_TRUE(search.pump_once());
  EXPECT_EQ(network.refused_sends.size(), 2U);
  clock.now += 500'000;
  ASSERT_TRUE(search.pump_once());
  EXPECT_EQ(network.refused_sends.size(), 4U);
  EXPECT_TRUE(search.stop());
}

}  // namespace
}  // namespace consoles_test
