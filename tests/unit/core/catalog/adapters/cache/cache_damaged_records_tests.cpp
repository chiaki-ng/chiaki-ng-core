#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/cache/shelf_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace catalog_test {
namespace {

namespace cache = libchiaking::catalog::cache;

class CacheDamagedRecords : public ::testing::Test, protected CacheCases {
 protected:
  static constexpr std::string_view library_name =
      "user/1111111111111111111/library";
  static constexpr std::string_view refusals_name =
      "user/1111111111111111111/refusals";

  template <class Record, class Key>
  auto erased_after(std::string_view name, const std::string& damaged,
                    const Key& key) -> ::testing::AssertionResult {
    shelf.put(name, damaged);
    const auto read = shelf.cache.read<Record>(key);
    if (!read) return ::testing::AssertionFailure() << "the read failed";
    if (read.value())
      return ::testing::AssertionFailure() << "it was served: " << damaged;
    if (!shelf.store.names().empty())
      return ::testing::AssertionFailure() << "it was not erased: " << damaged;
    return ::testing::AssertionSuccess();
  }

  auto library_erased_after(const std::string& damaged)
      -> ::testing::AssertionResult {
    return erased_after<cache::LibraryRecord>(library_name, damaged,
                                              first_account);
  }

  auto written_library() -> std::string {
    EXPECT_TRUE(shelf.cache.write(
        cache::LibraryRecord{library(first_account, shelf.now())}));
    return shelf.text(library_name);
  }

