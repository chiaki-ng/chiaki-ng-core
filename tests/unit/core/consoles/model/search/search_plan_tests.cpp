#include "tests/support/process_threading.hpp"
#include "tests/unit/core/consoles/model/search/rig.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/testing/in_memory_network.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::HomeNetwork;
using libchiaking::consoles::SearchPlan;
using libchiaking::consoles::SearchRanges;
using libchiaking::platform::IpAddress;
using Addresses = PlanRig::Addresses;
using Interfaces = PlanRig::Interfaces;
using Texts = PlanRig::Texts;

static_assert(SearchPlan::broadcast_interval == std::chrono::seconds{1});
static_assert(SearchPlan::batch_size == 64);
static_assert(SearchPlan::batch_interval == std::chrono::milliseconds{200});
static_assert(SearchPlan::cycle_interval == std::chrono::seconds{15});
static_assert(SearchPlan::reply_lifetime == std::chrono::seconds{10});
static_assert(SearchPlan::interface_limit == 128);
static_assert(SearchPlan::reply_limit == 128);

TEST(SearchPlanTargets, AreLeftOutWhenUnicastIsOff) {
  auto valid = PlanRig::plan_of(
      false, {"198.51.100.0/24", "198.51.100.17/32", "198.51.100.1/24"});
  const Interfaces interfaces{PlanRig::interface_at(
      PlanRig::ip(192, 0, 2, 23), PlanRig::ip(192, 0, 2, 255))};
  ASSERT_TRUE(valid.update_interfaces(interfaces));
  EXPECT_EQ(valid.progress().targets, 0U);
  EXPECT_TRUE(valid.progress().unicast_ranges.empty());
}

TEST(SearchPlanTargets, AreCountedOnceAcrossOverlappingRanges) {
  auto deduplicated = PlanRig::plan_of(
      true, {"198.51.100.0/24", "198.51.100.17/32", "198.51.100.1/24"});
  const Interfaces interfaces{PlanRig::interface_at(
      PlanRig::ip(192, 0, 2, 23), PlanRig::ip(192, 0, 2, 255))};
  ASSERT_TRUE(deduplicated.update_interfaces(interfaces));
  EXPECT_EQ(deduplicated.progress().targets, 254U);
  EXPECT_EQ(PlanRig::texts(deduplicated.progress().unicast_ranges),
            (Texts{"198.51.100.0/24", "198.51.100.17/32"}));
}

TEST(SearchRangesBudget, RefusesMoreThanFourWideRanges) {
  const auto over_budget =
      PlanRig::ranges(true, {"10.0.0.0/24", "10.0.1.0/24", "10.0.2.0/24",
                             "10.0.3.0/24", "10.0.4.0/24"});
  ASSERT_FALSE(over_budget);
  EXPECT_EQ(over_budget.error(), SearchRanges::Invalid::too_many_targets);
  EXPECT_TRUE(PlanRig::ranges(
      true, {"10.0.0.0/24", "10.0.1.0/24", "10.0.2.0/24", "10.0.3.0/24"}));
}

TEST(SearchRangesBudget, RefusesMoreThanThirtyTwoSubnets) {
  const std::vector<std::string> over_count(33, "198.51.100.1/32");
  const auto too_many = PlanRig::ranges(false, over_count);
  ASSERT_FALSE(too_many);
  EXPECT_EQ(too_many.error(), SearchRanges::Invalid::too_many_subnets);
  EXPECT_TRUE(
      PlanRig::ranges(false, std::vector<std::string>(32, "198.51.100.1/32")));
}

class SearchPlanBatches : public ::testing::Test {
 protected:
  SearchPlanBatches() {
    const auto changed = plan.update_interfaces(interfaces);
    EXPECT_TRUE(changed);
    if (changed) {
      EXPECT_TRUE(changed.value());
    }
  }

  void receive_host_and_send_first_batch() {
    plan.receive(host, 0);
    EXPECT_EQ(plan.next_deadline_us(0), 0U);
    ASSERT_EQ(plan.destinations(0).size(), 66U);
  }

