#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <initializer_list>

namespace play_test {
namespace {

using libchiaking::catalog::ServiceGeneration;
using libchiaking::play::LaunchActivity;
using libchiaking::play::LaunchPhase;
using libchiaking::play::LaunchProgress;
using Activity = LaunchActivity;

class LaunchProgresses : public ::testing::Test {
 protected:
  static constexpr std::array all_activities{
      Activity::clearing_with_catalog,
      Activity::checking_device,
      Activity::opening_title_session,
      Activity::resolving_title,
      Activity::taking_free_licence,
      Activity::opening_account_session,
      Activity::reserving_server,
      Activity::measuring_datacenters,
      Activity::waiting_for_datacenter_choice,
      Activity::allocating_server,
      Activity::migrating_data,
      Activity::connecting_to_server,
      Activity::measuring_bandwidth,
      Activity::loading_library,
      Activity::checking_membership,
      Activity::checking_cloud_streaming,
      Activity::checking_ownership,
      Activity::checking_entitlement,
      Activity::resolving_cloud_game,
      Activity::checking_exclusions,
      Activity::checking_release,
  };

  static auto old_service_only(LaunchActivity activity) -> bool {
    return activity == Activity::opening_title_session ||
           activity == Activity::resolving_title ||
           activity == Activity::taking_free_licence ||
           activity == Activity::opening_account_session;
  }

