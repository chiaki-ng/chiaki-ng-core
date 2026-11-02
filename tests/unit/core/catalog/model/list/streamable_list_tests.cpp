#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::ListStreamability;
using catalog::Platform;
using catalog::PlusInclusion;
using catalog::ServiceGeneration;
using catalog::Shelf;

class StreamableListing : public ::testing::Test, public RuleCases {};

TEST_F(StreamableListing,
       KeepsStoreEditionsAndSetsAsideIncludedTitlesThatDoNotStream) {
  const auto raw = raw_of({
      {.product = "EP-PPSA11111_00-ONE",
       .name = "ClockBreakers",
       .concept_id = "900"},
      {.product = "EP-PPSA27020_00-TWO",
       .name = "ClockBreakers 2",
       .concept_id = "900"},
      {.product = "EP-PPSA37005_00-MATCH",
       .name = "Supplemented",
       .concept_id = "901",
       .streams = false},
      {.product = "UP-PPSA37005_00-MATCHPLUS",
       .name = "Supplemented",
       .shelf = Shelf::plus_monthly_games,
       .concept_id = "901",
       .streams = false},
      {.product = "UP-PPSA47005_00-MONTHLY",
       .name = "Monthly Only",
       .shelf = Shelf::plus_monthly_games,
       .concept_id = "902",
       .streams = false},
  });
  const auto filtered = raw.filter_raw_streamable();
  ASSERT_TRUE(filtered);
  const auto& list = filtered.value();
  EXPECT_EQ(list.titles().size(), 2U);
  ASSERT_EQ(list.included_but_not_streamable().size(), 1U);
  EXPECT_NE(list.find_not_streamable(product("UP-PPSA37005_00-MATCHPLUS")),
            nullptr);
  EXPECT_EQ(list.find_not_streamable(product("UP-PPSA47005_00-MONTHLY")),
            nullptr);
  EXPECT_EQ(raw.entries().size(), 5U);
  const auto& aside = list.included_but_not_streamable().front();
  EXPECT_EQ(aside.inclusion,
            PlusInclusion::with_premium(PlusInclusion::Source::store_lists));
  EXPECT_EQ(aside.title.streamability(),
            ListStreamability::marked_unstreamable);
  EXPECT_EQ(list.marked_unstreamable().size(), 1U);
  EXPECT_TRUE(list.marked_unstreamable().contains(number("PPSA37005")));
}

TEST_F(StreamableListing, MakesARepeatedEditionAnAliasThatSharesItsInclusion) {
  const auto list = list_of({
      {.product = "EP-PPSA11111_00-A", .name = "Comet", .concept_id = "5"},
      {.product = "UP-PPSA11111_00-B", .name = "COMET", .concept_id = "5"},
      {.product = "JP-PPSA11111_00-C",
       .name = "Comet",
       .shelf = Shelf::plus_games,
       .concept_id = "5"},
  });
  ASSERT_EQ(list.titles().size(), 1U);
  const auto& comet = list.titles().front();
  EXPECT_EQ(comet.title.product_id(), product("EP-PPSA11111_00-A"));
  ASSERT_EQ(comet.title.aliases().size(), 2U);
  EXPECT_EQ(comet.title.aliases()[0], product("UP-PPSA11111_00-B"));
  EXPECT_EQ(comet.title.aliases()[1], product("JP-PPSA11111_00-C"));
  EXPECT_EQ(list.find(product("JP-PPSA11111_00-C")), &comet);
  EXPECT_TRUE(comet.inclusion.is_included());
  EXPECT_EQ(comet.inclusion.source(), PlusInclusion::Source::store_lists);
  EXPECT_EQ(comet.title.streamability(), ListStreamability::listed);
}

TEST_F(StreamableListing, MarksATitleNumberOnlyWhenNoEditionOfItStreams) {
  const auto list = list_of({
      {.product = "EP0101-PPSA07041_00-FAKE0023TITLE000",
       .name = "Line of Fire",
       .concept_id = "90000015",
       .streams = false},
      {.product = "UP0114-PPSA37004_00-FAKE0071TITLE000",
       .name = "League Diamond 26",
       .concept_id = "10015430"},
      {.product = "EP0109-PPSA07035_00-FAKE0073TITLE000",
       .name = "Ronin 2",
       .concept_id = "234389",
       .ps4 = true},
      {.product = "EP-PPSA57007_00-EDITIONA",
       .name = "Mixed",
       .concept_id = "5",
       .streams = false},
      {.product = "UP-PPSA57007_00-EDITIONB",
       .name = "Mixed",
       .concept_id = "5"},
  });
  EXPECT_EQ(list.marked_unstreamable().size(), 1U);
  EXPECT_TRUE(list.marked_unstreamable().contains(number("PPSA07041")));
  EXPECT_TRUE(list.marks_unstreamable(number("PPSA07041")));
  EXPECT_FALSE(list.marks_unstreamable(number("PPSA57007")));
  EXPECT_EQ(list.titles().size(), 3U);
  EXPECT_FALSE(list.covers(product("EP0101-PPSA07041_00-FAKE0023TITLE000")));
  EXPECT_FALSE(list.covers(product("EP-PPSA57007_00-EDITIONA")));
}

