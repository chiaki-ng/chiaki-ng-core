#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/usecases/clearance_rig.hpp"
#include "tests/unit/core/catalog/usecases/cloud_stream/launch_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace catalog_test {
namespace {

using libchiaking::catalog::Platform;
using libchiaking::catalog::ServiceGeneration;

class CloudStreamClearanceOfOwnedGames : public ::testing::Test,
                                         protected CloudStreamCases {};

TEST_F(CloudStreamClearanceOfOwnedGames,
       RefusesAnUnlistedTitleWithoutAskingPs) {
  CloudStreamLaunch unowned;
  EXPECT_EQ(kind_of(unowned.clear(owning_nothing(), bus)), Kind::not_listed);
  EXPECT_EQ(kind_of(unowned.clear(owning_nothing(), "EP-PPSA77003_00-NOWHERE")),
            Kind::not_listed);
  EXPECT_EQ(unowned.rig.http.requests(), 0U);
}

TEST_F(CloudStreamClearanceOfOwnedGames, ClearsAnOwnedGameAfterEveryCheck) {
  CloudStreamLaunch owned;
  const auto verdict = owned.clear(owning_rivet(), rivet);
  EXPECT_TRUE(verdict.is_allowed());
  ASSERT_TRUE(verdict.cleared_launch());
  const auto& cleared = *verdict.cleared_launch();
  EXPECT_EQ(cleared.identifier.text(), rivet);
  ASSERT_TRUE(cleared.held_as);
  EXPECT_EQ(cleared.held_as->text(), rivet);
  EXPECT_EQ(cleared.service, ServiceGeneration::ps5_cloud);
  EXPECT_EQ(cleared.platform, Platform::ps5);
  EXPECT_EQ(cleared.product, product(rivet));
}

TEST_F(CloudStreamClearanceOfOwnedGames, AsksPsInLaunchOrder) {
  CloudStreamLaunch owned;
  ASSERT_TRUE(owned.clear(owning_rivet(), rivet).is_allowed());
  const auto benefits =
      owned.rig.http.first_asking(ClearanceRig::cloud_benefits);
  const auto record = owned.rig.http.first_asking(ClearanceRig::ps5_record);
  const auto resolved = owned.rig.http.first_asking(ClearanceRig::resolver);
  const auto excluded =
      owned.rig.http.first_asking(ClearanceRig::feature_config);
  ASSERT_TRUE(benefits);
  ASSERT_TRUE(record);
  ASSERT_TRUE(resolved);
  ASSERT_TRUE(excluded);
  EXPECT_LT(*benefits, *record);
  EXPECT_LT(*record, *resolved);
  EXPECT_LT(*resolved, *excluded);
}

TEST_F(CloudStreamClearanceOfOwnedGames, AsksForTheExactEntitlementAndGame) {
  CloudStreamLaunch owned;
  ASSERT_TRUE(owned.clear(owning_rivet(), rivet).is_allowed());
  EXPECT_TRUE(owned.rig.http.first_asking(
      "internal/entitlements/"
      "EP0109-PPSA07028_00-FAKE0110TITLE000?fields=gameMeta,titleMeta,"
      "productMeta,conceptMeta,streamingSupported,skuMeta,preorderCoreFlag,"
      "isPlayTimeFullyConsumed"));
  EXPECT_TRUE(owned.rig.http.first_asking(
      "products/"
      "cloudgame?productId=EP0109-PPSA07028_00-FAKE0110TITLE000&country=GB&"
      "language=en&age=99&limit=1000&benefitPlanIds=PLAN-PREMIUM"));
  EXPECT_EQ(owned.rig.http.count(ClearanceRig::store_app), 0);
  EXPECT_EQ(owned.rig.http.count(ClearanceRig::subscriptions), 0);
  EXPECT_EQ(owned.rig.http.count(ClearanceRig::ps5_copies), 0);
  EXPECT_EQ(owned.rig.http.count(ClearanceRig::owned_lookup), 0);
}

TEST_F(CloudStreamClearanceOfOwnedGames,
       AsksAgainEachLaunchButKeepsTheExclusions) {
  CloudStreamLaunch owned;
  ASSERT_TRUE(owned.clear(owning_rivet(), rivet).is_allowed());
  EXPECT_TRUE(owned.clear(owning_rivet(), rivet).is_allowed());
  EXPECT_EQ(owned.rig.http.count(ClearanceRig::cloud_benefits), 2);
  EXPECT_EQ(owned.rig.http.count(ClearanceRig::ps5_record), 2);
  EXPECT_EQ(owned.rig.http.count(ClearanceRig::resolver), 2);
  EXPECT_EQ(owned.rig.http.count(ClearanceRig::feature_config), 1);
}

TEST_F(CloudStreamClearanceOfOwnedGames, RefusesACopyPsNoLongerHas) {
  for (const auto& gone :
       {std::pair{404, std::string{"{}"}},
        std::pair{400, std::string{R"({"error":{"code":3161955}})"}}}) {
    CloudStreamLaunch lapsed;
    lapsed.rig.ps5_record_answers(gone.first, gone.second);
    EXPECT_EQ(kind_of(lapsed.clear(owning_rivet(), rivet)), Kind::invalid_title)
        << gone.first;
    EXPECT_EQ(lapsed.rig.http.count(ClearanceRig::resolver), 0) << gone.first;
    EXPECT_EQ(lapsed.rig.http.count(ClearanceRig::store_app), 0) << gone.first;
  }
}

TEST_F(CloudStreamClearanceOfOwnedGames, RefusesACopyByWhatPsNowReads) {
  EXPECT_EQ(
      CloudStreamLaunch::refused_as(entitlement_reading(
          R"("activeFlag":true,"entitlementType":5,"featureType":3,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true)")),
      Kind::invalid_title);
  EXPECT_EQ(
      CloudStreamLaunch::refused_as(entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":false,"entitlementType":5,"featureType":3,"gameMeta":{"packageType":"PSGD"},"streamingSupported":false)")),
      Kind::title_taken_down);
  EXPECT_EQ(CloudStreamLaunch::refused_as(
                ClearanceRig::ps5_game(rivet, "Owned", "false")),
            Kind::streaming_unavailable);
  EXPECT_EQ(
      CloudStreamLaunch::refused_as(entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":true,"entitlementType":5,"featureType":3,"gameMeta":{"packageType":"PSGD"})")),
      Kind::streaming_unavailable);
  EXPECT_EQ(
      CloudStreamLaunch::refused_as(entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":false,"entitlementType":5,"featureType":3,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true)")),
      Kind::purchase_needed);
  EXPECT_EQ(
      CloudStreamLaunch::refused_as(entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":false,"entitlementType":5,"featureType":7,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true)")),
      Kind::purchase_needed);
  EXPECT_EQ(
      CloudStreamLaunch::refused_as(entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":true,"entitlementType":5,"featureType":7,"isPlayTimeFullyConsumed":true,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true)")),
      Kind::purchase_needed);
  EXPECT_EQ(
      CloudStreamLaunch::refused_as(entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":true,"entitlementType":5,"featureType":7,"gameMeta":{"packageType":"PSGD","isPlayTimeFullyConsumed":true},"streamingSupported":true)")),
      Kind::purchase_needed);
  EXPECT_EQ(
      CloudStreamLaunch::refused_as(entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":true,"entitlementType":5,"featureType":5,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true)")),
      Kind::unexpected_entitlement_state);
  EXPECT_EQ(
      CloudStreamLaunch::refused_as(entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":false,"entitlementType":5,"featureType":1,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true)")),
      Kind::purchase_needed);
  EXPECT_EQ(
      CloudStreamLaunch::refused_as(entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":true,"entitlementType":5,"featureType":3,"gameMeta":{"packageType":"PSAL"},"streamingSupported":true)")),
      Kind::unexpected_entitlement_state);
  EXPECT_EQ(
      CloudStreamLaunch::refused_as(entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":true,"entitlementType":5,"featureType":1,"gameMeta":{"packageType":"PSGD"},"streamingSupported":false)")),
      Kind::streaming_unavailable);
  EXPECT_EQ(
      CloudStreamLaunch::refused_as(entitlement_reading(
          R"("productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":false,"entitlementType":5,"featureType":1,"gameMeta":{"packageType":"PSAL"},"streamingSupported":true)")),
      Kind::purchase_needed);
}

