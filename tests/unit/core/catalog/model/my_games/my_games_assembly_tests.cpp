#include "src/core/catalog/model/legacy_cloud_stream/games.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/model/my_games/my_games_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::HideReason;
using catalog::LaunchVerdict;
using catalog::Platform;
using catalog::PlayPath;
using catalog::ServiceGeneration;
using catalog::Standing;
using libchiaking::users::Membership;
using libchiaking::users::Tier;

class MyGamesAssembly : public ::testing::Test, public MyGamesCases {
 protected:
  static auto owned_count(const MyGames& games) -> std::size_t {
    const auto owned = [](const MyGame& game) { return game.is_owned(); };
    return static_cast<std::size_t>(
               std::ranges::count_if(games.visible(), owned)) +
           static_cast<std::size_t>(
               std::ranges::count_if(games.hidden(), owned));
  }

  static auto visible_in_ps5_library(const MyGames& games) -> std::size_t {
    return static_cast<std::size_t>(
        std::ranges::count_if(games.visible(), &MyGame::is_in_ps5_library));
  }

  static auto all_unrestricted(const MyGames& games)
      -> testing::AssertionResult {
    for (const auto& game : games.visible())
      if (!game.release().is_unrestricted())
        return testing::AssertionFailure()
               << game.product_id().text() << " has a release date";
    return testing::AssertionSuccess();
  }

  static auto all_on_the_old_service(const MyGames& games)
      -> testing::AssertionResult {
    for (const auto shelf : {games.visible(), games.hidden()})
      for (const auto& game : shelf)
        if (game.title().service() != ServiceGeneration::old_service)
          return testing::AssertionFailure()
                 << game.product_id().text() << " is not on the old service";
    return testing::AssertionSuccess();
  }

  static auto all_hidden_by(const MyGames& games, Tier tier)
      -> testing::AssertionResult {
    for (const auto& game : games.hidden()) {
      const auto id = game.product_id().text();
      if (game.visibility().hide_reason() != HideReason::tier_does_not_include)
        return testing::AssertionFailure()
               << id << " is hidden for another reason";
      const auto explanation = games.explain(game.product_id());
      if (failed_rule(explanation) != Rule::tier)
        return testing::AssertionFailure() << id << " did not fail on the tier";
      if (explanation.first_failure()->tier != tier)
        return testing::AssertionFailure() << id << " failed on another tier";
    }
    return testing::AssertionSuccess();
  }

  static auto with_sources(
      const catalog::Library& library, const catalog::StreamableList& list,
      const catalog::OwnedPs5Games& owned,
      const catalog::cloud_stream::PremiumCategory& category,
      const Membership& membership,
      const std::optional<libchiaking::users::CloudStreaming>& streaming)
      -> MyGames {
    return catalog::MyGamesAssembly::games_of({.library = library,
                                               .list = &list,
                                               .ps5 = &owned,
                                               .category = &category},
                                              membership, streaming, {}, 0,
                                              at_ms(3000));
  }

  static auto builds_legacy_rows(const catalog::Library& library,
                                 const catalog::StreamableList& list)
      -> testing::AssertionResult {
    const auto category = category_of({included(plus_pick, "Plus Pick"),
                                       included(show, "League Diamond 26")});
    const auto owned = ps5_games(
        {{.id = odyssey, .product = odyssey, .name = "Synthetic Odyssey"}});
    const auto games = catalog::MyGamesAssembly::games_of(
        {.library = library,
         .list = &list,
         .ps5 = &owned,
         .category = &category},
        {}, streaming_allowed(), {}, 0, at_ms(3000));
    const catalog::RememberedRefusals remembered{
        account, {}, std::nullopt, at_ms(3000)};
    const auto built =
        catalog::legacy_cloud_stream::Games{library, list, games.inputs(),
                                            remembered}
            .listed();
    if (built.listed.empty())
      return testing::AssertionFailure() << "legacy cloud stream built nothing";
    for (const auto& row : built.listed) {
      const auto id = row.game.product_id().text();
      const auto* found = games.find(row.game.product_id());
      if (found == nullptr)
        return testing::AssertionFailure() << id << " is missing";
      if (*found != row.game)
        return testing::AssertionFailure() << id << " is another row";
      if (games.explain(row.game.product_id()) != row.explanation)
        return testing::AssertionFailure() << id << " is explained otherwise";
    }
    if (games.size() != built.listed.size() + 3)
      return testing::AssertionFailure()
             << games.size() << " games for " << built.listed.size()
             << " legacy rows";
    return testing::AssertionSuccess();
  }
};

