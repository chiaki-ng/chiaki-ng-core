#include "tests/unit/core/consoles/adapters/search/network_search_rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace consoles_test {
namespace {

using libchiaking::consoles::NetworkSearch;
using libchiaking::consoles::SearchReply;
using libchiaking::platform::IpAddress;
using libchiaking::testing::FakeConsole;
using libchiaking::testing::InMemoryNetwork;

class NetworkSearchReplies : public ::testing::Test {
 protected:
  NetworkSearchReplies() {
    network.memory.interface_list = {NetworkSearchRig::interface_at(
        SearchRig::ip(192, 0, 2, 10), SearchRig::ip(192, 0, 2, 255))};
  }

  void hear_living() {
    ASSERT_TRUE(search.start());
    ASSERT_TRUE(search.pump_once());
    EXPECT_TRUE(search.replies().empty());
    const auto endpoint = search.local_endpoint();
    ASSERT_TRUE(endpoint);
    peer.send_to(endpoint.value().port, NetworkSearchRig::living);
    ASSERT_TRUE(NetworkSearchRig::pump_until_revision(search, 1));
  }

  ScriptedNetwork network;
  ScriptedClock clock;
  FakeConsole peer{network.memory};
  NetworkSearch search{network, SearchRig::threading(),
                       clock,   NetworkSearchRig::ranges(),
                       nullptr, NetworkSearchRig::on_loopback()};
};

TEST_F(NetworkSearchReplies, PublishAHeardConsole) {
  ASSERT_NO_FATAL_FAILURE(hear_living());
  ASSERT_EQ(search.replies().size(), 1U);
  EXPECT_EQ(search.replies().front().hardware_id().text(), "aabbccddeeff");
  ASSERT_TRUE(search.replies().front().name());
  EXPECT_EQ(search.replies().front().name()->text(), "Living");
  EXPECT_EQ(search.next_deadline_microseconds(), 1'000'000U);
}

TEST_F(NetworkSearchReplies, SearchTheTargetsBeforeTheBroadcastsEverySecond) {
  ASSERT_NO_FATAL_FAILURE(hear_living());
  network.sent.clear();
  clock.now = 1'000'000;
  ASSERT_TRUE(search.pump_once());
  ASSERT_EQ(network.sent.size(), 4U);
  EXPECT_EQ(network.sent[0].address, IpAddress::loopback_ipv4());
  EXPECT_EQ(network.sent[2].address, SearchRig::ip(192, 0, 2, 255));
}

TEST_F(NetworkSearchReplies, ExpireTenSecondsAfterTheyWereHeard) {
  ASSERT_NO_FATAL_FAILURE(hear_living());
  clock.now = 1'000'000;
  ASSERT_TRUE(search.pump_once());
  clock.now = 9'999'999;
  ASSERT_TRUE(search.pump_once());
  EXPECT_EQ(search.replies().size(), 1U);
  EXPECT_EQ(search.revision(), 1U);
  clock.now = 10'000'000;
  ASSERT_TRUE(search.pump_once());
  EXPECT_TRUE(search.replies().empty());
  EXPECT_EQ(search.revision(), 2U);
  EXPECT_TRUE(search.stop());
}

class NetworkSearchTracking : public ::testing::Test {
 protected:
  NetworkSearchTracking() { clock.now = 1'000'000; }