  SearchPlan plan = PlanRig::plan_of(
      true, {"192.0.2.0/24", "198.51.100.0/25", "198.51.100.9/32"});
  const libchiaking::platform::InterfaceInfo local = PlanRig::interface_at(
      PlanRig::ip(192, 0, 2, 23), PlanRig::ip(192, 0, 2, 255));
  const Interfaces interfaces{local};
  const libchiaking::consoles::SearchReply host =
      PlanRig::reply("AABBCCDDEEFF", PlanRig::ip(203, 0, 113, 20));
};

TEST_F(SearchPlanBatches, ReportNoChangeForTheSameInterfaces) {
  const auto same = plan.update_interfaces(interfaces);
  ASSERT_TRUE(same);
  EXPECT_FALSE(same.value());
}

TEST_F(SearchPlanBatches, SuppressRangesOnALocalNetwork) {
  const auto initial = plan.progress();
  EXPECT_TRUE(initial.network_available);
  EXPECT_EQ(initial.targets, 126U);
  EXPECT_EQ(initial.visited, 0U);
  EXPECT_EQ(PlanRig::texts(initial.unicast_ranges),
            (Texts{"198.51.100.0/25", "198.51.100.9/32"}));
  ASSERT_EQ(initial.broadcast_addresses.size(), 1U);
  EXPECT_EQ(initial.broadcast_addresses.front(), PlanRig::ip(192, 0, 2, 255));
}

