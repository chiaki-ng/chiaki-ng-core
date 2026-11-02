#include "src/core/catalog/usecases/recently_played.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::ListStreamability;
using catalog::PlayPath;
using catalog::RecentlyPlayedGame;
using catalog::Standing;
using catalog::usecases::RecentlyPlayed;
using libchiaking::psn::rest::PlayedTitle;

class RecentlyPlayedMatching : public ::testing::Test, public RuleCases {
 protected:
  static constexpr std::string_view comet =
      "UP0114-PPSA07029_00-FAKE0005TITLE000";
  static constexpr std::string_view rivet =
      "EP0109-PPSA07028_00-FAKE0110TITLE000";
  static constexpr std::string_view returnal =
      "UP0114-PPSA07028_00-FAKE0115TITLE000";
  static constexpr std::string_view playroom =
      "IP0110-PPSA07030_00-FAKE0095TITLE000";
  static constexpr std::string_view store =
      "UP0114-PPSA07031_00-FAKE0123TITLE000";

  static auto played(std::string_view id, std::string_view when)
      -> PlayedTitle {
    return {.title_id = std::string{id},
            .name = "Game",
            .image_url = "",
            .category = "ps5_native_game",
            .last_played = std::string{when},
            .first_played = "",
            .play_duration = "",
            .play_count = std::nullopt};
  }

  static auto owned_games() -> catalog::MyGames {
    const auto owned =
        ps5_games({{.id = comet, .product = comet, .name = "Comet Bot"},
                   {.id = rivet, .product = rivet, .name = "Rivet"}});
    return ps5_games_of(&owned, nullptr);
  }

  static auto shown(std::string_view id, PlayPath path,
                    ListStreamability streamability) -> catalog::MyGame {
    auto title =
        catalog::Title::from({.product = product(id),
                              .name = "Game",
                              .platform = catalog::Platform::ps5,
                              .service = catalog::ServiceGeneration::ps5_cloud,
                              .streamability = streamability});
    EXPECT_TRUE(title) << id;
    auto game = catalog::MyGame::from(
        {.title = std::move(title).value(),
         .inclusion = catalog::PlusInclusion::not_included(
             catalog::PlusInclusion::Source::store_tags),
         .path = path,
         .visibility = catalog::Visibility::visible(),
         .launch_identifier = catalog::LaunchIdentifier::of(product(id))});
    EXPECT_TRUE(game) << id;
    return std::move(game).value();
  }

  static auto has_no_play_details(const RecentlyPlayedGame& game)
      -> testing::AssertionResult {
    if (!game.title) return testing::AssertionFailure() << "it has no title";
    if (game.play_time)
      return testing::AssertionFailure() << "it has a play time";
    if (game.first_played)
      return testing::AssertionFailure() << "it has a first play";
    if (game.play_count)
      return testing::AssertionFailure() << "it has a play count";
    return testing::AssertionSuccess();
  }
};

TEST_F(RecentlyPlayedMatching, ShowsPlayedTitlesAsGamesInPsnOrder) {
  const auto games = owned_games();
  EXPECT_EQ(games.visible().size(), 2U);
  const std::vector<PlayedTitle> history{
      played("PPSA07028_00", "2026-01-01T18:00:00Z"),
      played("PPSA99999_00", "2026-01-01T17:00:00Z"),
      played("NPUB37002_00", "2026-01-01T16:00:00Z"),
      played("PPSA07029_00", "2026-01-01T12:00:00.000Z"),
      played("PPSA07029_00", "2025-12-01T12:00:00Z"),
  };
  const auto found = RecentlyPlayed::matched(games, history);
  ASSERT_EQ(found.size(), 2U);
  EXPECT_EQ(found[0].product, rivet);
  EXPECT_EQ(found[0].last_played, at_ms(1767290400000));
  EXPECT_EQ(found[1].product, comet);
  EXPECT_EQ(found[1].last_played, at_ms(1767268800000));
}

TEST_F(RecentlyPlayedMatching, KeepsAGameWithAnUnreadableTimeAtTheEpoch) {
  const auto found = RecentlyPlayed::matched(
      owned_games(), std::vector{played("PPSA07029_00", "yesterday")});
  ASSERT_EQ(found.size(), 1U);
  EXPECT_EQ(found[0].product, comet);
  EXPECT_EQ(found[0].last_played, at_ms(0));
}

TEST_F(RecentlyPlayedMatching,
       TakesPlayTimeCountAndFirstPlayFromThePlayedTitle) {
  auto racer = played("PPSA07028_00", "2026-03-04T18:22:01Z");
  racer.first_played = "2025-11-02T19:04:11.000Z";
  racer.play_duration = "PT47H12M5S";
  racer.play_count = 41;
  const auto found = RecentlyPlayed::matched(owned_games(), std::vector{racer});
  ASSERT_EQ(found.size(), 1U);
  const auto& game = found[0];
  EXPECT_EQ(game.product, rivet);
  EXPECT_EQ(game.last_played, at_ms(1772648521000));
  ASSERT_TRUE(game.title);
  EXPECT_EQ(*game.title, "PPSA07028_00");
  EXPECT_EQ(game.play_time, std::chrono::seconds{47 * 3600 + 12 * 60 + 5});
  EXPECT_EQ(game.first_played, at_ms(1762110251000));
  EXPECT_EQ(game.play_count, 41U);
}

