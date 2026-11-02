#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/model/launch_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string_view>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::HideReason;
using catalog::ListStreamability;
using catalog::Platform;
using catalog::PlayPath;
using catalog::PlusInclusion;
using catalog::ServiceGeneration;
using catalog::Standing;
using libchiaking::users::Tier;

class OldServiceLaunchPaths : public ::testing::Test, public LaunchCases {
 protected:
  static auto all_hidden_by_tier(const MyGames& games)
      -> testing::AssertionResult {
    for (const auto& game : games.hidden())
      if (game.visibility().hide_reason() != HideReason::tier_does_not_include)
        return testing::AssertionFailure()
               << game.product_id().text() << " is hidden for another reason";
    return testing::AssertionSuccess();
  }

  auto owned_games() const -> MyGames {
    return games_of(library_of({
                        {.id = god_of_war,
                         .product = god_of_war,
                         .name = "Storm of Ages III Remastered",
                         .platform = "ps4",
                         .package = "PS4GD"},
                        {.id = crow_ownership,
                         .product = crow,
                         .name = "Raven Hollow",
                         .platform = "ps4",
                         .package = "PS4GD"},
                    }),
                    list);
  }

  const catalog::StreamableList list = sample_list();
};

TEST_F(OldServiceLaunchPaths, StreamAPs4TitleFromTheShelf) {
  const auto games = games_of(library_of({}), list);
  const auto& x01 = game_of(games, god_of_war);
  EXPECT_EQ(x01.title().platform(), Platform::ps4);
  EXPECT_EQ(x01.title().service(), ServiceGeneration::old_service);
  EXPECT_EQ(x01.play_path(), PlayPath::old_service);
  EXPECT_TRUE(x01.is_visible());
  EXPECT_EQ(x01.standing(), Standing::included);
  EXPECT_FALSE(x01.needs_storefront_check());
  EXPECT_TRUE(launch(games, god_of_war).is_allowed());
}

TEST_F(OldServiceLaunchPaths, StreamAPs3TitleFromTheShelf) {
  const auto games = games_of(library_of({}), list);
  const auto& x02 = game_of(games, ps3_classic);
  EXPECT_EQ(x02.title().platform(), Platform::ps3);
  EXPECT_EQ(x02.play_path(), PlayPath::old_service);
  EXPECT_TRUE(x02.is_visible());
  EXPECT_EQ(x02.standing(), Standing::included);
  EXPECT_TRUE(launch(games, ps3_classic).is_allowed());
}

TEST_F(OldServiceLaunchPaths, IncludeAPs5ShelfTitleWithoutAddingIt) {
  const auto games = games_of(library_of({}), list);
  const auto& x08 = game_of(games, crow);
  EXPECT_EQ(x08.title().platform(), Platform::ps5);
  EXPECT_EQ(x08.title().service(), ServiceGeneration::old_service);
  EXPECT_EQ(x08.play_path(), PlayPath::included);
  EXPECT_EQ(x08.standing(), Standing::included);
  EXPECT_FALSE(x08.needs_library_add());
  EXPECT_FALSE(x08.needs_storefront_check());
  EXPECT_TRUE(launch(games, crow).is_allowed());
}

TEST_F(OldServiceLaunchPaths, StreamAnOwnedPs4TitleFromTheShelf) {
  const auto owned = owned_games();
  const auto& x03 = game_of(owned, god_of_war);
  EXPECT_TRUE(x03.is_owned());
  EXPECT_EQ(x03.play_path(), PlayPath::old_service);
  EXPECT_TRUE(x03.is_visible());
  EXPECT_EQ(x03.standing(), Standing::included);
  EXPECT_FALSE(x03.is_in_ps5_library());
  EXPECT_TRUE(launch(owned, god_of_war).is_allowed());
}

TEST_F(OldServiceLaunchPaths, KeepAnOwnedPs5ShelfTitleInTheLibrary) {
  const auto owned = owned_games();
  const auto& x07 = game_of(owned, crow);
  EXPECT_TRUE(x07.is_owned());
  EXPECT_EQ(x07.title().service(), ServiceGeneration::old_service);
  EXPECT_EQ(x07.play_path(), PlayPath::included);
  EXPECT_TRUE(x07.is_visible());
  EXPECT_EQ(x07.standing(), Standing::in_library);
  EXPECT_TRUE(x07.is_in_ps5_library());
  EXPECT_FALSE(x07.needs_storefront_check());
  EXPECT_TRUE(launch(owned, crow).is_allowed());
}

TEST_F(OldServiceLaunchPaths, HideEveryTitleWithoutPlusOrStreaming) {
  const auto without_plus = games_of(
      library_of({}), list, Membership{Tier::none}, streaming_refused());
  EXPECT_TRUE(without_plus.visible().empty());
  EXPECT_FALSE(without_plus.hidden().empty());
  EXPECT_TRUE(all_hidden_by_tier(without_plus));
}