TEST_F(SearchPlanBatches, SendTheBroadcastThenSortedTargetsThenKnownHosts) {
  plan.receive(host, 0);
  EXPECT_EQ(plan.next_deadline_us(0), 0U);
  const auto first = plan.destinations(0);
  ASSERT_EQ(first.size(), 66U);
  EXPECT_EQ(first.front(), PlanRig::ip(192, 0, 2, 255));
  EXPECT_EQ(first[1], PlanRig::ip(198, 51, 100, 1));
  EXPECT_EQ(first[64], PlanRig::ip(198, 51, 100, 64));
  EXPECT_EQ(first.back(), host.address());
  EXPECT_EQ(plan.progress().visited, 64U);
  EXPECT_EQ(plan.next_deadline_us(0), 200'000U);
  EXPECT_TRUE(plan.destinations(199'999).empty());
}

TEST_F(SearchPlanBatches, SendTheRestInTheNextBatch) {
  ASSERT_NO_FATAL_FAILURE(receive_host_and_send_first_batch());
  const auto second = plan.destinations(200'000);
  ASSERT_EQ(second.size(), 62U);
  EXPECT_EQ(second.front(), PlanRig::ip(198, 51, 100, 65));
  EXPECT_EQ(second.back(), PlanRig::ip(198, 51, 100, 126));
  EXPECT_EQ(plan.progress().visited, 0U);
  EXPECT_EQ(plan.next_deadline_us(200'000), 1'000'000U);
}

TEST_F(SearchPlanBatches, PingTheBroadcastAndHostsUntilTheNextCycle) {
  ASSERT_NO_FATAL_FAILURE(receive_host_and_send_first_batch());
  static_cast<void>(plan.destinations(200'000));
  const auto ping = plan.destinations(1'000'000);
  ASSERT_EQ(ping.size(), 2U);
  EXPECT_EQ(ping.front(), local.broadcast);
  EXPECT_EQ(ping.back(), host.address());
  EXPECT_EQ(plan.destinations(15'199'999).size(), 1U);
  const auto next_cycle = plan.destinations(15'200'000);
  ASSERT_EQ(next_cycle.size(), 64U);
  EXPECT_EQ(next_cycle.front(), PlanRig::ip(198, 51, 100, 1));
  EXPECT_EQ(next_cycle.back(), PlanRig::ip(198, 51, 100, 64));
}

class SearchPlanReplies : public ::testing::Test {
 protected:
  SearchPlanReplies() {
    EXPECT_TRUE(plan.update_interfaces(interfaces));
    for (unsigned index = 0; index < 129; ++index)
      plan.receive(
          PlanRig::reply(
              PlanRig::hardware(129 - index),
              PlanRig::ip(203, 0, 113, static_cast<std::uint8_t>(index + 1))),
          0);
  }

  void receive_after_expiry() {
    plan.receive(PlanRig::reply("FEEDFACE0001", PlanRig::ip(203, 0, 113, 5)),
                 10'000'000);
    ASSERT_EQ(plan.replies(10'000'000).size(), 1U);
  }

  SearchPlan plan = PlanRig::plan_of(true, {"192.0.2.0/24"});
  const libchiaking::platform::InterfaceInfo local = PlanRig::interface_at(
      PlanRig::ip(198, 51, 100, 9), PlanRig::ip(198, 51, 100, 255));
  const Interfaces interfaces{local};
};

TEST_F(SearchPlanReplies, KeepTheLowestHardwareIdsUpToTheLimit) {
  const auto full = plan.replies(0);
  ASSERT_EQ(full.size(), SearchPlan::reply_limit);
  EXPECT_EQ(full.front().hardware_id().text(), "000000000002");
  EXPECT_EQ(full.back().hardware_id().text(), "000000000081");
}

TEST_F(SearchPlanReplies, ExpireTenSecondsAfterTheyArrive) {
  EXPECT_EQ(plan.replies(9'999'999).size(), SearchPlan::reply_limit);
  EXPECT_TRUE(plan.replies(10'000'000).empty());
}

TEST_F(SearchPlanReplies, AreProbedAlongsideTheSweep) {
  static_cast<void>(plan.replies(10'000'000));
  ASSERT_NO_FATAL_FAILURE(receive_after_expiry());
  EXPECT_EQ(plan.destinations(10'000'000).size(), 66U);
}

TEST_F(SearchPlanReplies, SurviveARefusedInterfaceList) {
  static_cast<void>(plan.replies(10'000'000));
  ASSERT_NO_FATAL_FAILURE(receive_after_expiry());
  const Interfaces over_limit(129, local);
  const auto rejected = plan.update_interfaces(over_limit);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), SearchPlan::Refusal::too_many_interfaces);
  EXPECT_EQ(plan.replies(10'000'000).size(), 1U);
}

TEST_F(SearchPlanReplies, AreClearedWhenTheInterfacesChange) {
  static_cast<void>(plan.replies(10'000'000));
  ASSERT_NO_FATAL_FAILURE(receive_after_expiry());
  static_cast<void>(plan.destinations(10'000'000));
  const Interfaces changed{PlanRig::interface_at(PlanRig::ip(192, 0, 2, 44),
                                                 PlanRig::ip(192, 0, 2, 255))};
  const auto reset = plan.update_interfaces(changed);
  ASSERT_TRUE(reset);
  EXPECT_TRUE(reset.value());
  EXPECT_TRUE(plan.replies(10'000'000).empty());
  const auto progress = plan.progress();
  EXPECT_EQ(progress.targets, 0U);
  EXPECT_EQ(progress.visited, 0U);
  EXPECT_TRUE(progress.unicast_ranges.empty());
  EXPECT_EQ(plan.next_deadline_us(10'000'000), 10'000'000U);
  const auto broadcast = plan.destinations(10'000'000);
  ASSERT_EQ(broadcast.size(), 1U);
  EXPECT_EQ(broadcast.front(), PlanRig::ip(192, 0, 2, 255));
}

TEST_F(SearchPlanReplies, StopTheSearchWhenNoInterfaceIsLeft) {
  const Interfaces none;
  ASSERT_TRUE(plan.update_interfaces(none));
  EXPECT_FALSE(plan.next_deadline_us(10'000'000));
  EXPECT_FALSE(plan.progress().network_available);
  EXPECT_TRUE(plan.destinations(10'000'000).empty());
}

TEST(SearchPlanExpiry, IsScheduledWithoutSpinningOnTheDeadline) {
  auto plan = PlanRig::plan_of(false, {});
  const Interfaces interfaces{PlanRig::interface_at(
      PlanRig::ip(192, 0, 2, 44), PlanRig::ip(192, 0, 2, 255))};
  ASSERT_TRUE(plan.update_interfaces(interfaces));
  plan.receive(PlanRig::reply("AABBCCDDEEFF", PlanRig::ip(203, 0, 113, 7)), 0);
  EXPECT_EQ(plan.destinations(0).size(), 2U);
  EXPECT_EQ(plan.destinations(9'500'000).size(), 2U);
  EXPECT_EQ(plan.next_deadline_us(9'500'000), 10'000'000U);
  EXPECT_TRUE(plan.replies(10'000'000).empty());
  EXPECT_EQ(plan.next_deadline_us(10'000'000), 10'500'000U);
  EXPECT_TRUE(plan.destinations(10'000'000).empty());
  EXPECT_EQ(plan.next_deadline_us(10'000'000), 10'500'000U);
}

class SearchPlanBroadcastLabels : public ::testing::Test {
 protected:
  SearchPlanBroadcastLabels() {
    zero.ipv4_prefix_length = 0;
    standard.ipv4_prefix_length = 24;
    host.ipv4_prefix_length = 32;
    interfaces = {zero, standard, host, fallback};
    EXPECT_TRUE(plan.update_interfaces(interfaces));
  }

  SearchPlan plan = PlanRig::plan_of(false, {});
  libchiaking::platform::InterfaceInfo zero = PlanRig::interface_at(
      PlanRig::ip(10, 9, 8, 7), PlanRig::ip(255, 255, 255, 255));
  libchiaking::platform::InterfaceInfo standard = PlanRig::interface_at(
      PlanRig::ip(192, 0, 2, 42), PlanRig::ip(192, 0, 2, 255));
  libchiaking::platform::InterfaceInfo host = PlanRig::interface_at(
      PlanRig::ip(203, 0, 113, 9), PlanRig::ip(203, 0, 113, 9));
  libchiaking::platform::InterfaceInfo fallback = PlanRig::interface_at(
      PlanRig::ip(198, 51, 100, 42), PlanRig::ip(198, 51, 100, 255));
  Interfaces interfaces;
};

TEST_F(SearchPlanBroadcastLabels, NameEachSubnetByItsPrefix) {
  const auto progress = plan.progress();
  EXPECT_EQ(progress.broadcast_addresses.size(), 4U);
  EXPECT_EQ(
      progress.broadcast_ranges,
      (Texts{"0.0.0.0/0", "192.0.2.0/24", "198.51.100.255", "203.0.113.9/32"}));
}

TEST_F(SearchPlanBroadcastLabels, KeepTheDestinationsSeparateAndOrdered) {
  const auto destinations = plan.destinations(0);
  ASSERT_EQ(destinations.size(), 4U);
  EXPECT_EQ(destinations.front(), standard.broadcast);
  EXPECT_EQ(destinations.back(), zero.broadcast);
}

TEST_F(SearchPlanBroadcastLabels, FollowAChangedPrefix) {
  auto changed = interfaces;
  changed[1].ipv4_prefix_length = 32;
  const auto relabelled = plan.update_interfaces(changed);
  ASSERT_TRUE(relabelled);
  EXPECT_TRUE(relabelled.value());
  ASSERT_GT(plan.progress().broadcast_ranges.size(), 1U);
  EXPECT_EQ(plan.progress().broadcast_ranges[1], "192.0.2.42/32");
}

TEST(SearchPlanInterfaces, AreFilteredAndOrdered) {
  auto plan = PlanRig::plan_of(false, {});
  auto down = PlanRig::interface_at(PlanRig::ip(10, 0, 0, 10),
                                    PlanRig::ip(10, 0, 0, 255));
  down.operational = false;
  auto loopback = PlanRig::interface_at(IpAddress::loopback_ipv4(),
                                        PlanRig::ip(127, 255, 255, 255));
  loopback.loopback = true;
  const auto unspecified = PlanRig::interface_at(
      IpAddress::any_ipv4(), PlanRig::ip(255, 255, 255, 255));
  const auto ipv6 = PlanRig::interface_at(IpAddress::loopback_ipv6(),
                                          PlanRig::ip(192, 0, 2, 255));
  auto forced_up = PlanRig::interface_at(PlanRig::ip(192, 0, 2, 10),
                                         PlanRig::ip(192, 0, 2, 255));
  forced_up.operational = false;
  forced_up.administrative_up = true;
  auto forced_down = PlanRig::interface_at(PlanRig::ip(198, 51, 100, 10),
                                           PlanRig::ip(198, 51, 100, 255));
  forced_down.administrative_up = false;
  const auto usable = PlanRig::interface_at(PlanRig::ip(203, 0, 113, 10),
                                            PlanRig::ip(203, 0, 113, 255));
  const Interfaces listed{down,   loopback,    unspecified, ipv6,
                          usable, forced_down, forced_up};
  const auto updated = plan.update_interfaces(listed);
  ASSERT_TRUE(updated);
  EXPECT_TRUE(updated.value());
  EXPECT_EQ(plan.interface_count(), 2U);
  EXPECT_EQ(
      plan.progress().broadcast_addresses,
      (Addresses{PlanRig::ip(192, 0, 2, 255), PlanRig::ip(203, 0, 113, 255)}));
  const Interfaces reordered{forced_up, usable};
  const auto same = plan.update_interfaces(reordered);
  ASSERT_TRUE(same);
  EXPECT_FALSE(same.value());
}

TEST(SearchPlanPinnedAddress, IsProbedWithEveryBroadcast) {
  auto plan = PlanRig::plan_of(false, {});
  const Interfaces interfaces{PlanRig::interface_at(
      PlanRig::ip(192, 0, 2, 44), PlanRig::ip(192, 0, 2, 255))};
  ASSERT_TRUE(plan.update_interfaces(interfaces));
  plan.keep_probing(PlanRig::ip(203, 0, 113, 9));
  plan.keep_probing(PlanRig::ip(203, 0, 113, 9));
  plan.keep_probing(IpAddress::loopback_ipv6());
  EXPECT_EQ(plan.destinations(0), (Addresses{PlanRig::ip(192, 0, 2, 255),
                                             PlanRig::ip(203, 0, 113, 9)}));
  EXPECT_TRUE(plan.destinations(999'999).empty());
  EXPECT_EQ(plan.destinations(1'000'000).size(), 2U);
}

TEST(SearchPlanWithoutBroadcast, SendsOnlyUnicast) {
  SearchPlan plan{PlanRig::ranges(true, {"198.51.100.0/30"}).value(), false};
  plan.keep_probing(PlanRig::ip(203, 0, 113, 9));
  EXPECT_EQ(plan.destinations(0), (Addresses{PlanRig::ip(203, 0, 113, 9)}));
  EXPECT_TRUE(plan.next_deadline_us(0));
  const Interfaces interfaces{PlanRig::interface_at(
      PlanRig::ip(192, 0, 2, 44), PlanRig::ip(192, 0, 2, 255))};
  ASSERT_TRUE(plan.update_interfaces(interfaces));
  EXPECT_EQ(plan.destinations(0), (Addresses{PlanRig::ip(198, 51, 100, 1),
                                             PlanRig::ip(198, 51, 100, 2),
                                             PlanRig::ip(203, 0, 113, 9)}));
  plan.receive(
      PlanRig::reply(PlanRig::hardware(1), PlanRig::ip(203, 0, 113, 20)), 0);
  EXPECT_EQ(
      plan.destinations(1'000'000),
      (Addresses{PlanRig::ip(203, 0, 113, 9), PlanRig::ip(203, 0, 113, 20)}));
  const auto progress = plan.progress();
  EXPECT_TRUE(progress.network_available);
  EXPECT_TRUE(progress.broadcast_addresses.empty());
  EXPECT_TRUE(progress.broadcast_ranges.empty());
}

TEST(SearchPlanHomeNetwork, SweepsASubnetWithItsOwnNetworkEvenWhenLocal) {
  libchiaking::platform::SteadyClock clock;
  const auto tunnel = std::make_shared<libchiaking::testing::InMemoryNetwork>(
      test_support::process_threading(), clock);
  auto valid = PlanRig::ranges(true, {"192.0.2.0/30", "192.0.2.4/30"});
  ASSERT_TRUE(valid);
  auto routed =
      valid->over(PlanRig::subnet("192.0.2.4/30"), HomeNetwork{tunnel});
  ASSERT_TRUE(routed);
  SearchPlan plan{std::move(routed.value())};
  const Interfaces interfaces{
      PlanRig::interface_at(PlanRig::ip(192, 0, 2, 1),
                            PlanRig::ip(192, 0, 2, 255)),
      PlanRig::interface_at(PlanRig::ip(192, 0, 2, 5),
                            PlanRig::ip(192, 0, 2, 255))};
  ASSERT_TRUE(plan.update_interfaces(interfaces));
  const auto progress = plan.progress();
  EXPECT_EQ(PlanRig::texts(progress.unicast_ranges), (Texts{"192.0.2.4/30"}));
  EXPECT_EQ(progress.targets, 2U);
  EXPECT_EQ(plan.destinations(0),
            (Addresses{PlanRig::ip(192, 0, 2, 5), PlanRig::ip(192, 0, 2, 6),
                       PlanRig::ip(192, 0, 2, 255)}));
}

}  // namespace
}  // namespace consoles_test