TEST_F(MyGamesAssembly, ListsATitleOnBothListsOnceOnTheOldService) {
  const auto list = list_of({
      on_old_service("PPSA-CROW_00", "Raven Hollow", "111"),
      {.product = "PPSA-CROW_00", .name = "Raven Hollow", .concept_id = "111"},
  });
  const auto games = games_of(library_of({}), list);
  EXPECT_EQ(games.size(), 1U);
  const auto& crow = game_of(games, "PPSA-CROW_00");
  EXPECT_EQ(crow.title().service(), ServiceGeneration::old_service);
  EXPECT_TRUE(crow.plays_through_subscription());
  EXPECT_EQ(crow.play_path(), PlayPath::included);
  EXPECT_EQ(crow.launch_identifier().text(), "PPSA-CROW_00");
}

TEST_F(MyGamesAssembly, PutsOwnedGamesFirstThenSortsByName) {
  const auto category =
      category_of({included("EP-PPSA00002_00-APPLE", "apple"),
                   included("EP-PPSA00003_00-BANANA", "Banana")});
  const auto owned = ps5_games({{.id = "EP-PPSA00001_00-ZEBRA",
                                 .product = "EP-PPSA00001_00-ZEBRA",
                                 .name = "Zebra"}});
  const auto list =
      list_of({on_old_service("EP-CUSA07001_00-AARDVARK", "Aardvark")});
  const auto games = ps5_games_of(&owned, &category, std::nullopt, {}, &list);
  EXPECT_EQ(games.account(), account);
  ASSERT_EQ(games.size(), 4U);
  EXPECT_TRUE(games.hidden().empty());
  EXPECT_EQ(games.inputs().list_fetched_at, at_ms(1000));
  EXPECT_EQ(games.inputs().library_fetched_at, at_ms(library_fetched_ms));
  EXPECT_EQ(games.inputs().included_fetched_at, at_ms(1500));
  EXPECT_FALSE(games.inputs().tier);
  ASSERT_EQ(games.visible().size(), 4U);
  EXPECT_TRUE(games.visible()[0].is_owned());
  EXPECT_EQ(games.visible()[0].title().name(), "Zebra");
  EXPECT_EQ(games.visible()[1].title().name(), "Aardvark");
  EXPECT_EQ(games.visible()[2].title().name(), "apple");
  EXPECT_EQ(games.visible()[3].title().name(), "Banana");
  EXPECT_EQ(visible_in_ps5_library(games), 1U);
  EXPECT_TRUE(all_unrestricted(games));
  EXPECT_EQ(games.visible()[0].play_path(), PlayPath::owned);
  EXPECT_EQ(games.visible()[3].play_path(), PlayPath::included);
}

TEST_F(MyGamesAssembly, LeavesOutABundleOnlyARelatedListEntryCarries) {
  const auto list = list_of({{.product = "EP-PPSA57007_00-PART",
                              .name = "Bundle Part",
                              .concept_id = "71"}});
  const auto library = library_of({
      {.id = "EP-CUSA77001_00-BUNDLE",
       .product = "EP-CUSA77001_00-BUNDLE",
       .name = "Mega Bundle",
       .platform = "",
       .package = ""},
      {.id = "EP-PPSA57007_00-PART",
       .product = "EP-CUSA77001_00-BUNDLE",
       .name = "Bundle Part",
       .active = false},
      {.id = "EP-PPSA57007_00-PART",
       .product = "EP-CUSA77001_00-BUNDLE",
       .name = "Bundle Part",
       .active = false},
  });
  EXPECT_EQ(library.size(), 1U);
  EXPECT_EQ(library.siblings_of(product("EP-CUSA77001_00-BUNDLE")).size(), 2U);
  const auto games = games_of(library, list);
  EXPECT_EQ(owned_count(games), 0U);
  EXPECT_EQ(games.find(product("EP-CUSA77001_00-BUNDLE")), nullptr);
  EXPECT_EQ(failed_rule(games.explain(product("EP-CUSA77001_00-BUNDLE"))),
            Rule::listed_by_service);
}

