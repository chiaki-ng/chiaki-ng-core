#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/openssl/household.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using Service = libchiaking::catalog::ServiceGeneration;

class PlayKnownDatacenters : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto measured = play::DatacenterMeasurement::create(
        dc1_, std::chrono::milliseconds{21},
        play::DatacenterMeasurement::Time{std::chrono::milliseconds{1000}});
    ASSERT_TRUE(measured);
    home_.datacenters.held[Service::ps5_cloud] = {*measured};
  }

  Household home_;
  play::DatacenterName dc1_ = play::DatacenterName::parse("dc1").value();
};

TEST_F(PlayKnownDatacenters, AreKeptPerService) {
  const auto known = home_.play.known_datacenters(Service::ps5_cloud);
  ASSERT_EQ(known.size(), 1U);
  ASSERT_NE(known.find(dc1_), nullptr);
  EXPECT_EQ(known.find(dc1_)->round_trip(), std::chrono::milliseconds{21});
  EXPECT_TRUE(home_.play.known_datacenters(Service::old_service).empty());
}

TEST_F(PlayKnownDatacenters, CanBeForgotten) {
  EXPECT_TRUE(home_.play.forget_datacenters(Service::ps5_cloud));
  EXPECT_TRUE(home_.play.known_datacenters(Service::ps5_cloud).empty());
  EXPECT_TRUE(home_.datacenters.held[Service::ps5_cloud].empty());
}

TEST_F(PlayKnownDatacenters, ReportsAStoreThatCannotForget) {
  home_.datacenters.failing = true;
  const auto failed = home_.play.forget_datacenters(Service::old_service);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), play::DatacenterStore::Error::unavailable);
}

}  // namespace
}  // namespace play_test
