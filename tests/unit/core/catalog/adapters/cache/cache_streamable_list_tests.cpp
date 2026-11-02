#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/cache/shelf_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace catalog_test {
namespace {

namespace cache = libchiaking::catalog::cache;

class CachedStreamableList : public ::testing::Test, protected CacheCases {
 protected:
  CacheShelf shelf;
};

TEST_F(CachedStreamableList, RoundTrips) {
  const auto list = streamable(shelf.now() - std::chrono::seconds{3});
  const auto record = cache::StreamableListRecord::of(list);
  ASSERT_TRUE(record);
  EXPECT_EQ(record->list(), list);
  ASSERT_TRUE(shelf.cache.write(*record));
  const auto read =
      shelf.cache.read<cache::StreamableListRecord>(locale("en-GB"));
  ASSERT_TRUE(read);
  ASSERT_TRUE(read.value());
  EXPECT_EQ(*read.value(), *record);
  const auto& back = read.value()->list();
  EXPECT_EQ(back, list);
  ASSERT_EQ(back.titles().size(), 3U);
  ASSERT_EQ(back.included_but_not_streamable().size(), 1U);
  EXPECT_EQ(back.titles()[0].title.name(),
            "Rivet & Bolt: Split Worlds \xE2\x84\xA2 \"Launch\"");
  EXPECT_EQ(back.titles()[0].title.aliases().size(), 1U);
  EXPECT_TRUE(back.titles()[0].title.concept_page());
  EXPECT_EQ(back.titles()[0].inclusion.lowest_tier(), Tier::extra);
  EXPECT_EQ(back.titles()[1].inclusion.lowest_tier(), Tier::premium);
  EXPECT_FALSE(back.titles()[2].inclusion.is_included());
  EXPECT_EQ(back.included_but_not_streamable()[0].inclusion.lowest_tier(),
            Tier::essential);
  EXPECT_EQ(back.marked_unstreamable().size(), 2U);
  EXPECT_EQ(back.settled_locale(), locale("en-US"));
  EXPECT_EQ(back.fetched_at(), list.fetched_at());
}

TEST_F(CachedStreamableList, IsKeptByRegionWithoutAnAccount) {
  ASSERT_TRUE(shelf.cache.write(*cache::StreamableListRecord::of(
      streamable(shelf.now() - std::chrono::seconds{3}))));
  const auto text = shelf.text("streamable/gb-en");
  EXPECT_TRUE(text.starts_with(
      R"({"record":"streamable","schema":2,"writtenAtMs":1800000000000,"region":"en-GB","body":{)"))
      << text;
  EXPECT_EQ(text.find("\"account\""), std::string::npos);
}

TEST_F(CachedStreamableList, IsWrittenByteForByte) {
  ASSERT_TRUE(shelf.cache.write(*cache::StreamableListRecord::of(
      streamable(shelf.now() - std::chrono::seconds{3}))));
  EXPECT_EQ(
      shelf.text("streamable/gb-en"),
      R"({"record":"streamable","schema":2,"writtenAtMs":1800000000000,"region")"
      R"(:"en-GB","body":{"settledLocale":"en-US","origin":"account_service","c)"
      R"(omplete":true,"fetchedAtMs":1799999997000,"titles":[{"product":"EP0109)"
      R"(-PPSA07028_00-FAKE0110TITLE000","name":"Rivet & Bolt: Split Worlds ™ )"
      R"(\"Launch\"","platform":"ps5","service":"ps5_cloud","streamability":"li)"
      R"(sted","concept":"90000016","cover":"https://image.api.playstation.com/)"
      R"(rift/cover.png","landscapeCover":"https://image.api.playstation.com/ri)"
      R"(ft/wide.png","conceptPage":"https://store.playstation.com/en-gb/concep)"
      R"(t/90000016","conceptPageSource":"concept_page","aliases":["EP0109-PPSA)"
      R"(07028_00-FAKE0109TITLE000"],"inclusionSource":"store_tags","inclusionT)"
      R"(ier":"extra"},{"product":"UP0114-CUSA07009_00-FAKE0051TITLE000","name")"
      R"(:"Storm of Ages III Remastered","platform":"ps4","service":"old_service",)"
      R"("streamability":"unknown","cover":"","landscapeCover":"","aliases":[],)"
      R"("inclusionSource":"old_service","inclusionTier":"premium"},{"product":)"
      R"("UP0114-PPSA37004_00-FAKE0071TITLE000","name":"League Diamond 26","platf)"
      R"(orm":"ps5","service":"ps5_cloud","streamability":"marked_unstreamable")"
      R"(,"cover":"","landscapeCover":"","aliases":[],"inclusionSource":"store_)"
      R"(lists"}],"includedButNotStreamable":[{"product":"EP0106-PPSA07037_00-F)"
      R"(AKE0016TITLE000","name":"Coach Simulator 21","platform":"ps5","service":)"
      R"("ps5_cloud","streamability":"unknown","cover":"","landscapeCover":"",")"
      R"(aliases":[],"inclusionSource":"store_lists","inclusionTier":"essential)"
      R"("}],"markedUnstreamable":["CUSA99999","PPSA37004"]}})");
}