TEST_F(MyGamesAssembly, NamesEveryRuleThatDropsALibraryRecord) {
  const auto list = sample_list();
  const auto library = sample_library();
  EXPECT_EQ(library.size(), 6U);
  const auto* held = library.find(*catalog::OwnershipId::parse(odyssey));
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held->product_id(), product(odyssey));
  const auto games = games_of(library, list);
  EXPECT_EQ(games.size(), 1U);
  EXPECT_TRUE(game_of(games, classic).is_visible());
  EXPECT_EQ(failed_rule(games.explain(product(no_match))), Rule::library_match);
  EXPECT_EQ(failed_rule(games.explain(product(supplement))),
            Rule::listed_by_service);
  EXPECT_EQ(games.find(product(supplement)), nullptr);
  EXPECT_EQ(games.find(product(no_match)), nullptr);
  const auto wrapper = games.explain(product(cross_buy));
  EXPECT_EQ(failed_rule(wrapper), Rule::service);
  EXPECT_EQ(wrapper.first_failure()->ownership,
            *catalog::OwnershipId::parse(cross_buy));
  EXPECT_EQ(games.find(product(cross_buy)), nullptr);
  EXPECT_FALSE(games.explain(product("UP0114-PPSA07044_00-FAKE0127TITLE000"))
                   .is_listed());
}

TEST_F(MyGamesAssembly, DropsARecordThePublicStoreListDoesNotStream) {
  const auto public_store = games_of(
      sample_library(), list_of(
                            {
                                on_old_service(classic, "Classic Quest"),
                                {.product = supplement,
                                 .name = "Supplement Only",
                                 .ps5 = false,
                                 .ps4 = true,
                                 .streams = std::nullopt},
                                {.product = supplement,
                                 .name = "Supplement Only",
                                 .shelf = Shelf::plus_games,
                                 .ps5 = false,
                                 .ps4 = true,
                                 .streams = std::nullopt},
                            },
                            catalog::ListOrigin::public_store));
  EXPECT_EQ(public_store.find(product(supplement)), nullptr);
  EXPECT_EQ(failed_rule(public_store.explain(product(supplement))),
            Rule::listed_by_service);
}

TEST_F(MyGamesAssembly, FollowsPlatformInclusionAndOwnershipToAPath) {
  const auto list = sample_list();
  const auto games = games_of(sample_library(), list);
  EXPECT_TRUE(every_game_agrees_with_its_explanation(games));
  const auto& quest = game_of(games, classic);
  EXPECT_EQ(quest.play_path(), PlayPath::old_service);
  EXPECT_TRUE(quest.is_visible());
  EXPECT_TRUE(quest.is_owned());
  EXPECT_TRUE(quest.release().is_unrestricted());
  EXPECT_EQ(quest.standing(), Standing::included);
  EXPECT_TRUE(all_on_the_old_service(games));
  const auto after = games_of(sample_library(true), list);
  EXPECT_TRUE(every_game_agrees_with_its_explanation(after));
  EXPECT_EQ(game_of(after, classic), quest);
}

