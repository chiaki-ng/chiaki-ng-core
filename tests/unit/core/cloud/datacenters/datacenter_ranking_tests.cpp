#include "src/core/play/adapters/datacenters/datacenter_ranking.hpp"
#include "src/core/psn/gk/measurement.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace cloud_test {
namespace {

namespace gk = libchiaking::psn::gk;
using libchiaking::play::DatacenterRanking;

class GkForcedDatacenter : public ::testing::Test {
 protected:
  const std::vector<gk::Datacenter> rows{
      {"dc-a", "198.51.100.1", 1111, 20000},
      {"dc-a", "198.51.100.2", 2222, 30000},
      {"dc-b", "198.51.100.3", 0, 40000},
  };
};

TEST_F(GkForcedDatacenter, PingsTheFirstRowOfTheForcedName) {
  const DatacenterRanking first{rows, "dc-a"};
  const auto ping = first.forced_ping();
  EXPECT_TRUE(first.forced());
  ASSERT_TRUE(ping);
  EXPECT_EQ(ping->name, "dc-a");
  EXPECT_EQ(ping->public_ip, "198.51.100.1");
  EXPECT_EQ(ping->port, 1111);
  EXPECT_EQ(ping->selected_port, 1111);
  EXPECT_EQ(ping->max_bandwidth, 20000);
  EXPECT_EQ(ping->rtt_ms, 40);
  EXPECT_EQ(ping->rtts_ms, std::vector<int>{40});
  EXPECT_EQ(ping->mtu_in, 1454);
  EXPECT_EQ(ping->mtu_out, 1254);
  EXPECT_TRUE(ping->measured);
}

TEST_F(GkForcedDatacenter, SelectsTheDefaultPortForARowWithoutOne) {
  const DatacenterRanking defaulted{rows, "dc-b"};
  const auto ping = defaulted.forced_ping();
  EXPECT_TRUE(defaulted.forced());
  ASSERT_TRUE(ping);
  EXPECT_EQ(ping->port, 0);
  EXPECT_EQ(ping->selected_port, 2053);
}

TEST_F(GkForcedDatacenter, HasNoPingForAnUnlistedName) {
  const DatacenterRanking missing{rows, "missing"};
  EXPECT_TRUE(missing.forced());
  EXPECT_FALSE(missing.forced_ping());
}

TEST_F(GkForcedDatacenter, IsNotForcedByAutoOrNothing) {
  const DatacenterRanking automatic{rows, "Auto"};
  EXPECT_FALSE(automatic.forced());
  EXPECT_FALSE(automatic.forced_ping());
  const DatacenterRanking unset{rows, ""};
  EXPECT_FALSE(unset.forced());
  EXPECT_FALSE(unset.forced_ping());
}

TEST_F(GkForcedDatacenter, TreatsLowercaseAutoAsAName) {
  const DatacenterRanking lowercase{rows, "auto"};
  EXPECT_TRUE(lowercase.forced());
  EXPECT_FALSE(lowercase.forced_ping());
}

class GkAutoDatacenter : public ::testing::Test {
 protected:
  static auto ping(const char* name, int rtt, bool measured, int port = 0)
      -> gk::Ping {
    gk::Ping row;
    row.name = name;
    row.rtt_ms = rtt;
    row.rtts_ms = {rtt};
    row.measured = measured;
    row.port = port;
    return row;
  }
};

TEST_F(GkAutoDatacenter, HasNoFastestWithoutRows) {
  const auto result = DatacenterRanking::rank({});
  EXPECT_FALSE(result.fastest);
  EXPECT_TRUE(result.sorted.empty());
}

TEST_F(GkAutoDatacenter, PicksAndSortsByRoundTrip) {
  const auto result = DatacenterRanking::rank(
      {ping("b", 60, true, 9000), ping("a", 30, true, 2055)});
  ASSERT_TRUE(result.fastest);
  EXPECT_EQ(result.fastest->name, "a");
  EXPECT_EQ(result.fastest->selected_port, 2055);
  ASSERT_EQ(result.sorted.size(), std::size_t{2});
  EXPECT_EQ(result.sorted[0].name, "a");
  EXPECT_EQ(result.sorted[1].name, "b");
}

TEST_F(GkAutoDatacenter, SelectsTheDefaultPortForARowWithoutOne) {
  const auto result = DatacenterRanking::rank({ping("a", 30, true)});
  ASSERT_TRUE(result.fastest);
  EXPECT_EQ(result.fastest->port, 0);
  EXPECT_EQ(result.fastest->selected_port, 2053);
}

TEST_F(GkAutoDatacenter, AcceptsAtMostAHundredMilliseconds) {
  EXPECT_TRUE(DatacenterRanking::rank({ping("a", 100, true)}).fastest);
  const auto result = DatacenterRanking::rank({ping("a", 101, true)});
  EXPECT_FALSE(result.fastest);
  EXPECT_EQ(result.sorted.size(), std::size_t{1});
}

TEST_F(GkAutoDatacenter, PicksNothingUnmeasured) {
  const auto result =
      DatacenterRanking::rank({ping("a", 10, false), ping("b", 999, false)});
  EXPECT_FALSE(result.fastest);
  EXPECT_EQ(result.sorted.size(), std::size_t{2});
}

TEST_F(GkAutoDatacenter, PicksNothingWhenTheFastestFailedToMeasure) {
  const auto result =
      DatacenterRanking::rank({ping("bad", 0, false), ping("good", 50, true)});
  EXPECT_FALSE(result.fastest);
  ASSERT_FALSE(result.sorted.empty());
  EXPECT_EQ(result.sorted.front().name, "bad");
}

TEST_F(GkAutoDatacenter, SkipsASlowUnmeasuredRow) {
  const auto result = DatacenterRanking::rank(
      {ping("unmeasured", 999, false), ping("ok", 50, true, 7000)});
  ASSERT_TRUE(result.fastest);
  EXPECT_EQ(result.fastest->name, "ok");
  EXPECT_EQ(result.fastest->selected_port, 7000);
}

TEST_F(GkAutoDatacenter, KeepsTheFirstOfATie) {
  const auto result =
      DatacenterRanking::rank({ping("x", 40, true), ping("y", 40, true)});
  ASSERT_TRUE(result.fastest);
  EXPECT_EQ(result.fastest->name, "x");
}

class GkPickerMerge : public ::testing::Test {
 protected:
  static auto ping(const char* name, int rtt, bool measured = true)
      -> gk::Ping {
    gk::Ping row;
    row.name = name;
    row.rtt_ms = rtt;
    row.rtts_ms = {rtt};
    row.mtu_in = 1454;
    row.mtu_out = 1254;
    row.measured = measured;
    return row;
  }