TEST_F(CloudStreamClearanceOfOwnedGames,
       ReportsTheCheckUnavailableWhenTheCopyCannotBeRead) {
  for (const int status : {403, 503}) {
    CloudStreamLaunch unreachable;
    unreachable.rig.ps5_record_answers(status, "{}");
    EXPECT_EQ(kind_of(unreachable.clear(owning_rivet(), rivet)),
              Kind::check_unavailable)
        << status;
    EXPECT_EQ(unreachable.rig.http.count(ClearanceRig::resolver), 0) << status;
  }
}

TEST_F(CloudStreamClearanceOfOwnedGames,
       RefusesAnUnclaimedPremiumGameWithItsExactOffer) {
  CloudStreamLaunch unclaimed;
  const auto refused = unclaimed.clear(owning_nothing(), show);
  EXPECT_EQ(kind_of(refused), Kind::add_to_library);
  const auto& offer = refused.refusal()->library_offer();
  ASSERT_TRUE(offer);
  EXPECT_EQ(offer->sku_id, "SKU1");
  EXPECT_EQ(offer->entitlement_ids,
            std::vector<std::string>{std::string{show}});
  EXPECT_EQ(offer->entitled_through_subscription, true);
  EXPECT_EQ(unclaimed.rig.http.count(ClearanceRig::store_app), 0);
  EXPECT_EQ(unclaimed.rig.http.count(ClearanceRig::ps5_record), 0);
  EXPECT_EQ(unclaimed.rig.http.count(ClearanceRig::resolver), 0);
  EXPECT_EQ(unclaimed.rig.http.count(ClearanceRig::feature_config), 0);
  EXPECT_EQ(unclaimed.gk(), 0);
  EXPECT_EQ(unclaimed.rig.http.count(ClearanceRig::cloud_benefits), 1);
}

