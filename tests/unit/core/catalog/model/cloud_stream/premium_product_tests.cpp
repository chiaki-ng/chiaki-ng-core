#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/ps/streaming_category_rig.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

namespace catalog_test {
namespace {

using libchiaking::catalog::OwnershipId;
using libchiaking::catalog::cloud_stream::PremiumEntitlement;
using Feature = PremiumEntitlement::Feature;

class PremiumProductOffer : public ::testing::Test, protected CategoryStore {};

TEST_F(PremiumProductOffer, IncludesABrandedSingleSkuFullGame) {
  const std::string a = "EP1-PPSA27002_00-";
  const auto game = offered(
      product(a + "GAME", {sku(a + "SKU", "PS_PLUS_STREAM_PS5",
                               {entitlement(a + "GAME", "FULL_GAME")})}));
  EXPECT_EQ(included_entitlement(game), a + "GAME");
  ASSERT_TRUE(game.included_offer());
  ASSERT_TRUE(game.included_offer()->sku);
  EXPECT_EQ(game.included_offer()->sku->text(), a + "SKU");
  EXPECT_FALSE(game.is_bundle());
}

TEST_F(PremiumProductOffer, LeavesOutABrandedSkuWhoseOnlyOfferIsATrial) {
  const std::string a = "EP1-PPSA27003_00-";
  const auto trial = offered(
      product(a + "GAME", {sku(a + "TRIAL", "PS_PLUS_STREAM_PS5",
                               {entitlement(a + "T", "TRIAL"),
                                entitlement(a + "P", "PLAY_FIRST_TRIAL"),
                                entitlement(a + "D", "DISC_REQUIRED")})}));
  EXPECT_FALSE(trial.included_offer());
  ASSERT_FALSE(trial.skus().empty());
  ASSERT_GE(trial.skus()[0].entitlements().size(), 2U);
  EXPECT_EQ(trial.skus()[0].entitlements()[1].facts().feature,
            Feature::play_first_trial);
}

TEST_F(PremiumProductOffer, AcceptsOnlyAPsgdEntitlementThatStreamsExactlyTrue) {
  const std::string a = "EP1-PPSA27004_00-";
  const auto game = offered(product(
      a + "GAME", {sku(a + "SKU", "PS_PLUS_STREAM_PS5",
                       {entitlement(a + "PS4", "FULL_GAME", "true", "PS4GD"),
                        entitlement(a + "UNSTATED", "FULL_GAME", ""),
                        entitlement(a + "TEXT", "FULL_GAME", "\"true\""),
                        entitlement(a + "NO", "FULL_GAME", "false"),
                        entitlement(a + "YES", "FULL_GAME"),
                        entitlement(a + "LATER", "FULL_GAME")})}));
  EXPECT_EQ(included_entitlement(game), a + "YES");
  ASSERT_FALSE(game.skus().empty());
  const auto& entitlements = game.skus()[0].entitlements();
  ASSERT_GE(entitlements.size(), 4U);
  EXPECT_FALSE(entitlements[1].facts().streaming_supported);
  EXPECT_FALSE(entitlements[2].facts().streaming_supported);
  EXPECT_EQ(entitlements[3].facts().streaming_supported, false);
}

TEST_F(PremiumProductOffer, DoesNotRetryABrandedSkuWithoutAnOfferOnAnotherSku) {
  const std::string a = "EP1-PPSA27005_00-", b = "EP1-PPSA27006_00-";
  EXPECT_FALSE(
      offered(
          product(a + "GAME",
                  {sku(a + "DISC", "PS_PLUS_STREAM_PS5",
                       {entitlement(a + "D", "DISC_REQUIRED")}),
                   unbranded(a + "FULL", {entitlement(a + "F", "FULL_GAME")})}))
          .included_offer());
  EXPECT_FALSE(
      offered(product(b + "GAME", {sku(b + "DISC", "PS_PLUS_STREAM_PS5",
                                       {entitlement(b + "D", "DISC_REQUIRED")}),
                                   sku(b + "FULL", "PS_PLUS_STREAM_PS5",
                                       {entitlement(b + "F", "FULL_GAME")})}))
          .included_offer());
}

TEST_F(PremiumProductOffer, SkipsSkusBrandedForDownloadOnly) {
  const std::string c = "EP1-PPSA27007_00-";
  const auto later =
      offered(product(c + "GAME", {sku(c + "DOWNLOAD", "PS_PLUS_DOWNLOAD",
                                       {entitlement(c + "X", "FULL_GAME")}),
                                   sku(c + "STREAM", "PS_PLUS_STREAM_PS5",
                                       {entitlement(c + "F", "FULL_GAME")})}));
  EXPECT_EQ(included_entitlement(later), c + "F");
}

TEST_F(PremiumProductOffer, LeavesOutAProductWithoutABrandedSku) {
  const std::string a = "EP1-PPSA27008_00-";
  EXPECT_FALSE(
      offered(
          product(a + "GAME", {sku(a + "SKU", "PS_PLUS_DOWNLOAD",
                                   {entitlement(a + "F", "FULL_GAME")}),
                               unbranded(a + "PLAIN",
                                         {entitlement(a + "G", "FULL_GAME")})}))
          .included_offer());
  EXPECT_FALSE(offered(product(a + "NONE", {})).included_offer());
}

TEST_F(PremiumProductOffer, IsABundleWithMoreThanOneStreamableFullGame) {
  const std::string a = "EP1-PPSA27009_00-", b = "EP1-PPSA27015_00-";
  EXPECT_TRUE(offered(product(a + "TWO",
                              {unbranded(a + "S1",
                                         {entitlement(a + "A", "FULL_GAME"),
                                          entitlement(b + "B", "FULL_GAME")})}))
                  .is_bundle());
  EXPECT_TRUE(
      offered(
          product(a + "SPLIT",
                  {unbranded(a + "S1", {entitlement(a + "A", "FULL_GAME")}),
                   unbranded(a + "S2", {entitlement(b + "B", "FULL_GAME")})}))
          .is_bundle());
}

TEST_F(PremiumProductOffer, IsNoBundleWithoutTwoStreamableFullGames) {
  const std::string a = "EP1-PPSA27009_00-", b = "EP1-PPSA27015_00-";
  EXPECT_FALSE(
      offered(
          product(a + "EDITIONS",
                  {sku(a + "S1", "PS_PLUS_STREAM_PS5",
                       {entitlement(a + "A", "FULL_GAME"),
                        entitlement(a + "ADDON", "", "", "PSAL")}),
                   unbranded(a + "S2",
                             {entitlement(a + "A", "PLAY_FIRST_TRIAL"),
                              entitlement(a + "TRACKER", "", "", "PSTRACK")})}))
          .is_bundle());
  EXPECT_FALSE(
      offered(product(a + "UNSTREAMED",
                      {unbranded(a + "S1", {entitlement(a + "A", "FULL_GAME"),
                                            entitlement(b + "B", "FULL_GAME",
                                                        "false")})}))
          .is_bundle());
  EXPECT_FALSE(
      offered(
          product(a + "ONE",
                  {unbranded(a + "S1", {entitlement(a + "A", "FULL_GAME")})}))
          .is_bundle());
  EXPECT_FALSE(
      offered(product(a + "EMPTY", {unbranded(a + "S1", {})})).is_bundle());
  EXPECT_FALSE(offered(product(a + "NOSKU", {})).is_bundle());
}

TEST_F(PremiumProductOffer, DeduplicatesStreamableChildrenAndKeepsTheirSku) {
  const std::string a = "EP1-PPSA27010_00-", b = "EP1-PPSA27011_00-";
  const auto bundle = offered(product(
      a + "BUNDLE",
      {unbranded(a + "S1", {entitlement(a + "ONE", "FULL_GAME"),
                            entitlement(a + "TRIAL", "TRIAL"),
                            entitlement(b + "TWO", "FULL_GAME", "false")}),
       sku(a + "S2", "PS_PLUS_STREAM_PS5",
           {entitlement(a + "ONE", "FULL_GAME"),
            entitlement(b + "TWO", "FULL_GAME"),
            entitlement("", "FULL_GAME")})}));
  const auto children = bundle.streamable_children();
  ASSERT_EQ(children.size(), 2U);
  EXPECT_EQ(children[0].entitlement.id(), id(a + "ONE"));
  ASSERT_TRUE(children[0].sku);
  EXPECT_EQ(children[0].sku->text(), a + "S1");
  EXPECT_EQ(children[1].entitlement.id(), id(b + "TWO"));
  ASSERT_TRUE(children[1].sku);
  EXPECT_EQ(children[1].sku->text(), a + "S2");
  EXPECT_EQ(included_entitlement(bundle), a + "ONE");
  ASSERT_TRUE(bundle.included_offer());
  ASSERT_TRUE(bundle.included_offer()->sku);
  EXPECT_EQ(bundle.included_offer()->sku->text(), a + "S2");
}

class PremiumProductOwnership : public ::testing::Test,
                                protected CategoryStore {};

TEST_F(PremiumProductOwnership, IsDecidedOnlyByGames) {
  const std::string a = "EP1-PPSA27016_00-", b = "EP1-PPSA27017_00-";
  const auto editions = offered(product(
      a + "GAME",
      {sku(a + "S1", "PS_PLUS_STREAM_PS5",
           {entitlement(a + "GAME", "FULL_GAME"),
            entitlement(a + "ADDON", "", "", "PSAL")}),
       unbranded(a + "S2", {entitlement(a + "GAME", "PLAY_FIRST_TRIAL"),
                            entitlement(a + "TRACKER", "", "", "PSTRACK")})}));
  EXPECT_TRUE(editions.is_owned_given({id(a + "GAME")}));
  EXPECT_FALSE(editions.is_owned_given({id(a + "ADDON")}));
  const auto trilogy = offered(product(
      a + "TRILOGY", {sku(a + "S1", "PS_PLUS_STREAM_PS5",
                          {entitlement(b + "PS4ONE", "FULL_GAME", "", "PS4GD"),
                           entitlement(a + "ONE", "FULL_GAME"),
                           entitlement(b + "TWO", "FULL_GAME")})}));
  EXPECT_TRUE(trilogy.is_owned_given({id(a + "ONE"), id(b + "TWO")}));
  EXPECT_FALSE(trilogy.is_owned_given({id(a + "ONE")}));
  const auto add_ons = offered(product(
      a + "ADDONS",
      {unbranded(a + "S1", {entitlement(a + "ADDON", "", "", "PSAL")})}));
  EXPECT_FALSE(add_ons.is_owned_given({}));
}

TEST_F(PremiumProductOwnership, NeedsOneSkuWithEveryEntitlementOwned) {
  const std::string a = "EP1-PPSA27012_00-";
  const auto bundle = offered(product(
      a + "BUNDLE", {unbranded(a + "S1", {entitlement(a + "ONE", "FULL_GAME"),
                                          entitlement(a + "TWO", "FULL_GAME")}),
                     unbranded(a + "S2", {entitlement(a + "THREE", "FULL_GAME"),
                                          entitlement("", "FULL_GAME")})}));
  EXPECT_TRUE(bundle.is_owned_given({id(a + "ONE"), id(a + "TWO")}));
  EXPECT_TRUE(bundle.is_owned_given({id(a + "THREE")}));
  EXPECT_FALSE(bundle.is_owned_given({id(a + "ONE"), id(a + "FOUR")}));
  EXPECT_FALSE(bundle.is_owned_given({}));
}

TEST_F(PremiumProductOwnership, LeavesASkuIncompleteWithAnInactiveEntitlement) {
  const std::string a = "EP1-PPSA27013_00-";
  const auto bundle = offered(
      product(a + "BUNDLE",
              {unbranded(a + "S1", {entitlement(a + "ONE", "FULL_GAME"),
                                    entitlement(a + "TWO", "FULL_GAME")})}));
  struct Held {
    OwnershipId id;
    bool active;
  };
  const std::vector<Held> library{{id(a + "ONE"), true},
                                  {id(a + "TWO"), false}};
  std::set<OwnershipId> owned_and_active;
  for (const auto& held : library)
    if (held.active) owned_and_active.insert(held.id);
  EXPECT_FALSE(bundle.is_owned_given(owned_and_active));
  owned_and_active.insert(id(a + "TWO"));
  EXPECT_TRUE(bundle.is_owned_given(owned_and_active));
}

TEST_F(PremiumProductOwnership, CountsASkuWithoutEntitlementIdsAsOwned) {
  const std::string a = "EP1-PPSA27014_00-";
  EXPECT_TRUE(offered(product(a + "EMPTY",
                              {unbranded(a + "S1",
                                         {entitlement(a + "ONE", "FULL_GAME")}),
                               unbranded(a + "S2", {})}))
                  .is_owned_given({}));
  EXPECT_TRUE(
      offered(product(a + "FALSY",
                      {unbranded(a + "S1", {entitlement("", "FULL_GAME"),
                                            R"({"packageType":"PSGD"})"})}))
          .is_owned_given({}));
}

TEST_F(PremiumProductOwnership, IsNeverGivenWithoutAReadableSku) {
  const std::string a = "EP1-PPSA27014_00-";
  EXPECT_FALSE(
      offered(product(a + "NOSKU", {})).is_owned_given({id(a + "ONE")}));
  EXPECT_FALSE(
      offered(product(a + "UNREADABLE",
                      {unbranded(a + "S1", {entitlement("an id with spaces",
                                                        "FULL_GAME")})}))
          .is_owned_given({}));
}

class PremiumProductSale : public ::testing::Test, protected CategoryStore {};

TEST_F(PremiumProductSale, OffersAnUnbrandedStreamableFullGame) {
  const auto unbranded_game = offered(product(
      "EP1-PPSA27002_00-GAME",
      {unbranded("EP1-PPSA27002_00-SKU",
                 {entitlement("EP1-PPSA27002_00-TRIAL", "TRIAL"),
                  entitlement("EP1-PPSA27002_00-GAME", "FULL_GAME")})}));
  const auto sold = unbranded_game.offer_for_sale();
  ASSERT_TRUE(sold);
  EXPECT_EQ(sold->entitlement.id(), id("EP1-PPSA27002_00-GAME"));
  EXPECT_FALSE(unbranded_game.included_offer());
}

TEST_F(PremiumProductSale, OffersNothingButAStreamableFullGame) {
  for (const auto& listed :
       {product("EP1-PPSA27003_00-GAME",
                {unbranded("EP1-PPSA27003_00-SKU",
                           {entitlement("EP1-PPSA27003_00-GAME", "TRIAL")})}),
        product("EP1-PPSA27004_00-GAME",
                {unbranded("EP1-PPSA27004_00-SKU",
                           {entitlement("EP1-PPSA27004_00-GAME", "FULL_GAME",
                                        "false")})}),
        product("EP1-PPSA27005_00-GAME",
                {unbranded(
                    "EP1-PPSA27005_00-SKU",
                    {entitlement("EP1-PPSA27005_00-GAME", "FULL_GAME", "")})}),
        product("EP1-PPSA27006_00-GAME",
                {unbranded("EP1-PPSA27006_00-SKU",
                           {entitlement("EP1-PPSA27006_00-GAME", "FULL_GAME",
                                        "true", "PSAC")})})})
    EXPECT_FALSE(offered(listed).offer_for_sale()) << listed;
}

TEST_F(PremiumProductSale, OffersAPlayFirstTrialTheUserDoesNotHold) {
  const std::string game = "UP0113-PPSA37006_00-FAKE0126TITLE000";
  const auto play_first = offered(product(
      game, {unbranded(game + "-U001", {entitlement(game, "FULL_GAME")}),
             unbranded(game + "-U003",
                       {entitlement("UP0113-PPSA37006_00-FAKE0094TITLE000",
                                    "unknown", "", "PSTRACK"),
                        entitlement(game, "PLAY_FIRST_TRIAL")})}));
  EXPECT_TRUE(play_first.offers(Feature::play_first_trial, {}));
  EXPECT_FALSE(play_first.offers(Feature::trial, {}));
  EXPECT_FALSE(play_first.offers(Feature::play_first_trial, {id(game)}));
}

TEST_F(PremiumProductSale, OffersADemoTheUserDoesNotHold) {
  const auto demo = offered(
      product("EP1-PPSA27011_00-GAME",
              {unbranded("EP1-PPSA27011_00-SKU",
                         {entitlement("EP1-PPSA27011_00-GAME", "FULL_GAME"),
                          entitlement("EP1-PPSA27011_00-DEMO", "TRIAL")})}));
  EXPECT_TRUE(demo.offers(Feature::trial, {}));
  EXPECT_FALSE(demo.offers(Feature::trial, {id("EP1-PPSA27011_00-DEMO")}));
}

TEST_F(PremiumProductSale, OffersNoTrialThatCannotStream) {
  for (const auto& listed :
       {product("EP1-PPSA27012_00-GAME",
                {unbranded(
                    "EP1-PPSA27012_00-SKU",
                    {entitlement("EP1-PPSA27012_00-GAME", "FULL_GAME"),
                     entitlement("EP1-PPSA27012_00-DEMO", "TRIAL", "false")})}),
        product("EP1-PPSA27013_00-GAME",
                {unbranded("EP1-PPSA27013_00-SKU",
                           {entitlement("EP1-PPSA27013_00-GAME", "FULL_GAME"),
                            entitlement("EP1-PPSA27013_00-DEMO", "TRIAL",
                                        "true", "PSAC")})}),
        full_game(12)})
    EXPECT_FALSE(offered(listed).offers(Feature::trial, {})) << listed;
}

TEST(PremiumEntitlementFeature, IsNamedAsTheStoreSpellsIt) {
  EXPECT_EQ(PremiumEntitlement::feature_named("FULL_GAME"), Feature::full_game);
  EXPECT_EQ(PremiumEntitlement::feature_named("TRIAL"), Feature::trial);
  EXPECT_EQ(PremiumEntitlement::feature_named("PLAY_FIRST_TRIAL"),
            Feature::play_first_trial);
  EXPECT_EQ(PremiumEntitlement::feature_named("DISC_REQUIRED"),
            Feature::disc_required);
  EXPECT_EQ(PremiumEntitlement::feature_named("full_game"), Feature::unknown);
  EXPECT_EQ(PremiumEntitlement::feature_named(""), Feature::unknown);
}

TEST(PremiumEntitlementFeature, RoundTripsThroughItsName) {
  for (const auto feature :
       {Feature::full_game, Feature::trial, Feature::play_first_trial,
        Feature::disc_required, Feature::unknown})
    EXPECT_EQ(
        PremiumEntitlement::feature_named(PremiumEntitlement::name_of(feature)),
        feature);
}

}  // namespace
}  // namespace catalog_test