  const std::vector<gk::Datacenter> api{
      {"a", "198.51.100.1", 2053, 1000},
      {"b", "198.51.100.2", 2054, 2000},
      {"c", "198.51.100.3", 2055, 3000},
  };
  const std::vector<gk::Ping> current{ping("b", 20), ping("a", 40),
                                      ping("a", 41)};
  const std::vector<gk::Ping> prior{ping("a", 10), ping("b", 30),
                                    ping("old", 60), ping("old", 61),
                                    ping("", 70)};
  const DatacenterRanking forced{api, "a"};
};

TEST_F(GkPickerMerge, ListsTheApiRowsWithTheirLatestRoundTrip) {
  const auto out = DatacenterRanking{api, "Auto"}.picker_rows(current, prior);
  ASSERT_EQ(out.size(), std::size_t{4});
  EXPECT_EQ(out[0].name, "a");
  EXPECT_EQ(out[0].rtt_ms, 40);
  EXPECT_EQ(out[1].name, "b");
  EXPECT_EQ(out[1].rtt_ms, 20);
  EXPECT_EQ(out[2].name, "c");
  EXPECT_EQ(out[2].rtt_ms, 0);
  EXPECT_EQ(out[2].rtts_ms, std::vector<int>{0});
  EXPECT_EQ(out[2].mtu_in, 0);
  EXPECT_EQ(out[2].mtu_out, 0);
  EXPECT_EQ(out[2].port, 2055);
  EXPECT_EQ(out[2].public_ip, "198.51.100.3");
  EXPECT_EQ(out[2].max_bandwidth, 3000);
  EXPECT_FALSE(out[2].measured);
  EXPECT_EQ(out[3].name, "old");
  EXPECT_EQ(out[3].rtt_ms, 60);
}

TEST_F(GkPickerMerge, PrefersThePriorRoundTripOfAForcedRow) {
  const auto out = forced.picker_rows(current, prior);
  ASSERT_GE(out.size(), std::size_t{2});
  EXPECT_EQ(out[0].rtt_ms, 10);
  EXPECT_EQ(out[1].rtt_ms, 20);
}

TEST_F(GkPickerMerge, TakesTheCurrentRoundTripOfAForcedRowWithoutPrior) {
  const auto out = forced.picker_rows(current, {});
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(out[0].rtt_ms, 40);
  EXPECT_TRUE(out[0].measured);
}

TEST_F(GkPickerMerge, TakesThePriorRoundTripOfAForcedRowWithoutCurrent) {
  const auto out = forced.picker_rows({}, prior);
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(out[0].rtt_ms, 10);
}

TEST_F(GkPickerMerge, LeavesAForcedRowUnmeasuredWithoutAnyRoundTrip) {
  const auto out = forced.picker_rows({}, {});
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(out[0].rtt_ms, 0);
  EXPECT_FALSE(out[0].measured);
  EXPECT_EQ(out[0].port, 2053);
}

class GkDatacenterChoice : public ::testing::Test {
 protected:
  static auto dc(std::string name, int port = 2053) -> gk::Datacenter {
    return {std::move(name), "127.0.0.1", port, 20'000};
  }

  static auto ping(std::string name, int rtt, bool measured = true)
      -> gk::Ping {
    gk::Ping row;
    row.name = std::move(name);
    row.rtt_ms = rtt;
    row.rtts_ms = {rtt};
    row.port = 0;
    row.measured = measured;
    return row;
  }

  const std::vector<gk::Datacenter> api{dc("a"), dc("b", 0)};
};

TEST_F(GkDatacenterChoice, SelectsAForcedRowWithTheAssumedRoundTrip) {
  const auto forced = DatacenterRanking{api, "b"}.choose(
      {ping("b", 900)}, {ping("b", 33), ping("old", 44)});
  ASSERT_TRUE(forced);
  ASSERT_TRUE(forced->selected);
  EXPECT_EQ(forced->selected->name, "b");
  EXPECT_EQ(forced->selected->rtt_ms, 40);
  EXPECT_EQ(forced->selected->selected_port, 2053);
  ASSERT_EQ(forced->ping_results.size(), std::size_t{1});
  EXPECT_EQ(forced->ping_results[0].rtt_ms, 40);
  ASSERT_EQ(forced->picker_rows.size(), std::size_t{3});
  EXPECT_EQ(forced->picker_rows[1].rtt_ms, 33);
}

TEST_F(GkDatacenterChoice, SelectsAForcedRowWithoutAnyMeasurement) {
  const auto forced = DatacenterRanking{api, "b"}.choose({}, {});
  ASSERT_TRUE(forced);
  ASSERT_EQ(forced->picker_rows.size(), std::size_t{2});
  EXPECT_EQ(forced->picker_rows[1].rtt_ms, 40);
  ASSERT_FALSE(forced->ping_results.empty());
  EXPECT_EQ(forced->ping_results[0].port, 0);
  EXPECT_EQ(forced->ping_results[0].selected_port, 2053);
}

TEST_F(GkDatacenterChoice, HasNoChoiceForAnUnlistedForcedName) {
  const DatacenterRanking missing{api, "missing"};
  EXPECT_FALSE(missing.choose({}, {}));
}

TEST_F(GkDatacenterChoice, SelectsTheFastestAutomatically) {
  const auto automatic =
      DatacenterRanking{api, "Auto"}.choose({ping("a", 55), ping("b", 30)}, {});
  ASSERT_TRUE(automatic);
  ASSERT_TRUE(automatic->selected);
  EXPECT_EQ(automatic->selected->name, "b");
  EXPECT_EQ(automatic->selected->selected_port, 2053);
  ASSERT_EQ(automatic->ping_results.size(), std::size_t{2});
  EXPECT_EQ(automatic->ping_results.front().name, "b");
}

TEST_F(GkDatacenterChoice, SelectsNothingTooSlow) {
  const auto too_slow =
      DatacenterRanking{api, ""}.choose({ping("a", 115), ping("b", 130)}, {});
  ASSERT_TRUE(too_slow);
  EXPECT_FALSE(too_slow->selected);
  ASSERT_EQ(too_slow->ping_results.size(), std::size_t{2});
  EXPECT_EQ(too_slow->ping_results.front().rtt_ms, 115);
}

TEST_F(GkDatacenterChoice, SelectsNothingUnmeasured) {
  const auto unmeasured = DatacenterRanking{api, ""}.choose(
      {ping("a", 999, false), ping("b", 999, false)}, {});
  ASSERT_TRUE(unmeasured);
  EXPECT_FALSE(unmeasured->selected);
  EXPECT_EQ(unmeasured->picker_rows.size(), std::size_t{2});
}

}  // namespace
}  // namespace cloud_test
