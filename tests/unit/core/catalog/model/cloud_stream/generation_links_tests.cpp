#include "src/core/catalog/model/cloud_stream/generation_links.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;

class GenerationLinks : public ::testing::Test, public RuleCases {
 protected:
  static constexpr std::string_view sackboy_ps5 =
      "UP9000-PPSA01289_00-SACKBOYADVENTURE";
  static constexpr std::string_view sackboy_ps4 =
      "UP9000-CUSA18886_00-SACKBOYADVENTURE";
  static constexpr std::string_view classic =
      "UP0114-CUSA07031_00-FAKE0106TITLE000";

  static auto cross_generation_offer(std::string_view ps5, std::string_view ps4,
                                     std::string_view name) -> Offered {
    return Offered{
        .product = ps5,
        .name = name,
        .skus = {Sold{.sku = "SKU1", .entitlements = {Granted{.id = ps5}}},
                 Sold{.sku = "SKU2",
                      .branded = false,
                      .entitlements = {Granted{.id = ps4, .package = "PS4GD"},
                                       Granted{.id = ps5}}}}};
  }

  static auto title_number(std::string_view id) -> catalog::TitleNumber {
    auto number = product(id).title_number();
    EXPECT_TRUE(number) << id;
    return std::move(number).value();
  }

  static auto visible_names(const catalog::MyGames& games)
      -> std::vector<std::string> {
    std::vector<std::string> names;
    for (const auto& game : games.visible())
      names.push_back(game.title().name());
    return names;
  }
};

TEST_F(GenerationLinks, LinkEachPs4TitleToOnePs5Title) {
  catalog::cloud_stream::GenerationLinks links;
  EXPECT_TRUE(links.empty());
  const std::vector products{
      premium_product(cross_generation_offer(sackboy_ps5, sackboy_ps4,
                                             "Sackboy: A Big Adventure")),
      premium_product(cross_generation_offer(
          "UP9000-PPSA09999_00-SACKBOYDELUXE000", sackboy_ps4, "Deluxe")),
      premium_product(
          cross_generation_offer("UP9000-CUSA00001_00-PS4ONLYPRODUCT00",
                                 "UP9000-CUSA00002_00-OTHER", "PS4 product")),
      premium_product(included("EP-PPSA00003_00-BANANA", "Banana"))};
  links.add(products);
  EXPECT_EQ(links.other_of(title_number(sackboy_ps4)),
            title_number(sackboy_ps5));
  EXPECT_EQ(links.other_of(title_number(sackboy_ps5)),
            title_number(sackboy_ps4));
  EXPECT_FALSE(
      links.other_of(title_number("UP9000-PPSA09999_00-SACKBOYDELUXE000")));
  EXPECT_FALSE(links.other_of(title_number("UP9000-CUSA00002_00-OTHER")));
  EXPECT_FALSE(links.other_of(title_number("EP-PPSA00003_00-BANANA")));
}

TEST_F(GenerationLinks, LinkBothGenerationsOfAGameAndOrderThemTogether) {
  const auto category = category_of(
      {included("EP-PPSA00003_00-BANANA", "Banana"),
       cross_generation_offer(sackboy_ps5, sackboy_ps4, "Sackboy")});
  const auto list = list_of({on_old_service(sackboy_ps4, "Zz Sackboy PS4"),
                             on_old_service(classic, "Classic Quest")});
  const auto games = ps5_games_of(nullptr, &category, std::nullopt, {}, &list);
  const auto& ps5 = game_of(games, sackboy_ps5);
  const auto& ps4 = game_of(games, sackboy_ps4);
  ASSERT_TRUE(ps5.other_version());
  EXPECT_EQ(ps5.other_version()->product, product(sackboy_ps4));
  EXPECT_TRUE(ps5.other_version()->leads);
  ASSERT_TRUE(ps4.other_version());
  EXPECT_EQ(ps4.other_version()->product, product(sackboy_ps5));
  EXPECT_FALSE(ps4.other_version()->leads);
  EXPECT_FALSE(game_of(games, classic).other_version());
  EXPECT_FALSE(game_of(games, "EP-PPSA00003_00-BANANA").other_version());
  EXPECT_EQ(visible_names(games),
            (std::vector<std::string>{"Banana", "Classic Quest", "Sackboy",
                                      "Zz Sackboy PS4"}));
  EXPECT_TRUE(ps4.title().is_known_as(product(sackboy_ps4)));
  EXPECT_FALSE(ps4.title().is_known_as(product(sackboy_ps5)));
}

TEST_F(GenerationLinks, AreAbsentWithoutTheStoreLinkingTheGenerations) {
  const auto category = category_of({included(sackboy_ps5, "Sackboy")});
  const auto list = list_of({on_old_service(sackboy_ps4, "Sackboy PS4")});
  const auto games = ps5_games_of(nullptr, &category, std::nullopt, {}, &list);
  EXPECT_FALSE(game_of(games, sackboy_ps5).other_version());
  EXPECT_FALSE(game_of(games, sackboy_ps4).other_version());
}

}  // namespace
}  // namespace catalog_test
