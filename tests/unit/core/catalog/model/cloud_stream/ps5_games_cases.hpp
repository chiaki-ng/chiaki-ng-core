#pragma once

#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace catalog_test {

class Ps5GamesCases : public RuleCases {
 public:
  using MyGame = libchiaking::catalog::MyGame;
  using MyGames = libchiaking::catalog::MyGames;
  using Explanation = libchiaking::catalog::Explanation;
  using Rule = Explanation::Rule;
  using Outcome = Explanation::Outcome;
  using Feature = PremiumEntitlement::Feature;
  using Recorded = Ps5Entitlement;

  static constexpr std::string_view rivet =
      "EP0109-PPSA07028_00-FAKE0110TITLE000";
  static constexpr std::string_view show =
      "UP0114-PPSA37004_00-FAKE0071TITLE000";
  static constexpr std::string_view bus =
      "EP0106-PPSA07037_00-FAKE0016TITLE000";
  static constexpr std::string_view bus_claim =
      "EP0106-PPSA07037_00-FAKE0014TITLE000";
  static constexpr std::string_view comet = "EP-PPSA10001_00-GAME";
  static constexpr std::string_view comet_deluxe = "EP-PPSA10001_00-GAMEDELUXE";
  static constexpr std::string_view bundle =
      "EP0100-PPSA57001_00-FAKE0010TITLE000";
  static constexpr std::string_view game_one =
      "EP0100-PPSA57002_00-FAKE0046TITLE000";
  static constexpr std::string_view game_two =
      "EP0100-PPSA57003_00-FAKE0047TITLE000";
  static constexpr std::string_view unbranded =
      "EP0100-PPSA67001_00-FAKE0134TITLE000";
  static constexpr std::string_view trial_only =
      "EP0100-PPSA77001_00-FAKE0132TITLE000";
  static constexpr std::string_view far_cry =
      "EP0100-CUSA07016_00-FAKE0038TITLE000";

  static auto two_game_bundle() -> Offered {
    return Offered{.product = bundle,
                   .name = "Double Pack",
                   .skus = {Sold{.sku = "EP0100-PPSA57001_00-BUNDLE-E001",
                                 .entitlements = {Granted{.id = game_one},
                                                  Granted{.id = game_two}}}},
                   .image = "https://image/bundle.png"};
  }

  static auto premium() -> libchiaking::catalog::cloud_stream::PremiumCategory {
    return category_of(
        {included(show, "League Diamond 26"),
         included(bus, "Coach Simulator 21 Next Stop"),
         included(comet, "Comet", "10001"), two_game_bundle(),
         Offered{.product = unbranded,
                 .name = "Unbranded",
                 .skus = {Sold{.sku = "U1",
                               .branded = false,
                               .entitlements = {Granted{.id = unbranded}}}}},
         Offered{.product = trial_only,
                 .name = "Trial Only",
                 .skus = {Sold{
                     .sku = "T1",
                     .entitlements = {Granted{.id = trial_only,
                                              .feature = Feature::trial}}}}}});
  }

  static auto count_on_ps5(const MyGames& games) -> std::size_t {
    const auto on_ps5 = [](const MyGame& game) {
      return game.title().service() ==
             libchiaking::catalog::ServiceGeneration::ps5_cloud;
    };
    return static_cast<std::size_t>(
        std::ranges::count_if(games.visible(), on_ps5) +
        std::ranges::count_if(games.hidden(), on_ps5));
  }

  static auto step_of(const Explanation& explanation, Rule rule)
      -> Explanation::Step {
    const auto found =
        std::ranges::find(explanation.steps(), rule, &Explanation::Step::rule);
    EXPECT_NE(found, explanation.steps().end());
    if (found == explanation.steps().end())
      throw std::out_of_range{"the explanation has no such step"};
    return *found;
  }
};

}  // namespace catalog_test
