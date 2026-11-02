#pragma once

#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace catalog_test {

class MyGamesCases : public RuleCases {
 public:
  using MyGame = libchiaking::catalog::MyGame;
  using MyGames = libchiaking::catalog::MyGames;
  using Explanation = libchiaking::catalog::Explanation;
  using Rule = Explanation::Rule;

  static constexpr std::string_view show =
      "UP0114-PPSA37004_00-FAKE0071TITLE000";
  static constexpr std::string_view odyssey =
      "UP0114-PPSA07043_00-FAKE0043TITLE000";
  static constexpr std::string_view odyssey_gold =
      "UP0114-PPSA07043_00-FAKE0053TITLE000";
  static constexpr std::string_view odyssey_standard =
      "UP0114-PPSA07047_00-FAKE0120TITLE000";
  static constexpr std::string_view plus_pick =
      "UP0114-PPSA07046_00-FAKE0093TITLE000";
  static constexpr std::string_view classic =
      "UP0114-CUSA07031_00-FAKE0106TITLE000";
  static constexpr std::string_view no_match =
      "UP0114-CUSA07032_00-FAKE0076TITLE000";
  static constexpr std::string_view cross_buy =
      "UP0114-CUSA07033_00-FAKE0024TITLE000";
  static constexpr std::string_view supplement =
      "UP0114-CUSA07034_00-FAKE0125TITLE000";
  static constexpr std::string_view preorder =
      "UP0114-PPSA07045_00-FAKE0097TITLE000";
  static constexpr std::string_view rivet =
      "EP0109-PPSA07028_00-FAKE0110TITLE000";
  static constexpr std::string_view old_listing =
      "EP0109-CUSA07023_00-FAKE0085TITLE000";
  static constexpr std::string_view bus =
      "EP0106-PPSA07037_00-FAKE0016TITLE000";
  static constexpr std::string_view god_of_war =
      "UP0114-CUSA07009_00-FAKE0051TITLE000";

  static auto sample_list() -> libchiaking::catalog::StreamableList {
    return list_of({
        on_old_service(classic, "Classic Quest"),
        {.product = cross_buy, .name = "Cross Buy"},
        {.product = odyssey_gold, .name = "Synthetic Odyssey Gold Edition"},
        {.product = odyssey_standard,
         .name = "Synthetic Odyssey: Standard Edition"},
        {.product = plus_pick, .name = "Plus Pick"},
        {.product = plus_pick, .name = "Plus Pick", .shelf = Shelf::plus_games},
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
    });
  }

  static auto sample_library(bool with_plus_pick = false)
      -> libchiaking::catalog::Library {
    std::vector<OwnedRecord> records{
        record({.id = "UP0114-CUSA07030_00-FAKE0077TITLE000",
                .product = "UP0114-CUSA07030_00-FAKE0077TITLE000",
                .name = "",
                .platform = "",
                .details = false}),
        record({.id = odyssey,
                .product = odyssey,
                .name = "Synthetic Odyssey",
                .active_date = "2025-03-14T00:00:00.000Z"}),
        record({.id = "UP0114-PPSA07044_00-FAKE0127TITLE000",
                .product = "UP0114-PPSA07044_00-FAKE0127TITLE000",
                .name = "Tracker",
                .package = "PSTRACK"}),
        record({.id = classic,
                .product = classic,
                .name = "Classic Quest",
                .platform = "ps4",
                .package = "PS4GD"}),
        record({.id = no_match,
                .product = no_match,
                .name = "Unlisted Game",
                .platform = "ps4",
                .package = "PS4GD"}),
        record({.id = odyssey,
                .product = "UP0114-PPSA07043_00-FAKE0010TITLE000",
                .name = "Synthetic Odyssey"}),
        record({.id = cross_buy,
                .product = cross_buy,
                .name = "Cross Buy",
                .platform = "ps4",
                .package = "PS4GD"}),
        record({.id = supplement,
                .product = supplement,
                .name = "Supplement Only",
                .platform = "ps4",
                .package = "PS4GD"}),
        record({.id = preorder,
                .product = preorder,
                .name = "Next Ember",
                .preorder = true}),
    };
    if (with_plus_pick)
      records.push_back(
          record({.id = plus_pick, .product = plus_pick, .name = "Plus Pick"}));
    return OwnedRecord::library_of(account, records, at_ms(library_fetched_ms));
  }

  static auto agrees_with_its_explanation(const MyGames& games,
                                          const MyGame& game)
      -> testing::AssertionResult {
    const auto id = game.product_id().text();
    const auto explanation = games.explain(game.product_id());
    if (explanation.product_id() != game.product_id())
      return testing::AssertionFailure()
             << id << " is explained as another product";
    const auto failures =
        std::ranges::count(explanation.steps(), Explanation::Outcome::failed,
                           &Explanation::Step::outcome);
    if (explanation.is_listed() != game.is_visible())
      return testing::AssertionFailure()
             << id << " is listed and visible differently";
    if (game.is_visible() != (failures == 0))
      return testing::AssertionFailure()
             << id << " has " << failures << " failures and visibility "
             << game.is_visible();
    if (game.is_visible()) return testing::AssertionSuccess();
    if (failures != 1)
      return testing::AssertionFailure()
             << id << " is hidden by " << failures << " failures";
    if (explanation.hide_reason() != game.visibility().hide_reason())
      return testing::AssertionFailure()
             << id << " is explained as hidden for another reason";
    const auto verdict =
        libchiaking::catalog::LaunchVerdict::of(&game, at_ms(1));
    if (verdict.is_allowed())
      return testing::AssertionFailure() << id << " is hidden but launches";
    if (verdict.refusal()->kind() !=
        libchiaking::catalog::LaunchRefusal::Kind::hidden)
      return testing::AssertionFailure()
             << id << " is refused for another kind of reason";
    if (verdict.refusal()->hide_reason() != game.visibility().hide_reason())
      return testing::AssertionFailure()
             << id << " is refused for another hide reason";
    return testing::AssertionSuccess();
  }

  static auto every_game_agrees_with_its_explanation(const MyGames& games)
      -> testing::AssertionResult {
    for (const auto shelf : {games.visible(), games.hidden()})
      for (const auto& game : shelf)
        if (auto agreed = agrees_with_its_explanation(games, game); !agreed)
          return agreed;
    return testing::AssertionSuccess();
  }
};

}  // namespace catalog_test
