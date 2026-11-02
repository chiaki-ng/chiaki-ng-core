#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/model/datacenter_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace play_test {
namespace {

using libchiaking::play::DatacenterPreference;
using std::chrono::milliseconds;

class DatacenterPreferences : public DatacenterRig {};

TEST_F(DatacenterPreferences, AreAutomaticByDefault) {
  const DatacenterPreference automatic;
  EXPECT_TRUE(automatic.is_automatic());
  EXPECT_FALSE(automatic.asks_at_launch());
  EXPECT_FALSE(automatic.name());
}

TEST_F(DatacenterPreferences, CanNameADatacenter) {
  const DatacenterPreference automatic;
  const auto named = DatacenterPreference::named(name("usw2"));
  EXPECT_FALSE(named.is_automatic());
  EXPECT_FALSE(named.asks_at_launch());
  ASSERT_TRUE(named.name());
  EXPECT_EQ(*named.name(), "usw2");
  EXPECT_NE(named, automatic);
  EXPECT_EQ(named, DatacenterPreference::named(name("usw2")));
}

TEST_F(DatacenterPreferences, CanAskAtLaunch) {
  const DatacenterPreference automatic;
  const auto asked = DatacenterPreference::ask_at_launch();
  ASSERT_TRUE(asked);
  EXPECT_TRUE(asked->asks_at_launch());
  EXPECT_FALSE(asked->is_automatic());
  EXPECT_FALSE(asked->name());
  EXPECT_EQ(asked->wait(), milliseconds{15000});
  EXPECT_NE(*asked, automatic);
}

TEST_F(DatacenterPreferences, BoundTheWaitAtLaunch) {
  EXPECT_EQ(DatacenterPreference::ask_at_launch(milliseconds{0})->wait(),
            DatacenterPreference::default_wait);
  EXPECT_EQ(DatacenterPreference::ask_at_launch(milliseconds{60000})->wait(),
            milliseconds{60000});
  EXPECT_FALSE(DatacenterPreference::ask_at_launch(milliseconds{60001}));
  EXPECT_FALSE(DatacenterPreference::ask_at_launch(milliseconds{-1}));
}

}  // namespace
}  // namespace play_test