TEST_F(MyGamesAssembly, ShowsNothingToAKnownTierBelowPremium) {
  const auto list = sample_list();
  const auto library = sample_library(true);
  const auto category = category_of({included(plus_pick, "Plus Pick")});
  const auto owned = ps5_games(
      {{.id = odyssey, .product = odyssey, .name = "Synthetic Odyssey"}});
  const auto alone = games_of(library, list);
  for (const auto tier : {Tier::none, Tier::essential, Tier::extra}) {
    const auto games = with_sources(library, list, owned, category,
                                    Membership{tier}, streaming_refused());
    EXPECT_TRUE(every_game_agrees_with_its_explanation(games));
    EXPECT_TRUE(games.visible().empty());
    EXPECT_EQ(games.size(), alone.size() + 2);
    EXPECT_EQ(games.inputs().tier, tier);
    EXPECT_TRUE(all_hidden_by(games, tier));
    EXPECT_TRUE(game_of(games, odyssey).is_owned());
  }
}

TEST_F(MyGamesAssembly, ShowsEverythingToPremiumOrAnUnknownTier) {
  const auto list = sample_list();
  const auto library = sample_library(true);
  const auto category = category_of({included(plus_pick, "Plus Pick")});
  const auto owned = ps5_games(
      {{.id = odyssey, .product = odyssey, .name = "Synthetic Odyssey"}});
  const auto alone = games_of(library, list);
  for (const auto& membership :
       {Membership{}, Membership::member_of_unknown_tier(),
        Membership{Tier::premium}}) {
    const auto games =
        with_sources(library, list, owned, category, membership, std::nullopt);
    EXPECT_TRUE(every_game_agrees_with_its_explanation(games));
    EXPECT_EQ(games.visible().size(), alone.visible().size() + 2);
    EXPECT_TRUE(game_of(games, plus_pick).is_visible());
    EXPECT_TRUE(game_of(games, odyssey).is_visible());
  }
}

TEST_F(MyGamesAssembly,
       StreamsAnOwnedPs4TitleThroughTheOldServiceWithoutItBeingIncluded) {
  const auto list = list_of({
      on_old_service(god_of_war, "Storm of Ages III Remastered"),
      {.product = old_listing,
       .name = "Old Listing",
       .ps5 = false,
       .ps4 = true},
      {.product = show, .name = "League Diamond 26"},
  });
  const auto unowned = games_of(library_of({}), list);
  EXPECT_EQ(unowned.find(product(old_listing)), nullptr);
  const auto library = library_of({{.id = old_listing,
                                    .product = old_listing,
                                    .name = "Old Listing",
                                    .platform = "ps4",
                                    .package = "PS4GD"}});
  const auto games = games_of(library, list);
  EXPECT_TRUE(every_game_agrees_with_its_explanation(games));
  const auto& owned = game_of(games, old_listing);
  EXPECT_TRUE(owned.is_visible());
  EXPECT_TRUE(owned.is_owned());
  EXPECT_EQ(owned.title().service(), ServiceGeneration::old_service);
  EXPECT_EQ(owned.title().platform(), Platform::ps4);
  EXPECT_FALSE(owned.inclusion().is_included());
  EXPECT_EQ(owned.play_path(), PlayPath::old_service);
  EXPECT_EQ(owned.standing(), Standing::included);
  EXPECT_FALSE(owned.is_in_ps5_library());
  EXPECT_FALSE(owned.needs_library_add());
  EXPECT_FALSE(owned.needs_storefront_check());
  EXPECT_FALSE(owned.can_be_added_to_library());
  EXPECT_EQ(owned.launch_identifier().text(), old_listing);
  EXPECT_TRUE(LaunchVerdict::of(&owned, std::nullopt).is_allowed());
  EXPECT_EQ(games.find(product(show)), nullptr);
  const auto& classic_title = game_of(games, god_of_war);
  EXPECT_EQ(classic_title.title().service(), ServiceGeneration::old_service);
  EXPECT_FALSE(classic_title.is_owned());
  EXPECT_TRUE(classic_title.inclusion().is_included());
  EXPECT_FALSE(classic_title.needs_library_add());
  EXPECT_TRUE(LaunchVerdict::of(&classic_title, std::nullopt).is_allowed());
}

