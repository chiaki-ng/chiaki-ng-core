#include "tests/unit/core/consoles/adapters/search/network_search_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::Beacon;
using libchiaking::consoles::NetworkSearch;
using libchiaking::consoles::SearchRanges;
using libchiaking::consoles::Subnet;
using libchiaking::platform::IpAddress;
using libchiaking::platform::SocketEndpoint;

static_assert(NetworkSearch::interface_refresh_us == 2'000'000);
static_assert(NetworkSearch::datagrams_per_pump == 128);
static_assert(NetworkSearch::longest_wait == std::chrono::milliseconds{100});
static_assert(NetworkSearch::poll_step == std::chrono::milliseconds{100});
static_assert(NetworkSearch::maximum_poll == std::chrono::milliseconds{200});

class NetworkSearchInterfaces : public ::testing::Test {
 protected:
  NetworkSearchInterfaces() {
    auto down = NetworkSearchRig::interface_at(SearchRig::ip(10, 0, 0, 10),
                                               SearchRig::ip(10, 0, 0, 255));
    down.operational = false;
    auto loopback = NetworkSearchRig::interface_at(
        IpAddress::loopback_ipv4(), SearchRig::ip(127, 255, 255, 255));
    loopback.loopback = true;
    network.memory.interface_list = {down, loopback, first};
  }

  void start_and_pump() {
    ASSERT_TRUE(search.start());
    ASSERT_TRUE(search.pump_once());
  }

  const libchiaking::platform::InterfaceInfo first =
      NetworkSearchRig::interface_at(SearchRig::ip(192, 0, 2, 10),
                                     SearchRig::ip(192, 0, 2, 255));
  ScriptedNetwork network;
  ScriptedClock clock;
  test_support::FakeLog log;
  NetworkSearch search{
      network, SearchRig::threading(),
      clock,   NetworkSearchRig::ranges(true, {"198.51.100.0/24"}),
      &log,    NetworkSearchRig::on_loopback()};
};

TEST_F(NetworkSearchInterfaces, AreNotReadUntilTheFirstPump) {
  EXPECT_FALSE(search.progress());
  ASSERT_TRUE(search.start());
  EXPECT_TRUE(search.local_endpoint());
  EXPECT_FALSE(NetworkSearchRig::progress_of(search).network_available);
  EXPECT_EQ(network.interface_calls, 0U);
}

TEST_F(NetworkSearchInterfaces, KeepOnlyUsableOnesForTheBroadcast) {
  ASSERT_NO_FATAL_FAILURE(start_and_pump());
  const auto active = NetworkSearchRig::progress_of(search);
  EXPECT_TRUE(active.network_available);
  EXPECT_FALSE(active.permission_denied);
  EXPECT_EQ(active.targets, 254U);
  EXPECT_EQ(active.visited, 64U);
  ASSERT_EQ(active.broadcast_addresses.size(), 1U);
  EXPECT_EQ(active.broadcast_addresses.front(), first.broadcast);
  ASSERT_EQ(active.unicast_ranges.size(), 1U);
  EXPECT_EQ(active.unicast_ranges.front().text(), "198.51.100.0/24");
  EXPECT_EQ(network.interface_calls, 1U);
}

TEST_F(NetworkSearchInterfaces, SendTheBroadcastBeforeTheFirstUnicastBatch) {
  ASSERT_NO_FATAL_FAILURE(start_and_pump());
  ASSERT_EQ(network.sent.size(), 130U);
  EXPECT_EQ(network.sent[0],
            (SocketEndpoint{first.broadcast, Beacon::ps4_port}));
  EXPECT_EQ(network.sent[1],
            (SocketEndpoint{first.broadcast, Beacon::ps5_port}));
  EXPECT_EQ(network.sent[2],
            (SocketEndpoint{SearchRig::ip(198, 51, 100, 1), Beacon::ps4_port}));
  EXPECT_EQ(network.sent[129], (SocketEndpoint{SearchRig::ip(198, 51, 100, 64),
                                               Beacon::ps5_port}));
  ASSERT_EQ(log.messages.size(), 1U);
  EXPECT_TRUE(log.messages.front().starts_with(
      "Scanning 1 IPv4 interfaces; 254 unicast targets; PS4 UDP 987, PS5 UDP "
      "9302"))
      << log.messages.front();
}

