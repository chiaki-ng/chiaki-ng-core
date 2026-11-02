#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/model/datacenter_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace play_test {
namespace {

using libchiaking::play::DatacenterMeasurement;
using std::chrono::milliseconds;

class DatacenterMeasurements : public DatacenterRig {
 protected:
  static auto probed() -> Probe {
    Probe probe;
    probe.samples = {milliseconds{31}, milliseconds{29}};
    probe.mtu_in = 1454;
    probe.mtu_out = 1254;
    probe.port = 2053;
    probe.public_address = "203.0.113.9";
    probe.max_bandwidth = 20000;
    return probe;
  }
};

TEST_F(DatacenterMeasurements, NeedAnAnswer) {
  EXPECT_FALSE(
      DatacenterMeasurement::create(name("usw2"), milliseconds{0}, at(1)));
  EXPECT_FALSE(
      DatacenterMeasurement::create(name("usw2"), milliseconds{-4}, at(1)));
}

TEST_F(DatacenterMeasurements, KeepTheProbe) {
  const auto probe = probed();
  const auto measurement = measured("usw2", 30, 7, probe);
  EXPECT_EQ(measurement.name(), "usw2");
  EXPECT_EQ(measurement.round_trip(), milliseconds{30});
  EXPECT_EQ(measurement.measured_at(), at(7));
  EXPECT_EQ(measurement.probe(), probe);
  EXPECT_FALSE(measurement.is_high_latency());
}

TEST_F(DatacenterMeasurements, AreHighLatencyAbove100Milliseconds) {
  EXPECT_FALSE(measured("a", 100).is_high_latency());
  EXPECT_TRUE(measured("a", 101).is_high_latency());
  EXPECT_EQ(DatacenterMeasurement::high_latency_threshold, milliseconds{100});
}

TEST_F(DatacenterMeasurements, MatchTheSameProbeTakenAtAnotherTime) {
  const auto probe = probed();
  const auto measurement = measured("usw2", 30, 7, probe);
  EXPECT_TRUE(measurement.is_same_probe_as(measured("usw2", 30, 900, probe)));
  EXPECT_NE(measurement, measured("usw2", 30, 900, probe));
  EXPECT_FALSE(measurement.is_same_probe_as(measured("usw2", 31, 7, probe)));
  EXPECT_FALSE(measurement.is_same_probe_as(measured("use1", 30, 7, probe)));
  auto other = probe;
  other.mtu_in = 1400;
  EXPECT_FALSE(measurement.is_same_probe_as(measured("usw2", 30, 7, other)));
}

}  // namespace
}  // namespace play_test
