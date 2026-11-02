#include "src/core/catalog/model/list/raw_listing.hpp"
#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/detail/catalog/streamable_list.hpp>

#include <gtest/gtest.h>

#include <utility>

namespace catalog_test {
namespace {

using libchiaking::catalog::StoreLocale;
using libchiaking::catalog::StreamableList;
using libchiaking::catalog::TitleNumber;
using Entry = StreamableList::Entry;

class RegionStreamableList : public ValueCases {
 protected:
  RegionStreamableList() {
    facts.titles = {Entry{title(rivet, "Rivet & Bolt: Split Worlds",
                                Platform::ps5, ServiceGeneration::ps5_cloud)
                              .with_alias(product(rivet_deluxe)),
                          PlusInclusion::not_included(Source::store_lists)},
                    Entry{title(god_of_war, "Storm of Ages III Remastered",
                                Platform::ps4, ServiceGeneration::old_service),
                          PlusInclusion::with_premium(Source::old_service)}};
    facts.included_but_not_streamable = {
        Entry{title(show, "League Diamond 26", Platform::ps5,
                    ServiceGeneration::ps5_cloud),
              PlusInclusion::with_premium(Source::store_lists)}};
    facts.marked_unstreamable = {*TitleNumber::from("PPSA07041"),
                                 *TitleNumber::from("PPSA07028")};
  }

  StreamableList::Facts facts{
      .region = *StoreLocale::parse("en-GB"),
      .settled_locale = *StoreLocale::parse("en-US"),
      .origin = libchiaking::catalog::ListOrigin::account_service,
      .complete = true,
      .fetched_at = at_ms(1000),
  };
};

TEST_F(RegionStreamableList, KeepsWhereAndWhenItWasRead) {
  const auto built = StreamableList::of(facts);
  ASSERT_TRUE(built);
  EXPECT_EQ(built->region().text(), "en-GB");
  EXPECT_EQ(built->settled_locale().text(), "en-US");
  EXPECT_EQ(built->origin(), libchiaking::catalog::ListOrigin::account_service);
  EXPECT_TRUE(built->is_complete());
  EXPECT_EQ(built->fetched_at(), at_ms(1000));
  EXPECT_EQ(built->titles().size(), 2U);
}

TEST_F(RegionStreamableList, FindsATitleByItsProductOrAlias) {
  const auto built = StreamableList::of(facts);
  ASSERT_TRUE(built);
  EXPECT_NE(built->find(product(rivet)), nullptr);
  EXPECT_EQ(built->find(product(rivet_deluxe)), built->find(product(rivet)));
  EXPECT_TRUE(built->covers(product(god_of_war)));
  EXPECT_FALSE(built->covers(product(show)));
  EXPECT_NE(built->find_not_streamable(product(show)), nullptr);
  EXPECT_EQ(built->find_not_streamable(product(rivet)), nullptr);
}

TEST_F(RegionStreamableList, ListsTitleNumbers) {
  const auto built = StreamableList::of(facts);
  ASSERT_TRUE(built);
  EXPECT_TRUE(built->lists(*TitleNumber::from("PPSA07028")));
  EXPECT_TRUE(built->lists(*TitleNumber::from("CUSA07009")));
  EXPECT_FALSE(built->lists(*TitleNumber::from("PPSA37004")));
  EXPECT_EQ(built->count_on(ServiceGeneration::old_service), 1U);
  EXPECT_EQ(built->count_on(ServiceGeneration::ps5_cloud), 1U);
}

TEST_F(RegionStreamableList, MarksUnstreamableOnlyTitlesItDoesNotList) {
  const auto built = StreamableList::of(facts);
  ASSERT_TRUE(built);
  EXPECT_TRUE(built->marks_unstreamable(*TitleNumber::from("PPSA07041")));
  EXPECT_FALSE(built->marks_unstreamable(*TitleNumber::from("PPSA07028")));
  EXPECT_FALSE(built->marks_unstreamable(*TitleNumber::from("PPSA37004")));
  EXPECT_EQ(built->marked_unstreamable().size(), 2U);
}

TEST_F(RegionStreamableList, RefusesAProductListedTwice) {
  facts.titles.push_back(
      Entry{title(rivet_deluxe, "Rivet Deluxe", Platform::ps5,
                  ServiceGeneration::ps5_cloud),
            PlusInclusion::not_included(Source::store_lists)});
  EXPECT_EQ(StreamableList::of(std::move(facts)).error(),
            StreamableList::Invalid::duplicate_product);
}

TEST_F(RegionStreamableList, StartsFromARawListingThatKeepsItsEntries) {
  libchiaking::catalog::RawListing raw{
      {*StoreLocale::parse("en-GB"), *StoreLocale::parse("en-GB"),
       libchiaking::catalog::ListOrigin::public_store, false, at_ms(1000)}};
  libchiaking::catalog::ListedEntry entry{.product = product(show)};
  entry.shelf = libchiaking::catalog::Shelf::plus_games;
  entry.streaming_supported = false;
  raw.add(entry);
  ASSERT_EQ(raw.entries().size(), 1U);
  EXPECT_EQ(raw.entries()[0], entry);
  EXPECT_FALSE(raw.source().complete);
}

}  // namespace
}  // namespace catalog_test