  InMemoryNetwork network{SearchRig::threading(), SearchRig::clock()};
  ScriptedClock clock;
};

TEST_F(NetworkSearchTracking, TracksAndDropsConsoles) {
  FakeConsole ps4_peer{network};
  FakeConsole ps5_peer{network};
  auto settings = NetworkSearchRig::on_loopback();
  settings.ps4_destination_port = ps4_peer.port();
  settings.ps5_destination_port = ps5_peer.port();
  network.interface_list.front().broadcast = IpAddress::loopback_ipv4();
  NetworkSearch search{network, SearchRig::threading(),
                       clock,   NetworkSearchRig::ranges(),
                       nullptr, settings};

  EXPECT_FALSE(search.pump_once());
  ASSERT_TRUE(search.start());
  EXPECT_FALSE(search.start());
  ASSERT_TRUE(search.pump_once());
  EXPECT_EQ(search.next_deadline_microseconds(), 2'000'000U);
  EXPECT_TRUE(SearchRig::contains(ps4_peer.receive(), "00020020"));
  EXPECT_TRUE(SearchRig::contains(ps5_peer.receive(), "00030010"));
  ASSERT_TRUE(search.pump_once());
  EXPECT_FALSE(ps4_peer.try_receive());

  clock.now = 2'000'000;
  ASSERT_TRUE(search.pump_once());
  EXPECT_EQ(search.next_deadline_microseconds(), 3'000'000U);
  EXPECT_TRUE(SearchRig::contains(ps4_peer.receive(), "00020020"));
  EXPECT_TRUE(SearchRig::contains(ps5_peer.receive(), "00030010"));

  const auto local = search.local_endpoint();
  ASSERT_TRUE(local);
  ps5_peer.send_to(local.value().port, NetworkSearchRig::living);
  ASSERT_TRUE(NetworkSearchRig::pump_until_revision(search, 1));
  ASSERT_EQ(search.replies().size(), 1U);
  EXPECT_EQ(search.replies().front().hardware_id().text(), "aabbccddeeff");
  EXPECT_EQ(search.replies().front().status(), SearchReply::Status::ready);

  ps5_peer.send_to(local.value().port, NetworkSearchRig::living);
  ASSERT_TRUE(NetworkSearchRig::pump_quietly(search));
  EXPECT_EQ(search.revision(), 1U);

  ps5_peer.send_to(
      local.value().port,
      "HTTP/1.1 620 Server Standby\nhost-id:AABBCCDDEEFF\nhost-type:PS5\n"
      "host-name:Living\nhost-request-port:997\n");
  ASSERT_TRUE(NetworkSearchRig::pump_until_revision(search, 2));
  ASSERT_FALSE(search.replies().empty());
  EXPECT_EQ(search.replies().front().status(), SearchReply::Status::standby);
  EXPECT_TRUE(search.replies().front().in_standby());

  ps4_peer.send_to(local.value().port,
                   "HTTP/1.1 200 Ok\nhost-type:PS4\nhost-request-port:997\n");
  ps4_peer.send_to(local.value().port,
                   "HTTP/1.1 200 Ok\nhost-id:not-a-mac-id\nhost-type:PS4\n");
  ASSERT_TRUE(NetworkSearchRig::pump_quietly(search));
  EXPECT_EQ(search.revision(), 2U);
  EXPECT_EQ(search.replies().size(), 1U);

  clock.now = 11'999'999;
  ASSERT_TRUE(search.pump_once());
  EXPECT_EQ(search.revision(), 2U);
  clock.now = 12'000'000;
  ASSERT_TRUE(search.pump_once());
  EXPECT_EQ(search.revision(), 3U);
  EXPECT_TRUE(search.replies().empty());

  EXPECT_TRUE(search.stop());
  EXPECT_TRUE(search.stop());
  EXPECT_FALSE(search.pump_once());
}

TEST_F(NetworkSearchTracking, DropsStaleConsolesInOnePass) {
  FakeConsole peer{network};
  NetworkSearch search{network, SearchRig::threading(),
                       clock,   NetworkSearchRig::ranges(),
                       nullptr, NetworkSearchRig::on_loopback()};
  ASSERT_TRUE(search.start());
  ASSERT_TRUE(search.pump_once());
  const auto local = search.local_endpoint();
  ASSERT_TRUE(local);
  peer.send_to(
      local.value().port,
      "HTTP/1.1 200 "
      "Ok\nhost-id:AAAAAAAAAAAA\nhost-type:PS5\nhost-request-port:997\n");
  ASSERT_TRUE(NetworkSearchRig::pump_until_revision(search, 1));
  peer.send_to(
      local.value().port,
      "HTTP/1.1 200 "
      "Ok\nhost-id:BBBBBBBBBBBB\nhost-type:PS5\nhost-request-port:997\n");
  ASSERT_TRUE(NetworkSearchRig::pump_until_revision(search, 2));
  peer.send_to(
      local.value().port,
      "HTTP/1.1 200 "
      "Ok\nhost-id:CCCCCCCCCCCC\nhost-type:PS5\nhost-request-port:997\n");
  ASSERT_TRUE(NetworkSearchRig::pump_until_revision(search, 3));
  EXPECT_EQ(search.replies().size(), 3U);

  clock.now += 5'000'000;
  ASSERT_TRUE(search.pump_once());
  peer.send_to(
      local.value().port,
      "HTTP/1.1 200 "
      "Ok\nhost-id:CCCCCCCCCCCC\nhost-type:PS5\nhost-request-port:997\n");
  ASSERT_TRUE(NetworkSearchRig::pump_quietly(search));
  EXPECT_EQ(search.revision(), 3U);

  clock.now += 5'000'000;
  ASSERT_TRUE(search.pump_once());
  EXPECT_EQ(search.revision(), 4U);
  ASSERT_EQ(search.replies().size(), 1U);
  EXPECT_EQ(search.replies().front().hardware_id().text(), "cccccccccccc");
  EXPECT_TRUE(search.stop());
}

}  // namespace
}  // namespace consoles_test