TEST_F(NetworkSearchInterfaces, SendTheNextBatchOnlyWhenItIsDue) {
  ASSERT_NO_FATAL_FAILURE(start_and_pump());
  ASSERT_TRUE(search.pump_once());
  EXPECT_EQ(network.interface_calls, 1U);
  EXPECT_EQ(network.sent.size(), 130U);
  clock.now = 200'000;
  ASSERT_TRUE(search.pump_once());
  EXPECT_EQ(NetworkSearchRig::progress_of(search).visited, 128U);
  EXPECT_EQ(network.sent.size(), 258U);
}

TEST_F(NetworkSearchInterfaces, AreRefreshedAndSuppressALocalRange) {
  ASSERT_NO_FATAL_FAILURE(start_and_pump());
  clock.now = 2'000'000;
  network.memory.interface_list = {NetworkSearchRig::interface_at(
      SearchRig::ip(198, 51, 100, 10), SearchRig::ip(198, 51, 100, 255))};
  ASSERT_TRUE(search.pump_once());
  const auto suppressed = NetworkSearchRig::progress_of(search);
  EXPECT_EQ(suppressed.targets, 0U);
  EXPECT_EQ(suppressed.visited, 0U);
  EXPECT_TRUE(suppressed.unicast_ranges.empty());
  EXPECT_EQ(network.interface_calls, 2U);
  EXPECT_EQ(log.messages.size(), 1U);
}

TEST_F(NetworkSearchInterfaces, ThatCannotBeListedFailThePump) {
  ASSERT_NO_FATAL_FAILURE(start_and_pump());
  clock.now = 4'000'000;
  network.interfaces_fail = true;
  const auto failed = search.pump_once();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), NetworkSearch::Failure::network);
  EXPECT_TRUE(search.stop());
  EXPECT_FALSE(search.progress());
  EXPECT_EQ(search.revision(), 0U);
}

TEST(NetworkSearchRoutedInterface, IsSearchedWithoutABroadcast) {
  ScriptedNetwork network;
  ScriptedClock clock;
  auto routed = NetworkSearchRig::interface_at(SearchRig::ip(10, 1, 0, 10),
                                               IpAddress::any_ipv4());
  routed.lan_adapter = false;
  routed.has_broadcast = false;
  network.memory.interface_list = {routed};
  NetworkSearch search(network, SearchRig::threading(), clock,
                       NetworkSearchRig::ranges(true, {"198.51.100.0/31"}),
                       nullptr, NetworkSearchRig::on_loopback());
  ASSERT_TRUE(search.start());
  ASSERT_TRUE(search.pump_once());
  const auto state = NetworkSearchRig::progress_of(search);
  EXPECT_TRUE(state.network_available);
  EXPECT_TRUE(state.broadcast_addresses.empty());
  EXPECT_EQ(state.targets, 2U);
  EXPECT_EQ(state.visited, 0U);
  EXPECT_EQ(network.sent.size(), 4U);
  EXPECT_TRUE(search.stop());
}

