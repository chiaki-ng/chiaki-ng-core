#include "tests/unit/api/consoles/home_rig.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace api_consoles_test {
namespace {

using libchiaking::consoles::Console;
using libchiaking::consoles::ConsoleName;
using libchiaking::consoles::Firmware;
using libchiaking::consoles::Generation;
using libchiaking::consoles::NetworkView;
using libchiaking::consoles::Presence;
using libchiaking::consoles::SearchFailure;
using libchiaking::consoles::SearchRanges;
using libchiaking::platform::IpAddress;
using libchiaking::platform::StopSource;
using libchiaking::testing::FakeConsole;

class HomeSearch : public HomeConsolesTest {
 protected:
  static auto refusal(const std::vector<std::string>& subnets)
      -> std::optional<SearchRanges::Invalid> {
    const auto valid = SearchRanges::parse(subnets, true);
    if (valid) return std::nullopt;
    return valid.error();
  }
};

TEST_F(HomeSearch, RefusesRangesItCannotSearch) {
  EXPECT_EQ(refusal({"10.0.0.0/8"}), SearchRanges::Invalid::subnet);
  EXPECT_EQ(refusal({"192.168.1.0"}), SearchRanges::Invalid::subnet);
  EXPECT_EQ(refusal({"127.0.0.0/24"}), SearchRanges::Invalid::subnet);
  EXPECT_EQ(refusal(std::vector<std::string>(33, "192.168.1.0/30")),
            SearchRanges::Invalid::too_many_subnets);
  EXPECT_EQ(refusal({"10.0.0.0/24", "10.0.1.0/24", "10.0.2.0/24", "10.0.3.0/24",
                     "10.0.4.0/24"}),
            SearchRanges::Invalid::too_many_targets);
}

TEST_F(HomeSearch, ReportsViewsUntilStopped) {
  FakeConsole console{home.network, {.name = "Loopback fixture"}};
  console.rest();
  EXPECT_EQ(console.announcement(), ps5_reply);
  StopSource stop;
  Views views;
  std::shared_ptr<Console> found;
  views.on_seen = [&](const NetworkView& view) {
    if (views.seen.size() == 1) {
      console.announce();
      console.send_to(FakeConsole::first_searcher_port, anonymous_reply);
    } else if (views.seen.size() == 2) {
      ASSERT_EQ(view.nearby.size(), 1U);
      found = view.nearby.front();
      EXPECT_EQ(found->presence(), Presence::rest_mode);
      home.clock.now.store(10'000'000);
    } else {
      ASSERT_NE(found, nullptr);
      EXPECT_EQ(found->presence(), Presence::unknown);
      static_cast<void>(stop.request_stop());
    }
  };
  const auto searched = home.known.find_on_my_network(
      ranges({"198.51.100.0/30"}, true), views, stop.token());
  EXPECT_TRUE(searched);
  ASSERT_EQ(views.seen.size(), 3U);

  const auto& idle = views.seen.front();
  EXPECT_TRUE(idle.nearby.empty());
  EXPECT_TRUE(idle.network_available);
  EXPECT_FALSE(idle.permission_denied);
  EXPECT_EQ(idle.targets, 2U);
  EXPECT_EQ(idle.visited, 0U);
  EXPECT_EQ(idle.broadcast_ranges, std::vector<std::string>{"192.0.2.0/24"});
  ASSERT_EQ(idle.unicast_ranges.size(), 1U);
  EXPECT_EQ(idle.unicast_ranges.front().text(), "198.51.100.0/30");
  EXPECT_TRUE(sent_to(home.network, IpAddress::ipv4({192, 0, 2, 255}), 987));
  EXPECT_TRUE(sent_to(home.network, IpAddress::ipv4({192, 0, 2, 255}), 9302));
  EXPECT_TRUE(sent_to(home.network, IpAddress::ipv4({198, 51, 100, 1}), 9302));
  EXPECT_TRUE(sent_to(home.network, IpAddress::ipv4({198, 51, 100, 2}), 987));

  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->hardware_id().text(), "aabbccddeeff");
  EXPECT_EQ(found, home.known.find(hardware_id(living_room)));
  EXPECT_EQ(home.known.everything().size(), 1U);
  EXPECT_EQ(found->generation(), Generation::ps5);
  EXPECT_EQ(found->name(), "Loopback fixture");
  EXPECT_EQ(found->name_source(), ConsoleName::Source::discovered);
  EXPECT_EQ(found->firmware(), Firmware{Firmware::Family::ps5_1});
  ASSERT_TRUE(found->last_address());
  EXPECT_EQ(found->last_address()->text(), "127.0.0.1");
  EXPECT_FALSE(found->is_saved());
  EXPECT_FALSE(found->is_paired());
  EXPECT_TRUE(views.seen.back().nearby.empty());
  EXPECT_TRUE(views.seen.back().network_available);
  EXPECT_EQ(found->presence(), Presence::unknown);