TEST_F(RecentlyPlayedMatching, LeavesMissingOrMalformedPlayDetailsEmpty) {
  auto bare = played("PPSA07028_00", "2026-03-04T18:22:01Z");
  auto garbled = played("PPSA07029_00", "2026-03-04T17:00:00Z");
  garbled.first_played = "last autumn";
  garbled.play_duration = "47 hours";
  const auto found =
      RecentlyPlayed::matched(owned_games(), std::vector{bare, garbled});
  ASSERT_EQ(found.size(), 2U);
  EXPECT_TRUE(has_no_play_details(found[0]));
  EXPECT_TRUE(has_no_play_details(found[1]));
}

TEST_F(RecentlyPlayedMatching, KeepsAZeroPlayTimeButNotAnEpochFirstPlay) {
  auto zero = played("PPSA07029_00", "");
  zero.play_duration = "PT0S";
  zero.first_played = "1970-01-01T00:00:00Z";
  const auto never = RecentlyPlayed::matched(owned_games(), std::vector{zero});
  ASSERT_EQ(never.size(), 1U);
  EXPECT_EQ(never[0].play_time, std::chrono::seconds{0});
  EXPECT_FALSE(never[0].first_played);
  EXPECT_EQ(never[0].last_played, at_ms(0));
}

TEST_F(RecentlyPlayedMatching, ListsARepeatedlyPlayedGameOnce) {
  const auto games = owned_games();
  std::vector<PlayedTitle> history;
  for (std::size_t index = 0; index < RecentlyPlayed::most_shown * 2; ++index)
    history.push_back(
        played(index % 2 == 0 ? "PPSA07029_00" : "PPSA07028_00", ""));
  EXPECT_EQ(RecentlyPlayed::matched(games, history).size(), 2U);
  EXPECT_TRUE(RecentlyPlayed::matched(games, {}).empty());
}

TEST_F(RecentlyPlayedMatching, ShowsOneGameForATitleSharedByTwoListings) {
  const auto owned =
      ps5_games({{.id = rivet, .product = rivet, .name = "Rivet"},
                 {.id = returnal, .product = returnal, .name = "Returnal"}});
  const auto games = ps5_games_of(&owned, nullptr);
  EXPECT_EQ(games.visible().size(), 2U);
  const auto found = RecentlyPlayed::matched(
      games, std::vector{played("PPSA07028_00", "2026-01-01T18:00:00Z")});
  EXPECT_EQ(found.size(), 1U);
}

TEST_F(RecentlyPlayedMatching, KeepsFlaggedGamesAndLeavesOutStoreOnlyGames) {
  auto games = catalog::MyGames::of(
      account,
      {shown(comet, PlayPath::included, ListStreamability::listed),
       shown(playroom, PlayPath::included,
             ListStreamability::marked_unstreamable),
       shown(store, PlayPath::store_only, ListStreamability::listed)},
      {});
  ASSERT_TRUE(games);
  EXPECT_EQ(game_of(games.value(), playroom).standing(), Standing::may_stream);
  EXPECT_EQ(game_of(games.value(), store).standing(), Standing::store);
  const auto found = RecentlyPlayed::matched(
      games.value(),
      std::vector{played("PPSA07030_00", "2026-01-01T18:00:00Z"),
                  played("PPSA07031_00", "2026-01-01T17:00:00Z"),
                  played("PPSA07029_00", "2026-01-01T16:00:00Z")});
  ASSERT_EQ(found.size(), 2U);
  EXPECT_EQ(found[0].product, playroom);
  EXPECT_EQ(found[1].product, comet);
}

TEST_F(RecentlyPlayedMatching,
       MatchesAnOwnedTileAndAnUnclaimedPremiumTileForPlayedPs5Titles) {
  const auto owned =
      ps5_games({{.id = comet, .product = comet, .name = "Comet Bot"}});
  const auto premium = category_of({included(rivet, "Rivet")});
  const auto games = ps5_games_of(&owned, &premium);
  const auto& claimed = game_of(games, comet);
  const auto& offered = game_of(games, rivet);
  EXPECT_EQ(claimed.play_path(), PlayPath::owned);
  EXPECT_EQ(offered.play_path(), PlayPath::included);
  EXPECT_TRUE(offered.needs_library_add());
  const auto found = RecentlyPlayed::matched(
      games, std::vector{played("PPSA07028_00", "2026-01-01T18:00:00Z"),
                         played("PPSA07029_00", "2026-01-01T17:00:00Z")});
  ASSERT_EQ(found.size(), 2U);
  EXPECT_EQ(found[0].product, rivet);
  EXPECT_EQ(found[1].product, comet);
}

TEST_F(RecentlyPlayedMatching, NeverPlaysBackAPs5GameThatIsOnlyForSale) {
  const auto owned = ps5_games({});
  const auto premium =
      category_of({{.product = store,
                    .name = "Store",
                    .skus = {{.sku = "SKU1",
                              .branded = false,
                              .entitlements = {{.id = store}}}}}});
  const auto sold =
      for_sale_of({{.product = store,
                    .name = "Store",
                    .skus = {{.sku = "SKU1",
                              .branded = false,
                              .entitlements = {{.id = store}}}}}});
  const auto games =
      ps5_games_of(&owned, &premium, std::nullopt, {}, nullptr, &sold);
  EXPECT_EQ(game_of(games, store).standing(), Standing::store);
  EXPECT_TRUE(game_of(games, store).is_visible());
  EXPECT_TRUE(
      RecentlyPlayed::matched(
          games, std::vector{played("PPSA07031_00", "2026-01-01T18:00:00Z")})
          .empty());
}

}  // namespace
}  // namespace catalog_test
