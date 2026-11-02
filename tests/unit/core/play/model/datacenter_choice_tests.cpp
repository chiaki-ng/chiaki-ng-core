#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/model/datacenter_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using play::DatacenterChoice;
using std::chrono::milliseconds;

class DatacenterChoices : public DatacenterRig {};

TEST_F(DatacenterChoices, SayAnAutomaticChoiceWasNeitherForcedNorMadeAtLaunch) {
  const auto automatic =
      DatacenterChoice::automatic(name("usw2"), milliseconds{22});
  EXPECT_EQ(automatic.how(), DatacenterChoice::How::automatic);
  EXPECT_EQ(automatic.round_trip(), milliseconds{22});
  EXPECT_FALSE(automatic.was_forced());
  EXPECT_FALSE(automatic.was_made_at_launch());
  EXPECT_FALSE(automatic.last_measured_at());
  EXPECT_FALSE(
      DatacenterChoice::automatic(name("usw2"), milliseconds{0}).round_trip());
  EXPECT_FALSE(
      DatacenterChoice::automatic(name("usw2"), std::nullopt).round_trip());
}

TEST_F(DatacenterChoices, TakeAPreferredDatacentersLastMeasurement) {
  const play::KnownDatacenters known{{measured("usw2", 30, 100)}};
  const auto preferred = DatacenterChoice::preferred(name("usw2"), known);
  EXPECT_TRUE(preferred.was_forced());
  EXPECT_FALSE(preferred.was_made_at_launch());
  EXPECT_EQ(preferred.round_trip(), milliseconds{30});
  EXPECT_EQ(preferred.last_measured_at(), at(100));
  const auto unmeasured = DatacenterChoice::preferred(name("euc1"), known);
  EXPECT_TRUE(unmeasured.was_forced());
  EXPECT_FALSE(unmeasured.round_trip());
  EXPECT_FALSE(unmeasured.last_measured_at());
}

TEST_F(DatacenterChoices, SayAChoiceAtLaunchWasMadeAtLaunch) {
  const auto picked =
      DatacenterChoice::picked_at_launch(name("use1"), milliseconds{95});
  EXPECT_TRUE(picked.was_forced());
  EXPECT_TRUE(picked.was_made_at_launch());
  EXPECT_EQ(picked.round_trip(), milliseconds{95});
  const auto lowest =
      DatacenterChoice::lowest_at_launch(name("use1"), milliseconds{20});
  EXPECT_FALSE(lowest.was_forced());
  EXPECT_TRUE(lowest.was_made_at_launch());
  const auto waited =
      DatacenterChoice::lowest_after_waiting(name("use1"), milliseconds{20});
  EXPECT_FALSE(waited.was_forced());
  EXPECT_TRUE(waited.was_made_at_launch());
  EXPECT_NE(waited, lowest);
  EXPECT_EQ(waited.how(), DatacenterChoice::How::lowest_after_waiting);
}

}  // namespace
}  // namespace play_test