TEST_F(CloudStreamClearanceOfOwnedGames,
       RefusesAnUnclaimedPremiumGameWithoutCloudStreaming) {
  CloudStreamLaunch denied;
  denied.rig.cloud_streaming_denied("NONE");
  EXPECT_EQ(kind_of(denied.clear(owning_nothing(), show)), Kind::needs_premium);
}

TEST_F(CloudStreamClearanceOfOwnedGames,
       RechecksUnknownOwnershipByTheExactEntitlement) {
  CloudStreamLaunch found;
  found.rig.ps5_record_answers(200, show_record());
  const auto verdict = found.clear(owning_unknown(), show);
  EXPECT_TRUE(verdict.is_allowed());
  ASSERT_TRUE(verdict.cleared_launch());
  EXPECT_EQ(verdict.cleared_launch()->identifier.text(), show);
  ASSERT_TRUE(verdict.cleared_launch()->held_as);
  EXPECT_EQ(verdict.cleared_launch()->held_as->text(), show);
  EXPECT_EQ(found.rig.http.count(ClearanceRig::ps5_record), 2);
  EXPECT_EQ(found.rig.http.count(ClearanceRig::owned_lookup), 0);
  EXPECT_EQ(found.rig.http.count(ClearanceRig::store_app), 0);
  EXPECT_TRUE(found.rig.http.first_asking(
      "internal/entitlements/UP0114-PPSA37004_00-FAKE0071TITLE000?fields="));
}

TEST_F(CloudStreamClearanceOfOwnedGames,
       OffersAnUnknownGameThatTurnsOutUnowned) {
  CloudStreamLaunch absent;
  absent.rig.library_answers(404, "{}");
  absent.rig.ps5_record_answers(404, "{}");
  absent.rig.ps5_library_holds();
  const auto offered = absent.clear(owning_unknown(), show);
  EXPECT_EQ(kind_of(offered), Kind::add_to_library);
  ASSERT_TRUE(offered.refusal()->library_offer());
  EXPECT_EQ(offered.refusal()->library_offer()->sku_id, "SKU1");
  EXPECT_EQ(absent.rig.http.count(ClearanceRig::store_app), 0);
  EXPECT_EQ(absent.rig.http.count(ClearanceRig::ps5_record), 1);
}

TEST_F(CloudStreamClearanceOfOwnedGames,
       AsksForAnOwnershipCheckOnlyBeforeTheLaunch) {
  CloudStreamLaunch failing;
  failing.rig.ps5_record_answers(503, "{}");
  const auto unanswered = failing.clear(owning_unknown(), show);
  EXPECT_EQ(kind_of(unanswered), Kind::check_unavailable);
  EXPECT_FALSE(unanswered.refusal()->asks_for_ownership_check());
  const auto held = failing.held(owning_unknown(), show);
  EXPECT_EQ(kind_of(held), Kind::check_unavailable);
  EXPECT_TRUE(held.refusal()->asks_for_ownership_check());
}

}  // namespace
}  // namespace catalog_test
