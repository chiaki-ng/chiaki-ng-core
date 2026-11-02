#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/model/cloud_stream/ps5_games_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string_view>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::LaunchRefusal;
using catalog::LaunchVerdict;
using catalog::PlayPath;

class Ps5GameBundles : public ::testing::Test, public Ps5GamesCases {
 protected:
  const catalog::cloud_stream::PremiumCategory category = premium();
};

TEST_F(Ps5GameBundles, ListTheirChildrenAndWhichOfThemAreOwned) {
  const auto one_held =
      ps5_games({{.id = game_one, .product = game_one, .name = "Game One"}});
  const auto games = ps5_games_of(&one_held, &category);
  const auto& row = game_of(games, bundle);
  ASSERT_EQ(row.children().size(), 2U);
  EXPECT_EQ(row.children()[0].entitlement, game_one);
  EXPECT_TRUE(row.children()[0].owned);
  EXPECT_EQ(row.children()[0].product, product(game_one));
  EXPECT_EQ(row.children()[1].entitlement, game_two);
  EXPECT_FALSE(row.children()[1].owned);
  EXPECT_FALSE(row.is_owned());
  EXPECT_EQ(row.play_path(), PlayPath::included);
  EXPECT_TRUE(row.needs_library_add());
  EXPECT_TRUE(row.is_bundle());
  EXPECT_FALSE(row.is_bundle_owned());
  EXPECT_EQ(row.launch_identifier().text(), game_one);
  EXPECT_EQ(row.title().cover_url(), "https://image/bundle.png");
  const auto told = games.explain(product(bundle));
  EXPECT_EQ(step_of(told, Rule::bundle_ownership).outcome, Outcome::noted);
  EXPECT_TRUE(has_step(told, Rule::joined_by_entitlement));
  EXPECT_EQ(count_on_ps5(games), 4U);
  EXPECT_EQ(LaunchVerdict::of(&row, std::nullopt).refusal()->kind(),
            LaunchRefusal::Kind::choose_a_game);
}

TEST_F(Ps5GameBundles, AreFoundThroughAnyOfTheirChildren) {
  const auto one_held =
      ps5_games({{.id = game_one, .product = game_one, .name = "Game One"}});
  const auto games = ps5_games_of(&one_held, &category);
  const auto& row = game_of(games, bundle);
  const auto* asked = games.find(product(game_two));
  ASSERT_EQ(asked, &row);
  const auto child = asked->child_known_as(product(game_two));
  ASSERT_TRUE(child);
  EXPECT_EQ(child->entitlement, game_two);
  EXPECT_FALSE(child->owned);
  EXPECT_EQ(games.find(product(game_one)), &row);
}

TEST_F(Ps5GameBundles, AreOwnedWhenEveryGameOfASkuIsHeld) {
  const auto both =
      ps5_games({{.id = game_two, .product = game_two, .name = "Game Two"},
                 {.id = game_one, .product = game_one, .name = "Game One"}});
  const auto owned_games = ps5_games_of(&both, &category);
  const auto& complete = game_of(owned_games, bundle);
  EXPECT_TRUE(complete.is_bundle_owned());
  EXPECT_FALSE(complete.is_owned());
  EXPECT_FALSE(complete.ownership());
  EXPECT_EQ(complete.play_path(), PlayPath::included);
  ASSERT_EQ(complete.children().size(), 2U);
  EXPECT_TRUE(complete.children()[0].owned);
  EXPECT_TRUE(complete.children()[1].owned);
  EXPECT_EQ(complete.launch_identifier().text(), game_one);
  const auto settled =
      step_of(owned_games.explain(product(bundle)), Rule::bundle_ownership);
  EXPECT_EQ(settled.outcome, Outcome::passed);
  EXPECT_FALSE(settled.ownership);
  EXPECT_EQ(LaunchVerdict::of(&complete, std::nullopt).refusal()->kind(),
            LaunchRefusal::Kind::choose_a_game);
}

TEST_F(Ps5GameBundles, CountAsOwnedThroughAnEmptySku) {
  auto empty_sku = two_game_bundle();
  empty_sku.skus.push_back(Sold{.sku = "EP0100-PPSA57001_00-BUNDLE-E002"});
  const auto with_empty_sku = category_of({empty_sku});
  const auto owned = ps5_games({});
  const auto games = ps5_games_of(&owned, &with_empty_sku);
  const auto& row = game_of(games, bundle);
  EXPECT_TRUE(row.is_bundle_owned());
  EXPECT_FALSE(row.is_owned());
  EXPECT_FALSE(row.ownership());
  EXPECT_EQ(row.play_path(), PlayPath::included);
  ASSERT_EQ(row.children().size(), 2U);
  EXPECT_FALSE(row.children()[0].owned);
  EXPECT_FALSE(row.children()[1].owned);
  EXPECT_EQ(
      step_of(games.explain(product(bundle)), Rule::bundle_ownership).outcome,
      Outcome::passed);
}

class Ps5GameTrialSkus : public ::testing::Test, public Ps5GamesCases {
 protected:
  static constexpr std::string_view arcane =
      "EP0104-PPSA07032_00-FAKE0055TITLE000";

  const catalog::cloud_stream::PremiumCategory category = category_of({Offered{
      .product = arcane,
      .name = "Arcane Legacy PS5 Version",
      .skus = {
          Sold{.sku = "EP0104-PPSA07032_00-FAKE0055TITLE000-E004",
               .entitlements =
                   {Granted{.id = arcane},
                    Granted{.id = "EP0104-PPSA07032_00-FAKE0121TITLE000",
                            .feature = Feature::unknown,
                            .package = "PSAL"}}},
          Sold{.sku = "EP0104-PPSA07032_00-FAKE0055TITLE000-E005",
               .branded = false,
               .entitlements = {
                   Granted{.id = arcane, .feature = Feature::play_first_trial},
                   Granted{.id = "EP0104-PPSA07032_00-FAKE0094TITLE000",
                           .feature = Feature::unknown,
                           .package = "PSTRACK"}}}}}});
};

TEST_F(Ps5GameTrialSkus, OwnAGameSoldWithATrialSkuThroughItsOffer) {
  const auto held =
      ps5_games({{.id = arcane, .product = arcane, .name = "Arcane Legacy"}});
  const auto games = ps5_games_of(&held, &category);
  const auto& row = game_of(games, arcane);
  EXPECT_TRUE(row.children().empty());
  EXPECT_TRUE(row.is_owned());
  EXPECT_EQ(row.play_path(), PlayPath::owned);
  EXPECT_FALSE(row.needs_library_add());
  EXPECT_EQ(row.launch_identifier().text(), arcane);
  EXPECT_TRUE(row.title().concept_page());
  EXPECT_EQ(count_on_ps5(games), 1U);
}

TEST_F(Ps5GameTrialSkus, OfferAnUnclaimedGameSoldWithATrialSku) {
  const auto unclaimed = ps5_games({});
  const auto offered = ps5_games_of(&unclaimed, &category);
  const auto& open = game_of(offered, arcane);
  EXPECT_FALSE(open.is_owned());
  EXPECT_TRUE(open.needs_library_add());
  EXPECT_TRUE(open.title().concept_page());
}

}  // namespace
}  // namespace catalog_test
