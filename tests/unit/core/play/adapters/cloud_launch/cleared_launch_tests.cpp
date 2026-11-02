#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/cloud_launch/launching.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace play_test {

using test_support::play::LaunchCase;
using test_support::play::Title;
namespace {

namespace catalog = libchiaking::catalog;
namespace cloud = libchiaking::cloud;
namespace play = libchiaking::play;
using cloud_launches::Launching;

class CloudLaunchClearedTitle : public cloud_launches::LaunchTest {
 protected:
  auto cleared_case() const -> LaunchCase {
    auto launched = ps5_case();
    launched.cleared = launched.title.cleared();
    return launched;
  }
};

TEST_F(CloudLaunchClearedTitle, StartsWithoutAskingTheCatalog) {
  Launching ps5{cleared_case()};
  ps5.clearance().told = {catalog::LaunchCheck::loading_library};
  const auto slot = ps5.core().run();
  ASSERT_TRUE(slot);
  EXPECT_EQ(slot.value().entitlement_id, "synthetic-entitlement");
  EXPECT_EQ(ps5.clearance().clears, 0);
  EXPECT_EQ(ps5.ps().unscripted, 0U);
  const auto& typed = ps5.listener().typed;
  ASSERT_FALSE(typed.empty());
  EXPECT_EQ(typed.front().activity(), Activity::reserving_server);
  EXPECT_TRUE(std::ranges::none_of(typed, [](const auto& progress) {
    return play::LaunchProgress::phase_of(*progress.activity()) ==
               play::LaunchPhase::preparing &&
           *progress.activity() != Activity::reserving_server;
  }));
  EXPECT_EQ(typed.back().activity(), Activity::allocating_server);
}

TEST_F(CloudLaunchClearedTitle, RetriesWithoutAskingTheCatalog) {
  Launching retried{cleared_case()};
  retried.ps().no_game_refusals = 1;
  EXPECT_TRUE(retried.core().run());
  EXPECT_EQ(retried.ps().starts, 2U);
  EXPECT_EQ(retried.clearance().clears, 0);
  EXPECT_TRUE(retried.clearance().remembered.empty());
  EXPECT_EQ(waits_of(retried.listener().typed).size(), 1U);
}

TEST_F(CloudLaunchClearedTitle, ReportsARefusalWithoutAskingTheCatalog) {
  Launching refused{cleared_case()};
  refused.ps().no_game_refusals = 2;
  EXPECT_FALSE(refused.core().run());
  EXPECT_EQ(refused.ps().starts, 2U);
  EXPECT_EQ(refused.clearance().clears, 0);
  ASSERT_EQ(refused.clearance().remembered.size(), 1U);
  EXPECT_EQ(refused.clearance().remembered.front().kind,
            catalog::StreamRefusal::Kind::no_game_for_entitlement);
}

TEST_F(CloudLaunchClearedTitle,
       StartsAnOldServiceTitleWithoutAskingTheCatalog) {
  auto old_case = old_service_case("HELD");
  old_case.cleared = old_case.title.cleared();
  Launching old_service{old_case};
  const auto slot = old_service.core().run();
  ASSERT_TRUE(slot);
  EXPECT_EQ(slot.value().service, cloud::CloudService::legacy_cloud_stream);
  EXPECT_EQ(old_service.clearance().clears, 0);
  EXPECT_EQ(old_service.core().service(),
            catalog::ServiceGeneration::old_service);
}

TEST_F(CloudLaunchClearedTitle, AsksTheCatalogWhenClearedForAnotherTitle) {
  auto elsewhere = ps5_case();
  elsewhere.cleared =
      Title{.product = "EP0109-PPSA07028_00-FAKE0110TITLE000"}.cleared();
  Launching mismatched{elsewhere};
  EXPECT_TRUE(mismatched.core().run());
  EXPECT_EQ(mismatched.clearance().clears, 1);
  ASSERT_FALSE(mismatched.clearance().cleared_products.empty());
  EXPECT_EQ(mismatched.clearance().cleared_products.front(),
            "synthetic-entitlement");
  ASSERT_FALSE(mismatched.listener().typed.empty());
  EXPECT_EQ(mismatched.listener().typed.front().activity(),
            Activity::clearing_with_catalog);
}

}  // namespace
}  // namespace play_test
