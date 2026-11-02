#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/model/datacenter_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using play::DatacenterMeasurement;
using std::chrono::milliseconds;

class KnownDatacenters : public DatacenterRig {
 protected:
  static auto usw2_probe() -> Probe {
    Probe probe;
    probe.port = 2053;
    return probe;
  }

  static auto known() -> play::KnownDatacenters {
    return play::KnownDatacenters{
        {measured("usw2", 30, 100, usw2_probe()), measured("use1", 55, 100)}};
  }
};

TEST_F(KnownDatacenters, AreOrderedByRoundTripKeepingTheFirstOfEachName) {
  const play::KnownDatacenters known{{measured("slow", 120), measured("b", 40),
                                      measured("a", 40), measured("fast", 9),
                                      measured("b", 3)}};
  EXPECT_EQ(names(known), (std::vector<std::string>{"fast", "a", "b", "slow"}));
  const auto* b = known.find(name("b"));
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->round_trip(), milliseconds{40});
  EXPECT_EQ(known.find(name("missing")), nullptr);
  EXPECT_EQ(known.size(), 4u);
  EXPECT_FALSE(known.empty());
  EXPECT_TRUE(play::KnownDatacenters{}.empty());
  EXPECT_TRUE(play::KnownDatacenters{}.all().empty());
}

TEST_F(KnownDatacenters, KeepOnlyTheFastestSixtyFour) {
  std::vector<DatacenterMeasurement> many;
  for (int index = 0; index < 70; ++index)
    many.push_back(measured("dc" + std::to_string(index), 200 - index));
  const play::KnownDatacenters capped{many};
  EXPECT_EQ(play::KnownDatacenters::limit, 64u);
  ASSERT_EQ(capped.size(), 64u);
  EXPECT_EQ(capped.all().front().name(), "dc69");
  EXPECT_EQ(capped.all().back().name(), "dc6");
  EXPECT_EQ(capped.find(name("dc5")), nullptr);
}

TEST_F(KnownDatacenters, KeepTheOldTimeWhenTheSameProbeIsRecorded) {
  const auto before = known();
  const auto same =
      before.recording(std::vector{measured("usw2", 30, 999, usw2_probe())});
  EXPECT_EQ(same, before);
  const auto* usw2 = same.find(name("usw2"));
  ASSERT_NE(usw2, nullptr);
  EXPECT_EQ(usw2->measured_at(), at(100));
}

TEST_F(KnownDatacenters, ReplaceAMeasurementWhoseRoundTripMoved) {
  const auto moved =
      known().recording(std::vector{measured("usw2", 34, 999, usw2_probe())});
  const auto* usw2 = moved.find(name("usw2"));
  ASSERT_NE(usw2, nullptr);
  EXPECT_EQ(usw2->round_trip(), milliseconds{34});
  EXPECT_EQ(usw2->measured_at(), at(999));
  const auto* use1 = moved.find(name("use1"));
  ASSERT_NE(use1, nullptr);
  EXPECT_EQ(use1->measured_at(), at(100));
  EXPECT_EQ(moved.size(), 2u);
}

TEST_F(KnownDatacenters, ReplaceAMeasurementWhoseProbeChanged) {
  auto changed = usw2_probe();
  changed.port = 2054;
  const auto reprobed =
      known().recording(std::vector{measured("usw2", 30, 999, changed)});
  const auto* usw2 = reprobed.find(name("usw2"));
  ASSERT_NE(usw2, nullptr);
  EXPECT_EQ(usw2->measured_at(), at(999));
}

TEST_F(KnownDatacenters, AddNewDatacentersKeepingTheLatestMeasurement) {
  const auto grown = known().recording(
      std::vector{measured("euc1", 12, 500), measured("euc1", 14, 501)});
  EXPECT_EQ(names(grown), (std::vector<std::string>{"euc1", "usw2", "use1"}));
  const auto* euc1 = grown.find(name("euc1"));
  ASSERT_NE(euc1, nullptr);
  EXPECT_EQ(euc1->round_trip(), milliseconds{14});
  EXPECT_EQ(known().recording({}), known());
}

TEST_F(KnownDatacenters, StayCappedWhenRecording) {
  std::vector<DatacenterMeasurement> fresh;
  for (int index = 0; index < 70; ++index)
    fresh.push_back(measured("n" + std::to_string(index), 60 + index, 200));
  const auto full = known().recording(fresh);
  ASSERT_EQ(full.size(), play::KnownDatacenters::limit);
  EXPECT_EQ(full.all()[0].name(), "usw2");
  EXPECT_EQ(full.all()[1].name(), "use1");
  EXPECT_EQ(full.all()[2].name(), "n0");
  EXPECT_NE(full.find(name("n61")), nullptr);
  EXPECT_EQ(full.find(name("n62")), nullptr);
}

}  // namespace
}  // namespace play_test