class MyGamesLegacyCloudStreamTitles : public ::testing::Test,
                                       public MyGamesCases {
 protected:
  static constexpr std::string_view syndicate =
      "EP0102-CUSA17008_00-FAKE0117TITLE000";
  static constexpr std::string_view syndicate_ps5 =
      "EP0102-PPSA01234_00-FAKE0117TITLE000";

  const catalog::StreamableList list =
      list_of({on_old_service(syndicate, "Syndicate: Rising Tiger")});
};

TEST_F(MyGamesLegacyCloudStreamTitles, AreNotTakenOverByARecordTaggedPs5) {
  const auto tagged_ps5 =
      games_of(library_of({{.id = syndicate,
                            .product = syndicate,
                            .name = "Syndicate: Rising Tiger",
                            .package = "PSGD"}}),
               list);
  EXPECT_TRUE(every_game_agrees_with_its_explanation(tagged_ps5));
  EXPECT_EQ(tagged_ps5.size(), 1U);
  const auto& streamed = game_of(tagged_ps5, syndicate);
  EXPECT_EQ(streamed.title().service(), ServiceGeneration::old_service);
  EXPECT_EQ(streamed.title().platform(), Platform::ps4);
  EXPECT_FALSE(streamed.is_owned());
  EXPECT_TRUE(streamed.is_visible());
  EXPECT_EQ(streamed.play_path(), PlayPath::old_service);
  EXPECT_EQ(streamed.launch_identifier().text(), syndicate);
  EXPECT_TRUE(LaunchVerdict::of(&streamed, std::nullopt).is_allowed());
}

TEST_F(MyGamesLegacyCloudStreamTitles, StayBesideAnOwnedPs5Version) {
  const auto owned_on_ps5 = ps5_games({{.id = syndicate_ps5,
                                        .product = syndicate_ps5,
                                        .name = "Syndicate: Rising Tiger"}});
  const auto native =
      ps5_games_of(&owned_on_ps5, nullptr, std::nullopt, {}, &list);
  EXPECT_TRUE(every_game_agrees_with_its_explanation(native));
  EXPECT_EQ(native.size(), 2U);
  EXPECT_TRUE(native.hidden().empty());
  const auto& on_legacy_cloud_stream = game_of(native, syndicate);
  EXPECT_EQ(on_legacy_cloud_stream.title().service(),
            ServiceGeneration::old_service);
  EXPECT_FALSE(on_legacy_cloud_stream.is_owned());
  EXPECT_EQ(on_legacy_cloud_stream.launch_identifier().text(), syndicate);
  const auto& on_cloud_stream = game_of(native, syndicate_ps5);
  EXPECT_EQ(on_cloud_stream.title().service(), ServiceGeneration::ps5_cloud);
  EXPECT_EQ(on_cloud_stream.title().platform(), Platform::ps5);
  EXPECT_TRUE(on_cloud_stream.is_owned());
  EXPECT_EQ(on_cloud_stream.play_path(), PlayPath::owned);
  EXPECT_EQ(on_cloud_stream.launch_identifier().text(), syndicate_ps5);
}

TEST_F(MyGamesLegacyCloudStreamTitles, AreLicensedByAHeldStreamingCopy) {
  const auto held_copy =
      games_of(library_of({{.id = "EP0102-CUSA17008_00-FAKE0107TITLE000",
                            .product = syndicate,
                            .name = "Syndicate: Rising Tiger",
                            .platform = "ps4",
                            .package = "PS4GS"}}),
               list);
  const auto& licensed = game_of(held_copy, syndicate);
  EXPECT_TRUE(licensed.is_owned());
  EXPECT_EQ(licensed.title().service(), ServiceGeneration::old_service);
  ASSERT_TRUE(licensed.ownership());
  EXPECT_EQ(licensed.ownership()->id(),
            std::string_view{"EP0102-CUSA17008_00-FAKE0107TITLE000"});
  EXPECT_EQ(licensed.launch_identifier().text(), syndicate);
}

