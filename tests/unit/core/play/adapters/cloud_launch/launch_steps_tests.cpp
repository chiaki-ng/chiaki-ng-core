#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/cloud_launch/launching.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {

using test_support::play::RecordingListener;
namespace {

namespace catalog = libchiaking::catalog;
namespace cloud = libchiaking::cloud;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using cloud_launches::Launching;

class CloudLaunchSteps : public cloud_launches::LaunchTest {};

TEST_F(CloudLaunchSteps, APs5LaunchReportsOnlyPs5Activities) {
  Launching ps5{ps5_case()};
  EXPECT_EQ(ps5.core().service(), catalog::ServiceGeneration::ps5_cloud);
  EXPECT_EQ(ps5.core().product(), "synthetic-entitlement");
  EXPECT_EQ(ps5.core().progress(),
            play::LaunchProgress{catalog::ServiceGeneration::ps5_cloud});
  EXPECT_EQ(ps5.core().progress().phase(), play::LaunchPhase::starting);
  const play::CloudLaunch handle{ps5.core()};
  EXPECT_EQ(handle.service(), catalog::ServiceGeneration::ps5_cloud);
  EXPECT_EQ(handle.product(), ps5.core().product());

  const auto slot = ps5.core().run();
  ASSERT_TRUE(slot);
  EXPECT_EQ(ps5.ps().unscripted, 0U);
  EXPECT_EQ(slot.value().service, cloud::CloudService::cloud_stream);
  EXPECT_EQ(slot.value().platform, cloud::CloudPlatform::ps5);
  EXPECT_EQ(slot.value().entitlement_id, "synthetic-entitlement");

  const std::vector<std::pair<Activity, Step>> expected{
      {Activity::clearing_with_catalog, {0, 0}},
      {Activity::reserving_server, {1, 10}},
      {Activity::reserving_server, {2, 10}},
      {Activity::reserving_server, {3, 10}},
      {Activity::reserving_server, {4, 10}},
      {Activity::reserving_server, {5, 10}},
      {Activity::reserving_server, {6, 10}},
      {Activity::reserving_server, {7, 10}},
      {Activity::measuring_datacenters, {8, 10}},
      {Activity::measuring_datacenters, {9, 10}},
      {Activity::allocating_server, {10, 10}}};
  const auto& typed = ps5.listener().typed;
  ASSERT_EQ(typed.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    SCOPED_TRACE(index);
    const auto& progress = typed[index];
    EXPECT_EQ(progress.service(), catalog::ServiceGeneration::ps5_cloud);
    EXPECT_EQ(progress.activity(), expected[index].first);
    EXPECT_EQ(step_of(progress), expected[index].second);
    ASSERT_TRUE(progress.activity());
    EXPECT_FALSE(is_old_service_only(*progress.activity()));
    EXPECT_TRUE(play::LaunchProgress::belongs_to(*progress.activity(),
                                                 progress.service()));
  }
  EXPECT_EQ(typed.front().phase(), play::LaunchPhase::preparing);
  EXPECT_EQ(typed.back().phase(), play::LaunchPhase::finding_server);
  EXPECT_EQ(ps5.core().progress(), typed.back());
  EXPECT_EQ(handle.progress(), typed.back());
  EXPECT_EQ(
      ps5.listener().texts,
      std::vector<std::string>(
          {"Checking PlayStation Plus Premium and title streaming",
           "Getting Client IDs - Step 1 of 10",
           "Getting Configuration - Step 2 of 10",
           "Starting Session - Step 3 of 10", "Getting Tokens - Step 4 of 10",
           "Getting Server Tokens - Step 5 of 10",
           "Authorizing Session - Step 6 of 10",
           "Locking Session - Step 7 of 10",
           "Getting Datacenters - Step 8 of 10",
           "Selecting Datacenter (dc1) - Step 9 of 10",
           "Allocating Streaming Slot - Step 10 of 10"}));
}

class ForwardingObserver final : public play::LaunchObserver {
 public:
  explicit ForwardingObserver(RecordingListener& listener)
      : listener_(listener) {}

  void on_progress(const play::LaunchProgress& progress,
                   remote_play::FailureStage stage,
                   std::string_view text) override {
    listener_.progressed(progress, stage, text);
  }