  EXPECT_TRUE(home.known.find_on_my_network(ranges({"198.51.100.0/30"}, true),
                                            views, stop.token()));
  EXPECT_EQ(views.seen.size(), 3U);
  EXPECT_EQ(home.http.requests, 0);
}

TEST_F(HomeSearch, AdmitsRepliesStrictly) {
  const std::string duplicated = std::string{ps4_reply} + "host-name:Second\n";
  const std::string oversized =
      std::string{ps4_reply} + "x-padding:" + std::string(4200, 'p') + "\n";
  FakeConsole console{home.network};
  StopSource stop;
  Views views;
  views.on_seen = [&](const NetworkView& view) {
    if (views.seen.size() == 1) {
      console.send_to(FakeConsole::first_searcher_port, duplicated);
      console.send_to(FakeConsole::first_searcher_port, oversized);
      console.send_to(FakeConsole::first_searcher_port, ps5_reply);
      return;
    }
    static_cast<void>(stop.request_stop());
    ASSERT_EQ(view.nearby.size(), 1U);
    EXPECT_EQ(view.nearby.front()->hardware_id(), hardware_id(living_room));
  };
  EXPECT_TRUE(home.known.find_on_my_network({}, views, stop.token()));
  EXPECT_EQ(views.seen.size(), 2U);
  EXPECT_EQ(home.known.everything().size(), 1U);
}

TEST_F(HomeSearch, DecidesPresenceFromWhatItSees) {
  const auto absent = home.saved(den, Generation::ps4);
  EXPECT_EQ(absent->presence(), Presence::unknown);
  FakeConsole console{home.network, {.name = "Living room"}};
  StopSource stop;
  Views views;
  std::vector<std::pair<Presence, Presence>> observed;
  views.on_seen = [&](const NetworkView& view) {
    const auto present = home.known.find(hardware_id(living_room));
    observed.emplace_back(absent->presence(),
                          present ? present->presence() : Presence::unknown);
    switch (views.seen.size()) {
      case 1:
        EXPECT_TRUE(view.nearby.empty());
        console.announce();
        break;
      case 2:
        EXPECT_EQ(view.nearby.size(), 1U);
        if (!view.nearby.empty()) {
          EXPECT_EQ(view.nearby.front(), present);
        }
        console.rest();
        console.announce();
        break;
      case 3:
        home.clock.now.store(10'000'000);
        break;
      default:
        static_cast<void>(stop.request_stop());
    }
  };
  EXPECT_TRUE(home.known.find_on_my_network({}, views, stop.token()));
  EXPECT_EQ(observed, (std::vector<std::pair<Presence, Presence>>{
                          {Presence::unknown, Presence::unknown},
                          {Presence::not_nearby, Presence::ready},
                          {Presence::not_nearby, Presence::rest_mode},
                          {Presence::unknown, Presence::unknown}}));
  const auto present = home.known.find(hardware_id(living_room));
  ASSERT_NE(present, nullptr);
  EXPECT_EQ(present->presence(), Presence::unknown);
  EXPECT_EQ(absent->presence(), Presence::unknown);
}

TEST_F(HomeSearch, RunsOneAtATime) {
  FakeConsole console{home.network};
  const auto wide = ranges({"198.51.100.0/30"}, true);
  StopSource first_stop;
  Views first;
  std::atomic<bool> running{false};
  std::atomic<bool> first_found{false};
  first.on_seen = [&](const NetworkView& view) {
    running = true;
    if (!view.nearby.empty()) first_found = true;
  };
  libchiaking::Expected<void, SearchFailure> first_result{};
  auto searcher = libchiaking::platform::Thread::start(
                      home.threading, "searcher",
                      libchiaking::platform::ThreadRole::discovery,
                      [&] {
                        first_result = home.known.find_on_my_network(
                            wide, first, first_stop.token());
                      })
                      .value();
  wait_for(running);

  Views refused;
  StopSource unused;
  const auto busy = home.known.find_on_my_network({}, refused, unused.token());
  EXPECT_FALSE(busy);
  if (!busy) {
    EXPECT_EQ(busy.error(), SearchFailure::busy);
  }
  EXPECT_TRUE(refused.seen.empty());

  StopSource second_stop;
  Views second;
  second.on_seen = [&](const NetworkView& view) {
    if (view.nearby.empty())
      console.announce();
    else
      static_cast<void>(second_stop.request_stop());
  };
  const auto attached =
      home.known.find_on_my_network(wide, second, second_stop.token());
  EXPECT_TRUE(attached);
  EXPECT_GE(second.seen.size(), 2U);
  if (!second.seen.empty()) {
    EXPECT_EQ(second.seen.front().targets, 2U);
    EXPECT_EQ(second.seen.back().nearby.size(), 1U);
  }

  EXPECT_GE(home.network.sent().size(), 6U);
  const auto still_busy =
      home.known.find_on_my_network({}, refused, unused.token());
  EXPECT_FALSE(still_busy);
  if (!still_busy) {
    EXPECT_EQ(still_busy.error(), SearchFailure::busy);
  }
  wait_for(first_found);
  static_cast<void>(first_stop.request_stop());
  ASSERT_TRUE(searcher.join());
  EXPECT_TRUE(first_result);
  ASSERT_FALSE(first.seen.empty());
  ASSERT_EQ(first.seen.back().nearby.size(), 1U);
  ASSERT_FALSE(second.seen.empty());
  ASSERT_FALSE(second.seen.back().nearby.empty());
  EXPECT_EQ(first.seen.back().nearby.front(),
            second.seen.back().nearby.front());

  StopSource third_stop;
  Views third;
  third.on_seen = [&](const NetworkView& view) {
    EXPECT_EQ(view.targets, 0U);
    static_cast<void>(third_stop.request_stop());
  };
  EXPECT_TRUE(home.known.find_on_my_network({}, third, third_stop.token()));
  EXPECT_EQ(third.seen.size(), 1U);
}