TEST_F(MyGamesLegacyCloudStreamTitles,
       StayOnTheShelfWhenTheStoreAlsoListsThem) {
  constexpr std::string_view on_shelf = "UP0114-CUSA07039_00-FAKE0105TITLE000";
  constexpr std::string_view in_store = "UP0114-CUSA07039_00-FAKE0122TITLE000";
  const auto shelf_and_store = list_of({
      on_old_service(on_shelf, "Shelf Game"),
      {.product = in_store, .name = "Shelf Game", .ps5 = false, .ps4 = true},
  });
  const auto games =
      games_of(library_of({{.id = "UP0114-CUSA07039_00-FAKE0103TITLE000",
                            .product = on_shelf,
                            .name = "Shelf Game",
                            .package = "PSGD"}}),
               shelf_and_store);
  EXPECT_TRUE(every_game_agrees_with_its_explanation(games));
  EXPECT_EQ(games.size(), 1U);
  const auto& kept = game_of(games, on_shelf);
  EXPECT_EQ(kept.title().service(), ServiceGeneration::old_service);
  EXPECT_FALSE(kept.is_owned());
  EXPECT_TRUE(kept.is_visible());
  EXPECT_EQ(kept.launch_identifier().text(), on_shelf);
}

TEST_F(MyGamesAssembly, ExplainsNothingItWasNotToldWhenRebuilt) {
  const auto owned = ps5_games(
      {{.id = odyssey, .product = odyssey, .name = "Synthetic Odyssey"}});
  const auto list = sample_list();
  const auto games = ps5_games_of(&owned, nullptr, std::nullopt, {}, &list);
  const auto rebuilt = MyGames::of(
      games.account(),
      std::vector<MyGame>{games.visible().begin(), games.visible().end()},
      games.inputs());
  ASSERT_TRUE(rebuilt);
  EXPECT_TRUE(rebuilt.value().explain(product(odyssey)).is_listed());
  EXPECT_TRUE(rebuilt.value().explain(product(odyssey)).steps().empty());
  EXPECT_FALSE(rebuilt.value().explain(product(no_match)).is_listed());
  const auto explained = MyGames::of(
      games.account(),
      std::vector<MyGame>{games.visible().begin(), games.visible().end()},
      games.inputs(),
      {games.explain(product(odyssey)), games.explain(product(no_match))});
  ASSERT_TRUE(explained);
  EXPECT_EQ(explained.value().explain(product(odyssey)),
            games.explain(product(odyssey)));
  EXPECT_FALSE(explained.value().explain(product(odyssey)).steps().empty());
  EXPECT_EQ(explained.value().explain(product(no_match)),
            games.explain(product(no_match)));
  EXPECT_NE(explained.value(), rebuilt.value());
}

TEST_F(MyGamesAssembly, RefusesAGameListedTwice) {
  const auto owned = ps5_games(
      {{.id = odyssey, .product = odyssey, .name = "Synthetic Odyssey"}});
  const auto list = sample_list();
  const auto games = ps5_games_of(&owned, nullptr, std::nullopt, {}, &list);
  std::vector<MyGame> twice{games.visible().begin(), games.visible().end()};
  ASSERT_FALSE(twice.empty());
  twice.push_back(games.visible().front());
  EXPECT_EQ(MyGames::of(games.account(), twice, games.inputs()).error(),
            MyGames::Invalid::duplicate_product);
}

TEST_F(MyGamesAssembly,
       KeepsTheRowsLegacyCloudStreamBuildsWhateverThePs5Sources) {
  const auto shelves = sample_list();
  const auto ps4_list =
      list_of({on_old_service(god_of_war, "Storm of Ages III Remastered"),
               {.product = old_listing,
                .name = "Old Listing",
                .ps5 = false,
                .ps4 = true}});
  const auto ps4_owned = library_of({{.id = old_listing,
                                      .product = old_listing,
                                      .name = "Old Listing",
                                      .platform = "ps4",
                                      .package = "PS4GD"}});
  EXPECT_TRUE(builds_legacy_rows(sample_library(), shelves));
  EXPECT_TRUE(builds_legacy_rows(sample_library(true), shelves));
  EXPECT_TRUE(builds_legacy_rows(ps4_owned, ps4_list));
}

}  // namespace
}  // namespace catalog_test