TEST_F(StreamableListing, ConsidersOnlyEntriesForPs4OrPs5) {
  const auto list = list_of({
      {.product = "UP-PPSA07007_00-VITAONLY",
       .name = "Vita Only",
       .ps5 = false},
      {.product = "UP-PPSA07008_00-UNFLAGGED",
       .name = "Unflagged",
       .streams = std::nullopt},
      {.product = "UP-PPSA07009_00-KEPT", .name = "Kept"},
  });
  EXPECT_EQ(list.titles().size(), 1U);
  EXPECT_TRUE(list.covers(product("UP-PPSA07009_00-KEPT")));
  EXPECT_TRUE(list.marked_unstreamable().empty());
}

TEST_F(StreamableListing,
       PutsTheOldServiceListFirstAndKeepsATitleTheStoreAlsoLists) {
  const auto list = list_of({
      {.product = "PPSA-CROW_00", .name = "Raven Hollow", .concept_id = "111"},
      {.product = "UP-PPSA07010_00-STOREONLY",
       .name = "Store Only",
       .shelf = Shelf::free_to_play},
      on_old_service("PPSA-CROW_00", "Raven Hollow", "111"),
      on_old_service("UP0114-CUSA07009_00-FAKE0051TITLE000",
                     "Storm of Ages III Remastered"),
      on_old_service("UP0114-CUSA07009_00-FAKE0051TITLE000",
                     "Storm of Ages III Remastered"),
      on_old_service("UP0114-NPUB37002_00-FAKE0021TITLE000", "Classic"),
  });
  ASSERT_EQ(list.titles().size(), 4U);
  const auto& crow = list.titles()[0];
  EXPECT_EQ(crow.title.product_id(), product("PPSA-CROW_00"));
  EXPECT_EQ(crow.title.service(), ServiceGeneration::old_service);
  EXPECT_EQ(crow.title.platform(), Platform::ps5);
  EXPECT_EQ(crow.inclusion,
            PlusInclusion::with_premium(PlusInclusion::Source::old_service));
  EXPECT_EQ(list.titles()[1].title.platform(), Platform::ps4);
  EXPECT_EQ(list.titles()[2].title.platform(), Platform::ps3);
  const auto& store = list.titles()[3];
  EXPECT_EQ(store.title.service(), ServiceGeneration::ps5_cloud);
  EXPECT_FALSE(store.inclusion.is_included());
  EXPECT_EQ(list.count_on(ServiceGeneration::old_service), 3U);
  EXPECT_EQ(list.count_on(ServiceGeneration::ps5_cloud), 1U);
  EXPECT_EQ(list.origin(), catalog::ListOrigin::account_service);
  EXPECT_TRUE(list.is_complete());
  EXPECT_EQ(list.fetched_at(), at_ms(1000));
  EXPECT_EQ(list.region().text(), "en-GB");
}

TEST_F(StreamableListing, LetsTheDeviceListDecideTheStorePlatform) {
  const auto list = list_of(
      {
          {.product = "CUSA-BUNDLE_00",
           .name = "Indie Bundle",
           .concept_id = "501",
           .ps4 = true},
          {.product = "CUSA-PS4ONLY_00",
           .name = "PS4 Only",
           .concept_id = "502",
           .ps5 = false,
           .ps4 = true},
      },
      catalog::ListOrigin::public_store);
  EXPECT_EQ(list.origin(), catalog::ListOrigin::public_store);
  const auto* bundle = list.find(product("CUSA-BUNDLE_00"));
  const auto* ps4_only = list.find(product("CUSA-PS4ONLY_00"));
  ASSERT_NE(bundle, nullptr);
  EXPECT_EQ(bundle->title.platform(), Platform::ps5);
  EXPECT_EQ(bundle->title.service(), ServiceGeneration::ps5_cloud);
  ASSERT_NE(ps4_only, nullptr);
  EXPECT_EQ(ps4_only->title.platform(), Platform::ps4);
}

TEST_F(StreamableListing, KeepsOneTitleForAProductListedTwiceUnderAnotherName) {
  const auto list = list_of({
      {.product = "UP-PPSA07011_00-GAME", .name = "Game"},
      {.product = "UP-PPSA07011_00-GAME",
       .name = "Game Deluxe",
       .shelf = Shelf::plus_classics},
      {.product = "UP-PPSA07012_00-UNNAMED", .name = "  "},
  });
  ASSERT_EQ(list.titles().size(), 1U);
  EXPECT_EQ(list.titles().front().title.name(), "Game");
  EXPECT_TRUE(list.titles().front().inclusion.is_included());
}

}  // namespace
}  // namespace catalog_test