  static constexpr auto old = ServiceGeneration::old_service;
  static constexpr auto ps5 = ServiceGeneration::ps5_cloud;
};

TEST_F(LaunchProgresses, StartWithNothingKnown) {
  const LaunchProgress progress{ps5};
  EXPECT_EQ(progress.service(), ps5);
  EXPECT_EQ(progress.phase(), LaunchPhase::starting);
  EXPECT_FALSE(progress.activity());
  EXPECT_FALSE(progress.step());
  EXPECT_FALSE(progress.queue_position());
  EXPECT_FALSE(progress.attempt());
  EXPECT_FALSE(progress.migration_percent());
  EXPECT_NE(progress, LaunchProgress{old});
}

TEST_F(LaunchProgresses, NeverPutAnOldServiceStepInAPs5Launch) {
  for (const auto activity : all_activities) {
    SCOPED_TRACE(testing::PrintToString(activity));
    EXPECT_TRUE(LaunchProgress::at(old, activity));
    EXPECT_TRUE(LaunchProgress::belongs_to(activity, old));
    EXPECT_NE(LaunchProgress::at(ps5, activity).has_value(),
              old_service_only(activity));
    EXPECT_NE(LaunchProgress::belongs_to(activity, ps5),
              old_service_only(activity));
  }
}

TEST_F(LaunchProgresses, PlaceEveryActivityInItsPhase) {
  for (const auto activity :
       {Activity::clearing_with_catalog, Activity::checking_device,
        Activity::opening_title_session, Activity::resolving_title,
        Activity::taking_free_licence, Activity::opening_account_session,
        Activity::reserving_server, Activity::loading_library,
        Activity::checking_membership, Activity::checking_cloud_streaming,
        Activity::checking_ownership, Activity::checking_entitlement,
        Activity::resolving_cloud_game, Activity::checking_exclusions,
        Activity::checking_release}) {
    SCOPED_TRACE(testing::PrintToString(activity));
    EXPECT_EQ(LaunchProgress::phase_of(activity), LaunchPhase::preparing);
  }
  for (const auto activity :
       {Activity::measuring_datacenters, Activity::measuring_bandwidth,
        Activity::waiting_for_datacenter_choice, Activity::allocating_server,
        Activity::migrating_data}) {
    SCOPED_TRACE(testing::PrintToString(activity));
    EXPECT_EQ(LaunchProgress::phase_of(activity), LaunchPhase::finding_server);
  }
  EXPECT_EQ(LaunchProgress::phase_of(Activity::connecting_to_server),
            LaunchPhase::connecting);
  const auto resolving = LaunchProgress::at(old, Activity::resolving_title);
  ASSERT_TRUE(resolving);
  EXPECT_EQ(resolving->phase(), LaunchPhase::preparing);
  EXPECT_EQ(resolving->activity(), Activity::resolving_title);
}

TEST_F(LaunchProgresses, OrderTheirPhasesFromOne) {
  EXPECT_LT(LaunchPhase::starting, LaunchPhase::preparing);
  EXPECT_LT(LaunchPhase::preparing, LaunchPhase::finding_server);
  EXPECT_LT(LaunchPhase::finding_server, LaunchPhase::connecting);
  EXPECT_EQ(static_cast<int>(LaunchPhase::starting), 1);
  EXPECT_EQ(static_cast<int>(LaunchPhase::connecting), 4);
}

TEST_F(LaunchProgresses, CountStepsWithinTheirTotal) {
  const auto step = LaunchProgress::at(old, Activity::taking_free_licence,
                                       {.step = LaunchProgress::Step{3, 5}});
  ASSERT_TRUE(step);
  EXPECT_EQ(step->step(), (LaunchProgress::Step{3, 5}));
  EXPECT_FALSE(step->attempt());
  EXPECT_TRUE(LaunchProgress::at(old, Activity::opening_account_session,
                                 {.step = LaunchProgress::Step{5, 5}}));
  EXPECT_FALSE(LaunchProgress::at(old, Activity::resolving_title,
                                  {.step = LaunchProgress::Step{0, 5}}));
  EXPECT_FALSE(LaunchProgress::at(old, Activity::resolving_title,
                                  {.step = LaunchProgress::Step{6, 5}}));
  EXPECT_FALSE(LaunchProgress::at(old, Activity::resolving_title,
                                  {.step = LaunchProgress::Step{1, 0}}));
}

TEST_F(LaunchProgresses, CountQueuePositionsAndAttemptsWhileAllocating) {
  const auto queued = LaunchProgress::at(ps5, Activity::allocating_server,
                                         {.queue_position = 12, .attempt = 4});
  ASSERT_TRUE(queued);
  EXPECT_EQ(queued->queue_position(), 12U);
  EXPECT_EQ(queued->attempt(), 4U);
  EXPECT_EQ(queued->phase(), LaunchPhase::finding_server);
  EXPECT_FALSE(queued->migration_percent());
  EXPECT_TRUE(LaunchProgress::at(ps5, Activity::allocating_server,
                                 {.queue_position = 0, .attempt = 1}));
  EXPECT_FALSE(
      LaunchProgress::at(ps5, Activity::allocating_server, {.attempt = 0}));
  EXPECT_FALSE(LaunchProgress::at(ps5, Activity::measuring_datacenters,
                                  {.queue_position = 1}));
}

TEST_F(LaunchProgresses, CountMigrationPercentOnlyWhileMigrating) {
  const auto migrating = LaunchProgress::at(
      ps5, Activity::migrating_data, {.attempt = 2, .migration_percent = 40});
  ASSERT_TRUE(migrating);
  EXPECT_EQ(migrating->migration_percent(), 40U);
  EXPECT_EQ(migrating->attempt(), 2U);
  EXPECT_TRUE(LaunchProgress::at(ps5, Activity::migrating_data,
                                 {.migration_percent = 100}));
  EXPECT_FALSE(LaunchProgress::at(ps5, Activity::migrating_data,
                                  {.migration_percent = 101}));
  EXPECT_FALSE(LaunchProgress::at(ps5, Activity::allocating_server,
                                  {.migration_percent = 10}));
}

TEST_F(LaunchProgresses, CarryBandwidthOnlyWhileMeasuringIt) {
  const auto measured = LaunchProgress::at(ps5, Activity::measuring_bandwidth,
                                           {.bandwidth_kbps = 55000});
  ASSERT_TRUE(measured);
  EXPECT_EQ(measured->bandwidth_kbps(), 55000U);
  EXPECT_FALSE(measured->bandwidth_age());
  const auto reused = LaunchProgress::at(
      ps5, Activity::measuring_bandwidth,
      {.bandwidth_kbps = 55000, .bandwidth_age = std::chrono::seconds{720}});
  ASSERT_TRUE(reused);
  EXPECT_EQ(reused->bandwidth_age(), std::chrono::seconds{720});
  EXPECT_TRUE(LaunchProgress::at(ps5, Activity::measuring_bandwidth));
  EXPECT_FALSE(LaunchProgress::at(ps5, Activity::measuring_bandwidth,
                                  {.bandwidth_age = std::chrono::seconds{1}}));
  EXPECT_FALSE(LaunchProgress::at(ps5, Activity::allocating_server,
                                  {.bandwidth_kbps = 55000}));
}

TEST_F(LaunchProgresses, CompareByServiceActivityAndCounts) {
  const auto queued = LaunchProgress::at(ps5, Activity::allocating_server,
                                         {.queue_position = 12, .attempt = 4});
  const auto same = LaunchProgress::at(ps5, Activity::allocating_server,
                                       {.queue_position = 12, .attempt = 4});
  const auto ahead = LaunchProgress::at(ps5, Activity::allocating_server,
                                        {.queue_position = 11, .attempt = 4});
  const auto other = LaunchProgress::at(old, Activity::allocating_server,
                                        {.queue_position = 12, .attempt = 4});
  ASSERT_TRUE(queued);
  ASSERT_TRUE(same);
  ASSERT_TRUE(ahead);
  ASSERT_TRUE(other);
  EXPECT_EQ(*queued, *same);
  EXPECT_NE(*queued, *ahead);
  EXPECT_NE(*queued, *other);
}

}  // namespace
}  // namespace play_test