TEST_F(HomeSearch, EndsWhenProbingIsNotPermitted) {
  home.network.fail_sends_with(
      libchiaking::platform::NetError::permission_denied);
  StopSource stop;
  Views views;
  const auto denied = home.known.find_on_my_network({}, views, stop.token());
  ASSERT_FALSE(denied);
  EXPECT_EQ(denied.error(), SearchFailure::permission_denied);
  ASSERT_EQ(views.seen.size(), 1U);
  EXPECT_TRUE(views.seen.front().permission_denied);
}

TEST_F(HomeSearch, SearchesASubnetOverTheNetworkItIsGiven) {
  const auto tunnel = std::make_shared<Tunnel>(false);
  const auto behind = IpAddress::ipv4({198, 51, 100, 7});
  libchiaking::platform::InterfaceInfo far_end;
  far_end.name = "tun0";
  far_end.index = 2;
  far_end.address = behind;
  far_end.operational = true;
  far_end.ipv4_prefix_length = 32;
  tunnel->memory.interface_list.push_back(far_end);
  FakeConsole console{tunnel->memory, {behind, FakeConsole::ps5_search_port}};
  const auto single = libchiaking::consoles::Subnet::parse("198.51.100.7/32");
  const auto other = libchiaking::consoles::Subnet::parse("203.0.113.0/30");
  ASSERT_TRUE(single);
  ASSERT_TRUE(other);
  const auto plain = ranges({"198.51.100.7/32", "203.0.113.0/30"}, true);
  const auto routed =
      plain.over(*single, libchiaking::consoles::HomeNetwork{tunnel});
  ASSERT_TRUE(routed);
  EXPECT_EQ(routed->network_of(*single).network(), tunnel);
  EXPECT_FALSE(routed->network_of(*other).network());
  EXPECT_FALSE(plain.network_of(*single).network());
  EXPECT_FALSE(routed.value() == plain);
  const auto stray =
      plain.over(*libchiaking::consoles::Subnet::parse("192.0.2.0/24"),
                 libchiaking::consoles::HomeNetwork{tunnel});
  ASSERT_FALSE(stray);
  EXPECT_EQ(stray.error(), SearchRanges::Invalid::subnet);
  EXPECT_EQ(routed->over(*single, {}).value(), plain);
  EXPECT_EQ(tunnel.use_count(), 2);

  StopSource stop;
  Views views;
  int reads = 0;
  home.clock.on_read = [&] {
    console.serve();
    if (++reads > 400) static_cast<void>(stop.request_stop());
  };
  views.on_seen = [&](const NetworkView& view) {
    if (!view.nearby.empty()) static_cast<void>(stop.request_stop());
  };
  EXPECT_TRUE(
      home.known.find_on_my_network(routed.value(), views, stop.token()));
  home.clock.on_read = nullptr;
  EXPECT_EQ(tunnel.use_count(), 2);

  ASSERT_FALSE(views.seen.empty());
  const auto& seen = views.seen.back();
  ASSERT_EQ(seen.nearby.size(), 1U);
  const auto found = seen.nearby.front();
  EXPECT_EQ(found->name(), "Fake console");
  EXPECT_FALSE(found->is_saved());
  ASSERT_TRUE(found->last_address());
  EXPECT_EQ(found->last_address()->text(), "198.51.100.7");
  EXPECT_EQ(seen.unicast_ranges.size(), 2U);
  EXPECT_GT(console.searches(), 0U);
  EXPECT_EQ(tunnel->destinations(), std::vector<IpAddress>{behind});
  EXPECT_TRUE(sent_to(home.network, IpAddress::ipv4({192, 0, 2, 255})));
  EXPECT_TRUE(sent_to(home.network, IpAddress::ipv4({203, 0, 113, 1})));
  EXPECT_TRUE(sent_to(home.network, IpAddress::ipv4({203, 0, 113, 2})));
  EXPECT_FALSE(sent_to(home.network, behind));
}

}  // namespace
}  // namespace api_consoles_test