 private:
  RecordingListener& listener_;
};

TEST(LaunchReport, KeepsOldServiceActivitiesOutOfAPs5Launch) {
  for (const auto activity : {play::LaunchActivity::opening_title_session,
                              play::LaunchActivity::resolving_title,
                              play::LaunchActivity::taking_free_licence,
                              play::LaunchActivity::opening_account_session}) {
    SCOPED_TRACE(static_cast<int>(activity));
    EXPECT_FALSE(play::LaunchProgress::at(catalog::ServiceGeneration::ps5_cloud,
                                          activity));
    EXPECT_TRUE(play::LaunchProgress::at(
        catalog::ServiceGeneration::old_service, activity));
    RecordingListener unheard;
    ForwardingObserver forward{unheard};
    const play::LaunchReport report{forward,
                                    catalog::ServiceGeneration::ps5_cloud};
    report.step(activity, remote_play::FailureStage::cloud_title,
                "Resolving Game", 2, 5);
    report.say(activity, remote_play::FailureStage::cloud_title,
               "Resolving Game");
    EXPECT_TRUE(unheard.typed.empty());
    EXPECT_TRUE(unheard.texts.empty());
    const play::LaunchReport old_report{
        forward, catalog::ServiceGeneration::old_service};
    old_report.step(activity, remote_play::FailureStage::cloud_title,
                    "Resolving Game", 2, 5);
    ASSERT_EQ(unheard.typed.size(), 1U);
    EXPECT_EQ(unheard.typed.front().activity(), activity);
  }
}

TEST_F(CloudLaunchSteps, AnOldServiceLaunchReportsItsOwnTitleSteps) {
  Launching old_service{old_service_case()};
  EXPECT_EQ(old_service.core().service(),
            catalog::ServiceGeneration::old_service);
  const auto slot = old_service.core().run();
  ASSERT_TRUE(slot);
  EXPECT_EQ(old_service.ps().unscripted, 0U);
  EXPECT_EQ(old_service.ps().lookups, 1U);
  EXPECT_EQ(slot.value().service, cloud::CloudService::legacy_cloud_stream);
  EXPECT_EQ(slot.value().platform, cloud::CloudPlatform::ps4);
  EXPECT_EQ(slot.value().entitlement_id, "RESOLVED");

  std::vector<std::pair<Activity, Step>> title_steps;
  for (const auto& progress : old_service.listener().typed) {
    EXPECT_EQ(progress.service(), catalog::ServiceGeneration::old_service);
    if (is_old_service_only(*progress.activity()))
      title_steps.emplace_back(*progress.activity(), step_of(progress));
  }
  EXPECT_EQ(title_steps, (std::vector<std::pair<Activity, Step>>{
                             {Activity::opening_title_session, {1, 5}},
                             {Activity::opening_title_session, {1, 5}},
                             {Activity::resolving_title, {2, 5}},
                             {Activity::taking_free_licence, {3, 5}},
                             {Activity::opening_account_session, {4, 5}},
                             {Activity::opening_account_session, {5, 5}}}));
  ASSERT_FALSE(old_service.listener().typed.empty());
  EXPECT_EQ(old_service.listener().typed.front().activity(),
            Activity::clearing_with_catalog);
  EXPECT_EQ(old_service.listener().typed.back().activity(),
            Activity::allocating_server);
}

TEST_F(CloudLaunchSteps, AnOldServiceLaunchOfAHeldTitleSkipsResolvingIt) {
  Launching known{old_service_case("OWNED")};
  const auto fast = known.core().run();
  ASSERT_TRUE(fast);
  EXPECT_EQ(known.ps().lookups, 0U);
  EXPECT_EQ(fast.value().entitlement_id, "OWNED");
  for (const auto& progress : known.listener().typed) {
    EXPECT_NE(*progress.activity(), Activity::resolving_title);
    EXPECT_NE(*progress.activity(), Activity::taking_free_licence);
  }
}

TEST_F(CloudLaunchSteps, ReportsEachCatalogCheckBetweenClearingAndReserving) {
  using Check = catalog::LaunchCheck;
  Launching ps5{ps5_case()};
  ps5.clearance().told = {Check::loading_library, Check::cloud_streaming,
                          Check::ownership,       Check::entitlement,
                          Check::cloud_game,      Check::exclusions,
                          Check::release};
  ASSERT_TRUE(ps5.core().run());
  std::vector<Activity> activities;
  std::ranges::transform(
      ps5.listener().typed, std::back_inserter(activities),
      [](const auto& progress) { return *progress.activity(); });
  const std::vector<Activity> opening{
      Activity::clearing_with_catalog,    Activity::loading_library,
      Activity::checking_cloud_streaming, Activity::checking_ownership,
      Activity::checking_entitlement,     Activity::resolving_cloud_game,
      Activity::checking_exclusions,      Activity::checking_release,
      Activity::reserving_server};
  ASSERT_GT(activities.size(), opening.size());
  EXPECT_TRUE(
      std::ranges::equal(opening, std::span{activities}.first(opening.size())));
  for (std::size_t index = 0; index + 1 < opening.size(); ++index) {
    SCOPED_TRACE(index);
    const auto& progress = ps5.listener().typed[index];
    EXPECT_EQ(progress.phase(), play::LaunchPhase::preparing);
    EXPECT_FALSE(progress.step());
    EXPECT_EQ(progress.service(), catalog::ServiceGeneration::ps5_cloud);
  }
  ASSERT_GE(ps5.listener().texts.size(), 8U);
  EXPECT_EQ(
      std::vector<std::string>(ps5.listener().texts.begin(),
                               ps5.listener().texts.begin() + 8),
      (std::vector<std::string>{
          "Checking PlayStation Plus Premium and title streaming",
          "Loading your games", "Checking cloud streaming access",
          "Checking that you own the game", "Checking your copy of the game",
          "Finding the cloud version of the game",
          "Checking the game is not restricted", "Checking the release date"}));
}

TEST_F(CloudLaunchSteps, ReportsTheOldServiceCatalogChecks) {
  using Check = catalog::LaunchCheck;
  Launching old_service{old_service_case()};
  old_service.clearance().told = {Check::loading_library, Check::membership,
                                  Check::release};
  ASSERT_TRUE(old_service.core().run());
  const auto& typed = old_service.listener().typed;
  ASSERT_GT(typed.size(), 4U);
  EXPECT_EQ(typed[0].activity(), Activity::clearing_with_catalog);
  EXPECT_EQ(typed[1].activity(), Activity::loading_library);
  EXPECT_EQ(typed[2].activity(), Activity::checking_membership);
  EXPECT_EQ(typed[3].activity(), Activity::checking_release);
  EXPECT_EQ(typed[2].service(), catalog::ServiceGeneration::old_service);
}

}  // namespace
}  // namespace play_test