TEST_F(OldServiceLaunchPaths, ShowTitlesToAMemberOfAnUnknownTier) {
  const auto x19 =
      games_of(library_of({}), list, Membership::member_of_unknown_tier());
  EXPECT_TRUE(game_of(x19, god_of_war).is_visible());
  EXPECT_TRUE(launch(x19, god_of_war).is_allowed());
}

TEST_F(OldServiceLaunchPaths, LetAMarkedTitleTryToStreamWithoutAStoreCheck) {
  const auto x28 =
      directly(god_of_war, Platform::ps4, ServiceGeneration::old_service,
               PlayPath::old_service, ListStreamability::marked_unstreamable);
  EXPECT_EQ(x28.standing(), Standing::may_stream);
  EXPECT_FALSE(x28.needs_storefront_check());
}

class Ps5CloudLaunchPaths : public ::testing::Test, public LaunchCases {
 protected:
  auto held() const -> catalog::OwnedPs5Games {
    return ps5_games({
        {.id = rivet, .product = rivet, .name = "Rivet & Bolt: Split Worlds"},
        {.id = show, .product = show, .name = "League Diamond 26"},
    });
  }

  const catalog::cloud_stream::PremiumCategory category = sample_premium();
  const catalog::OwnedPs5Games nothing = ps5_games({});
};

TEST_F(Ps5CloudLaunchPaths, AskToAddAnIncludedGameToTheLibrary) {
  const auto unowned = ps5_games_of(&nothing, &category);
  const auto& x04 = game_of(unowned, show);
  EXPECT_EQ(x04.play_path(), PlayPath::included);
  EXPECT_TRUE(x04.is_visible());
  EXPECT_EQ(x04.standing(), Standing::included);
  EXPECT_TRUE(x04.needs_library_add());
  EXPECT_TRUE(x04.can_be_added_to_library());
  EXPECT_EQ(x04.inclusion().source(), PlusInclusion::Source::store_tags);
  const auto add = launch(unowned, show);
  EXPECT_EQ(kind_of(add), Kind::add_to_library);
  ASSERT_TRUE(add.refusal());
  EXPECT_FALSE(add.refusal()->library_offer());
  ASSERT_TRUE(x04.library_offer());
  ASSERT_FALSE(x04.library_offer()->entitlement_ids.empty());
  EXPECT_EQ(x04.library_offer()->entitlement_ids.front(), show);
}

TEST_F(Ps5CloudLaunchPaths, RefuseAGameTheCategoryDoesNotList) {
  const auto unowned = ps5_games_of(&nothing, &category);
  EXPECT_EQ(unowned.find(product(bus)), nullptr);
  EXPECT_EQ(kind_of(launch(unowned, bus)), Kind::not_listed);
}

TEST_F(Ps5CloudLaunchPaths, AskToAddAnIncludedGameWhateverTheMembership) {
  for (const auto& membership :
       {Membership{}, Membership::member_of_unknown_tier(),
        Membership{Tier::premium}})
    EXPECT_EQ(
        kind_of(launch(
            ps5_games_of(&nothing, &category, std::nullopt, membership), show)),
        Kind::add_to_library);
}

TEST_F(Ps5CloudLaunchPaths, StreamAnOwnedGameAfterAStorefrontCheck) {
  const auto owned_copies = held();
  const auto owned = ps5_games_of(&owned_copies, &category);
  const auto& x05 = game_of(owned, rivet);
  EXPECT_EQ(x05.play_path(), PlayPath::owned);
  EXPECT_TRUE(x05.is_visible());
  EXPECT_EQ(x05.standing(), Standing::in_library);
  EXPECT_TRUE(x05.needs_storefront_check());
  EXPECT_FALSE(x05.needs_library_add());
  EXPECT_TRUE(launch(owned, rivet).is_allowed());
}

TEST_F(Ps5CloudLaunchPaths, StreamAnIncludedGameThatIsAlreadyOwned) {
  const auto owned_copies = held();
  const auto owned = ps5_games_of(&owned_copies, &category);
  const auto& x09 = game_of(owned, show);
  EXPECT_EQ(x09.play_path(), PlayPath::owned);
  EXPECT_TRUE(x09.inclusion().is_included());
  EXPECT_TRUE(x09.is_visible());
  EXPECT_EQ(x09.standing(), Standing::in_library);
  EXPECT_TRUE(launch(owned, show).is_allowed());
}

TEST_F(Ps5CloudLaunchPaths, HideEveryGameBelowPremiumWithoutStreaming) {
  const auto owned_copies = held();
  for (const auto tier : {Tier::none, Tier::essential, Tier::extra}) {
    const auto absent = ps5_games_of(&owned_copies, &category,
                                     streaming_refused(), Membership{tier});
    EXPECT_TRUE(absent.visible().empty());
    for (const auto id : {show, rivet})
      EXPECT_TRUE(hidden_by_tier(launch(absent, id))) << id;
  }
}

}  // namespace
}  // namespace catalog_test