TEST(NetworkSearchInterfaceState,
     PrefersTheAdministrativeStateAndFallsBackToTheOperationalOne) {
  ScriptedNetwork network;
  ScriptedClock clock;
  auto administratively_up = NetworkSearchRig::interface_at(
      SearchRig::ip(192, 0, 2, 10), SearchRig::ip(192, 0, 2, 255));
  administratively_up.administrative_up = true;
  administratively_up.operational = false;
  administratively_up.ipv4_prefix_length = 24;
  auto administratively_down = NetworkSearchRig::interface_at(
      SearchRig::ip(198, 51, 100, 10), SearchRig::ip(198, 51, 100, 255));
  administratively_down.administrative_up = false;
  const auto fallback = NetworkSearchRig::interface_at(
      SearchRig::ip(203, 0, 113, 10), SearchRig::ip(203, 0, 113, 255));
  network.memory.interface_list = {administratively_up, administratively_down,
                                   fallback};
  NetworkSearch search(network, SearchRig::threading(), clock,
                       NetworkSearchRig::ranges(), nullptr,
                       NetworkSearchRig::on_loopback());
  ASSERT_TRUE(search.start());
  ASSERT_TRUE(search.pump_once());
  const auto progress = NetworkSearchRig::progress_of(search);
  EXPECT_EQ(progress.broadcast_addresses.size(), 2U);
  EXPECT_EQ(progress.broadcast_ranges,
            (std::vector<std::string>{"192.0.2.0/24", "203.0.113.255"}));
  EXPECT_EQ(network.sent.size(), 4U);
  EXPECT_TRUE(
      std::ranges::none_of(network.sent, [](const SocketEndpoint& endpoint) {
        return endpoint.address == SearchRig::ip(203, 0, 113, 77);
      }));
  EXPECT_TRUE(search.stop());
}

TEST(NetworkSearchInterfaceLimit, RefusesTooManyInterfaces) {
  ScriptedNetwork network;
  ScriptedClock clock;
  network.memory.interface_list = {NetworkSearchRig::interface_at(
      SearchRig::ip(192, 0, 2, 10), SearchRig::ip(192, 0, 2, 255))};
  NetworkSearch search(network, SearchRig::threading(), clock,
                       NetworkSearchRig::ranges(), nullptr,
                       NetworkSearchRig::on_loopback());
  ASSERT_TRUE(search.start());
  ASSERT_TRUE(search.pump_once());
  network.memory.interface_list.assign(
      129, NetworkSearchRig::interface_at(SearchRig::ip(198, 51, 100, 10),
                                          SearchRig::ip(198, 51, 100, 255)));
  clock.now = 12'000'000;
  const auto rejected = search.pump_once();
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), NetworkSearch::Failure::invalid_argument);
  EXPECT_TRUE(search.stop());
}

TEST(NetworkSearchRanges, RefuseASubnetWiderThanTwentyFourBits) {
  EXPECT_FALSE(Subnet::parse("192.0.2.0/23"));
}

TEST(NetworkSearchRanges, RefuseRangesOverTheTargetBudget) {
  const std::vector<std::string> over_budget{"10.0.0.0/24", "10.0.1.0/24",
                                             "10.0.2.0/24", "10.0.3.0/24",
                                             "10.0.4.0/24"};
  const auto refused_ranges = SearchRanges::parse(over_budget, true);
  ASSERT_FALSE(refused_ranges);
  EXPECT_EQ(refused_ranges.error(), SearchRanges::Invalid::too_many_targets);
}

TEST(NetworkSearchBind, RefusesTheWrongFamilyBeforeOpeningASocket) {
  ScriptedNetwork network;
  ScriptedClock clock;
  auto ipv6 = NetworkSearchRig::on_loopback();
  ipv6.bind_address = IpAddress::loopback_ipv6();
  NetworkSearch wrong_family(network, SearchRig::threading(), clock,
                             NetworkSearchRig::ranges(), nullptr, ipv6);
  const auto refused = wrong_family.start();
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), NetworkSearch::Failure::invalid_argument);
  EXPECT_FALSE(wrong_family.local_endpoint());
  EXPECT_FALSE(wrong_family.progress());
  EXPECT_EQ(network.sockets_created, 0U);
  EXPECT_EQ(network.interface_calls, 0U);
}

}  // namespace
}  // namespace consoles_test