  CacheShelf shelf;
};

TEST_F(CacheDamagedRecords, AnUnreadableLibraryIsAbsentAndErased) {
  const auto good = written_library();
  EXPECT_TRUE(library_erased_after("{not json"));
  EXPECT_TRUE(library_erased_after(""));
  EXPECT_TRUE(library_erased_after("[]"));
  EXPECT_TRUE(library_erased_after("null"));
  EXPECT_TRUE(library_erased_after(std::string{"\xff\xfe\x00\x01", 4}));
  EXPECT_TRUE(library_erased_after(good.substr(0, good.size() / 2)));
}

TEST_F(CacheDamagedRecords, ALibraryOfAnotherSchemaIsAbsentAndErased) {
  const auto good = written_library();
  EXPECT_TRUE(
      library_erased_after(replaced(good, R"("schema":5)", R"("schema":4)")));
  EXPECT_TRUE(
      library_erased_after(replaced(good, R"("schema":5)", R"("schema":0)")));
  EXPECT_TRUE(
      library_erased_after(replaced(good, R"("schema":5)", R"("schema":"5")")));
  EXPECT_TRUE(library_erased_after(replaced(good, R"("schema":5,)", "")));
}

TEST_F(CacheDamagedRecords, ALibraryWithBadFieldsIsAbsentAndErased) {
  const auto good = written_library();
  EXPECT_TRUE(library_erased_after(replaced(good, R"("ps5Ownership":"fetched")",
                                            R"("ps5Ownership":"maybe")")));
  EXPECT_TRUE(
      library_erased_after(replaced(good, R"(,"ps5Ownership":"fetched")", "")));
  EXPECT_TRUE(library_erased_after(
      replaced(good, R"("record":"library")", R"("record":"refusals")")));
  EXPECT_TRUE(
      library_erased_after(replaced(good, R"("record":"library",)", "")));
  EXPECT_TRUE(library_erased_after(replaced(
      good, R"("writtenAtMs":1800000000000)", R"("writtenAtMs":"now")")));
  EXPECT_TRUE(library_erased_after(
      replaced(good, R"("writtenAtMs":1800000000000,)", "")));
  EXPECT_TRUE(
      library_erased_after(replaced(good, R"("body":{)", R"("payload":{)")));
  EXPECT_TRUE(library_erased_after(
      replaced(good, R"("heldAs":"full_game")", R"("heldAs":"rented")")));
  EXPECT_TRUE(library_erased_after(
      replaced(good, R"("platform":"ps5")", R"("platform":"PS5")")));
  EXPECT_TRUE(library_erased_after(
      replaced(good, R"("fetchedAtMs":)", R"("fetchedAt":)")));
  EXPECT_TRUE(library_erased_after(replaced(
      good, R"("id":"EP0109-PPSA07028_00-FAKE0110TITLE000")", R"("id":"")")));
  EXPECT_TRUE(library_erased_after(
      replaced(good, "\"name\":\"Owned \xC3\xA9\",\"platform\":\"ps4\"",
               "\"name\":\"Demo\",\"platform\":\"ps4\"")));
  EXPECT_TRUE(library_erased_after(
      replaced(good, R"("bundles":[)", R"("bundles":[7,)")));
}

TEST_F(CacheDamagedRecords, AnIntactLibraryIsStillServed) {
  const auto good = written_library();
  ASSERT_TRUE(library_erased_after("{not json"));
  shelf.put(library_name, good);
  const auto kept = shelf.cache.read<cache::LibraryRecord>(first_account);
  ASSERT_TRUE(kept);
  EXPECT_TRUE(kept.value());
}

TEST_F(CacheDamagedRecords, RefusalsFiledAsALibraryAreErasedAlone) {
  const auto good = written_library();
  ASSERT_TRUE(shelf.cache.write(refusals(first_account, shelf.now())));
  shelf.put(library_name, shelf.text(refusals_name));
  const auto crossed = shelf.cache.read<cache::LibraryRecord>(first_account);
  ASSERT_TRUE(crossed);
  EXPECT_FALSE(crossed.value());
  EXPECT_FALSE(shelf.has(library_name));
  EXPECT_TRUE(shelf.has(refusals_name));
}

TEST_F(CacheDamagedRecords, RefusalsWithBadWordsAreAbsentAndErased) {
  ASSERT_TRUE(shelf.cache.write(refusals(first_account, shelf.now())));
  const auto refused = shelf.text(refusals_name);
  EXPECT_TRUE(erased_after<cache::RefusalRecord>(
      refusals_name,
      replaced(refused, R"("tier":"premium")", R"("tier":"gold")"),
      first_account));
  EXPECT_TRUE(erased_after<cache::RefusalRecord>(
      refusals_name,
      replaced(refused, R"("kind":"no_game_for_entitlement")",
               R"("kind":"storefront")"),
      first_account));
}

TEST_F(CacheDamagedRecords, AListWithBadFieldsIsAbsentAndErased) {
  ASSERT_TRUE(shelf.cache.write(
      *cache::StreamableListRecord::of(streamable(shelf.now()))));
  const auto list = shelf.text("streamable/gb-en");
  for (const auto& damaged :
       {replaced(list, R"("service":"ps5_cloud")",
                 R"("service":"cloud_stream")"),
        replaced(list, R"("inclusionTier":"extra")",
                 R"("inclusionTier":"none")"),
        replaced(list, R"("settledLocale":"en-US")",
                 R"("settledLocale":"english")"),
        replaced(list, R"("conceptPage":"https://store.playstation.com/)",
                 R"("conceptPage":"https://example.com/)"),
        replaced(list, R"("markedUnstreamable":[)",
                 R"("markedUnstreamable":["nothing",)"),
        replaced(list, R"("product":"UP0114-CUSA07009_00-FAKE0051TITLE000")",
                 R"("product":"EP0109-PPSA07028_00-FAKE0110TITLE000")")})
    EXPECT_TRUE(erased_after<cache::StreamableListRecord>(
        "streamable/gb-en", damaged, locale("en-GB")));
}

TEST_F(CacheDamagedRecords, AnIncludedCategoryWithBadFieldsIsAbsentAndErased) {
  const auto gb = locale("en-GB");
  ASSERT_TRUE(shelf.cache.write(
      cache::PremiumCategoryRecord::nothing_held(gb).with_answer(
          included(gb, shelf.now()), shelf.now())));
  const auto tags = shelf.text("included/en-gb");
  for (const auto& damaged :
       {replaced(tags, R"("id":"EP1-PPSA10000_00-GAME")",
                 R"("id":"EP1 PPSA10000")"),
        replaced(tags, R"("branded":true)", R"("branded":"yes")"),
        replaced(tags, R"("complete":true)", R"("complete":false)"),
        replaced(tags, R"("products":[)", R"("products":{},"was":[)"),
        replaced(tags, R"("fetchedAtMs":)",
                 R"("failedAtMs":"soon","fetchedAtMs":)")})
    EXPECT_TRUE(erased_after<cache::PremiumCategoryRecord>("included/en-gb",
                                                           damaged, gb));
}

TEST_F(CacheDamagedRecords, AnEmptyIncludedCategoryIsAbsentAndErased) {
  const auto gb = locale("en-GB");
  ASSERT_TRUE(
      shelf.cache.write(cache::PremiumCategoryRecord::nothing_held(gb)));
  const auto empty = shelf.cache.read<cache::PremiumCategoryRecord>(gb);
  ASSERT_TRUE(empty);
  EXPECT_FALSE(empty.value());
  EXPECT_TRUE(shelf.store.names().empty());
}

}  // namespace
}  // namespace catalog_test
