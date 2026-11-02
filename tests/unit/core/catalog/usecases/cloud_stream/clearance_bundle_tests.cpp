#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/usecases/clearance_rig.hpp"
#include "tests/unit/core/catalog/usecases/cloud_stream/launch_rig.hpp"

#include <gtest/gtest.h>

namespace catalog_test {
namespace {

class CloudStreamClearanceOfBundles : public ::testing::Test,
                                      protected CloudStreamCases {};

TEST_F(CloudStreamClearanceOfBundles, ClearsAnOwnedChildAsItself) {
  CloudStreamLaunch owned;
  owned.rig.ps5_record_answers(
      200, ClearanceRig::ps5_game_titled(first, "PPSA47002_00"));
  const auto verdict = owned.clear(owning_the_first_game(), first);
  EXPECT_TRUE(verdict.is_allowed());
  ASSERT_TRUE(verdict.cleared_launch());
  EXPECT_EQ(verdict.cleared_launch()->identifier.text(), first);
  ASSERT_TRUE(verdict.cleared_launch()->held_as);
  EXPECT_EQ(verdict.cleared_launch()->held_as->text(), first);
  EXPECT_EQ(verdict.cleared_launch()->product, product(bundle));
  EXPECT_TRUE(owned.rig.http.first_asking(
      "internal/entitlements/EP0109-PPSA47002_00-FAKE0042TITLE000?fields="));
  EXPECT_TRUE(owned.rig.http.first_asking(
      "products/cloudgame?productId=EP0109-PPSA47001_00-FAKE0133TITLE000&"));
  const auto held = owned.held(owning_the_first_game(), first);
  EXPECT_TRUE(held.is_allowed());
  ASSERT_TRUE(held.cleared_launch());
  EXPECT_EQ(held.cleared_launch()->identifier.text(), first);
}

TEST_F(CloudStreamClearanceOfBundles, OffersAnUnownedChildByTheBundleSku) {
  CloudStreamLaunch unowned;
  const auto offered = unowned.clear(owning_the_first_game(), second);
  EXPECT_EQ(kind_of(offered), Kind::add_to_library);
  ASSERT_TRUE(offered.refusal()->library_offer());
  EXPECT_EQ(offered.refusal()->library_offer()->sku_id, "BUNDLESKU");
  EXPECT_EQ(unowned.rig.http.count(ClearanceRig::ps5_record), 0);
  EXPECT_EQ(unowned.rig.http.count(ClearanceRig::store_app), 0);
  EXPECT_EQ(kind_of(unowned.held(owning_the_first_game(), second)),
            Kind::add_to_library);
}

TEST_F(CloudStreamClearanceOfBundles, ClearsTheSecondOwnedChildAsItself) {
  CloudStreamLaunch both;
  both.rig.ps5_record_answers(
      200, ClearanceRig::ps5_game_titled(second, "PPSA47003_00"));
  const auto second_game = both.clear(owning_both_games(), second);
  EXPECT_TRUE(second_game.is_allowed());
  ASSERT_TRUE(second_game.cleared_launch());
  EXPECT_EQ(second_game.cleared_launch()->identifier.text(), second);
  ASSERT_TRUE(second_game.cleared_launch()->held_as);
  EXPECT_EQ(second_game.cleared_launch()->held_as->text(), second);
  EXPECT_EQ(second_game.cleared_launch()->product, product(bundle));
  EXPECT_TRUE(both.rig.http.first_asking(
      "products/cloudgame?productId=EP0109-PPSA47001_00-FAKE0133TITLE000&"));
}

TEST_F(CloudStreamClearanceOfBundles, NeverLaunchesTheBundleItself) {
  for (const auto owned :
       {&owning_nothing, &owning_the_first_game, &owning_both_games}) {
    CloudStreamLaunch bundled;
    EXPECT_EQ(kind_of(bundled.clear((*owned)(), bundle)), Kind::choose_a_game);
    EXPECT_EQ(kind_of(bundled.held((*owned)(), bundle)), Kind::choose_a_game);
    EXPECT_EQ(bundled.rig.http.requests(), 0U);
    EXPECT_EQ(bundled.gk(), 0);
  }
}

TEST_F(CloudStreamClearanceOfBundles,
       OffersEachChildWhenEverySkuOfTheBundleIsOwned) {
  CloudStreamLaunch every_sku;
  every_sku.category = premium_with_an_empty_bundle_sku();
  const auto seen = every_sku.seen(owning_nothing());
  const auto* tile = seen.games->find(product(bundle));
  ASSERT_NE(tile, nullptr);
  EXPECT_TRUE(tile->is_bundle_owned());
  EXPECT_FALSE(tile->is_owned());
  EXPECT_FALSE(tile->ownership());
  EXPECT_EQ(kind_of(every_sku.clear(owning_nothing(), bundle)),
            Kind::choose_a_game);
  const auto unowned_child = every_sku.clear(owning_nothing(), first);
  EXPECT_EQ(kind_of(unowned_child), Kind::add_to_library);
  EXPECT_FALSE(unowned_child.cleared_launch());
  EXPECT_EQ(kind_of(every_sku.held(owning_nothing(), second)),
            Kind::add_to_library);
  EXPECT_EQ(every_sku.rig.http.count(ClearanceRig::ps5_record), 0);
  EXPECT_EQ(every_sku.rig.http.count(ClearanceRig::resolver), 0);
  EXPECT_EQ(every_sku.gk(), 0);
}

}  // namespace
}  // namespace catalog_test