TEST_F(CachedStreamableList, IsMadeOnlyFromACompleteAccountServiceList) {
  EXPECT_FALSE(cache::StreamableListRecord::of(streamable(shelf.now(), false)));
  EXPECT_FALSE(cache::StreamableListRecord::of(
      streamable(shelf.now(), true, ListOrigin::public_store)));
  EXPECT_FALSE(cache::StreamableListRecord::of(
      streamable(shelf.now(), false, ListOrigin::public_store)));
  EXPECT_TRUE(cache::StreamableListRecord::of(streamable(shelf.now())));
  EXPECT_EQ(shelf.store.writes, 0U);
}

TEST_F(CachedStreamableList, ErasesAStoredIncompleteList) {
  ASSERT_TRUE(shelf.cache.write(
      *cache::StreamableListRecord::of(streamable(shelf.now()))));
  const auto complete = shelf.text("streamable/gb-en");
  shelf.put("streamable/gb-en",
            replaced(complete, R"("complete":true)", R"("complete":false)"));
  const auto read =
      shelf.cache.read<cache::StreamableListRecord>(locale("en-GB"));
  ASSERT_TRUE(read);
  EXPECT_FALSE(read.value());
  EXPECT_FALSE(shelf.has("streamable/gb-en"));
}

TEST_F(CachedStreamableList, ErasesAStoredPublicStoreList) {
  ASSERT_TRUE(shelf.cache.write(
      *cache::StreamableListRecord::of(streamable(shelf.now()))));
  const auto complete = shelf.text("streamable/gb-en");
  shelf.put("streamable/gb-en",
            replaced(complete, R"("origin":"account_service")",
                     R"("origin":"public_store")"));
  const auto read =
      shelf.cache.read<cache::StreamableListRecord>(locale("en-GB"));
  ASSERT_TRUE(read);
  EXPECT_FALSE(read.value());
  EXPECT_FALSE(shelf.has("streamable/gb-en"));
}

TEST_F(CachedStreamableList, IsNotReadForAnotherRegion) {
  ASSERT_TRUE(shelf.cache.write(
      *cache::StreamableListRecord::of(streamable(shelf.now()))));
  auto elsewhere =
      shelf.cache.read<cache::StreamableListRecord>(locale("en-US"));
  ASSERT_TRUE(elsewhere);
  EXPECT_FALSE(elsewhere.value());
  elsewhere = shelf.cache.read<cache::StreamableListRecord>(locale("fr-GB"));
  ASSERT_TRUE(elsewhere);
  EXPECT_FALSE(elsewhere.value());
  EXPECT_TRUE(shelf.has("streamable/gb-en"));
}

TEST_F(CachedStreamableList, ErasesAListFiledUnderTheWrongRegion) {
  ASSERT_TRUE(shelf.cache.write(
      *cache::StreamableListRecord::of(streamable(shelf.now()))));
  shelf.put("streamable/us-en", shelf.text("streamable/gb-en"));
  const auto elsewhere =
      shelf.cache.read<cache::StreamableListRecord>(locale("en-US"));
  ASSERT_TRUE(elsewhere);
  EXPECT_FALSE(elsewhere.value());
  EXPECT_FALSE(shelf.has("streamable/us-en"));
  const auto kept =
      shelf.cache.read<cache::StreamableListRecord>(locale("en-GB"));
  ASSERT_TRUE(kept);
  EXPECT_TRUE(kept.value());
}

}  // namespace
}  // namespace catalog_test
