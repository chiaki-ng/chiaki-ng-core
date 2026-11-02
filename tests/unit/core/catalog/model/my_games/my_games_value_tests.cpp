#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <optional>

namespace catalog_test {
namespace {

using libchiaking::catalog::HideReason;
using libchiaking::catalog::MyGames;
using libchiaking::users::Tier;

class MyGamesOfAUser : public ValueCases {
 protected:
  MyGame rift = owned_rivet();
  MyGame hidden =
      game(title(show, "League Diamond 26", Platform::ps5,
                 ServiceGeneration::ps5_cloud),
           PlusInclusion::with_premium(Source::store_tags), PlayPath::included,
           std::nullopt, Visibility::hidden(HideReason::tier_does_not_include))
          .value();
  MyGame classic = game(title(god_of_war, "Storm of Ages III Remastered",
                              Platform::ps4, ServiceGeneration::old_service),
                        PlusInclusion::with_premium(Source::old_service),
                        PlayPath::old_service)
                       .value();
  MyGames::Inputs inputs{at_ms(1000), at_ms(2000), at_ms(3000), Tier::premium,
                         4};
};

TEST_F(MyGamesOfAUser, AreTiedToTheUserAndTheirInputs) {
  const auto built = MyGames::of(account, {rift, hidden, classic}, inputs);
  ASSERT_TRUE(built);
  EXPECT_EQ(built->account(), account);
  EXPECT_EQ(built->inputs(), inputs);
  EXPECT_TRUE(built->was_computed_from(inputs));
  auto changed = inputs;
  changed.tier = Tier::extra;
  EXPECT_FALSE(built->was_computed_from(changed));
}

TEST_F(MyGamesOfAUser, SplitVisibleFromHiddenInOrder) {
  const auto built = MyGames::of(account, {rift, hidden, classic}, inputs);
  ASSERT_TRUE(built);
  EXPECT_EQ(built->size(), 3U);
  ASSERT_EQ(built->visible().size(), 2U);
  ASSERT_EQ(built->hidden().size(), 1U);
  EXPECT_EQ(built->visible()[0], rift);
  EXPECT_EQ(built->visible()[1], classic);
  EXPECT_EQ(built->hidden()[0], hidden);
}

TEST_F(MyGamesOfAUser, FindHiddenGamesToo) {
  const auto built = MyGames::of(account, {rift, hidden, classic}, inputs);
  ASSERT_TRUE(built);
  const auto* found = built->find(product(show));
  ASSERT_NE(found, nullptr);
  EXPECT_FALSE(found->is_visible());
  EXPECT_EQ(built->find(product(bus)), nullptr);
}

TEST_F(MyGamesOfAUser, RefuseAProductTwice) {
  EXPECT_EQ(MyGames::of(account, {rift, classic, rift}, inputs).error(),
            MyGames::Invalid::duplicate_product);
}

TEST_F(MyGamesOfAUser, MayBeEmpty) {
  const auto none = MyGames::of(account, {}, inputs);
  ASSERT_TRUE(none);
  EXPECT_TRUE(none.value().visible().empty());
  EXPECT_TRUE(none.value().hidden().empty());
}

}  // namespace
}  // namespace catalog_test
