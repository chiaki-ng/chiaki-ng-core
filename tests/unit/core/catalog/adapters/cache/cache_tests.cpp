#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/cache/shelf_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <string_view>

namespace catalog_test {
namespace {

namespace cache = libchiaking::catalog::cache;
using Reason = libchiaking::catalog::Unanswered::Reason;

class CacheRecordNames : public ::testing::Test, protected CacheCases {};

TEST_F(CacheRecordNames, AreLowercaseWithoutDots) {
  EXPECT_EQ(cache::StreamableListRecord::name_of(locale("en-GB"))->text(),
            "streamable/gb-en");
  EXPECT_EQ(cache::StreamableListRecord::name_of(locale("pt-BR"))->text(),
            "streamable/br-pt");
  EXPECT_EQ(cache::PremiumCategoryRecord::name_of(locale("en-GB"))->text(),
            "included/en-gb");
  EXPECT_EQ(cache::LibraryRecord::name_of(first_account)->text(),
            "user/1111111111111111111/library");
  EXPECT_EQ(cache::RefusalRecord::name_of(first_account)->text(),
            "user/1111111111111111111/refusals");
  EXPECT_EQ(
      cache::LibraryRecord::name_of(AccountId{18446744073709551615ULL})->text(),
      "user/18446744073709551615/library");
}

class CacheStartOver : public ::testing::Test, protected CacheCases {
 protected:
  CacheShelf shelf;
};

TEST_F(CacheStartOver, ErasesAnAbsentRecord) {
  EXPECT_TRUE(shelf.cache.erase<cache::StreamableListRecord>(locale("en-GB")));
}

TEST_F(CacheStartOver, ErasesOnlyTheNamedRecord) {
  ASSERT_TRUE(shelf.cache.write(
      *cache::StreamableListRecord::of(streamable(shelf.now()))));
  ASSERT_TRUE(shelf.cache.write(
      cache::LibraryRecord{library(first_account, shelf.now())}));
  EXPECT_TRUE(shelf.cache.erase<cache::StreamableListRecord>(locale("en-GB")));
  EXPECT_FALSE(shelf.has("streamable/gb-en"));
  EXPECT_TRUE(shelf.has("user/1111111111111111111/library"));
  EXPECT_TRUE(shelf.cache.erase<cache::LibraryRecord>(first_account));
  EXPECT_TRUE(shelf.store.names().empty());
}

class CacheOldFiles : public ::testing::Test {
 protected:
  static auto lists(std::string_view name) -> ::testing::AssertionResult {
    if (std::ranges::find(cache::Cache::old_file_names, name) !=
        cache::Cache::old_file_names.end())
      return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << name << " is not among the old file names";
  }

  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryRecordStore store{threading};
  libchiaking::testing::ManualClock clock;
};

TEST_F(CacheOldFiles, NameEveryCacheFileTheOldCatalogWrote) {
  EXPECT_EQ(cache::Cache::old_file_names.size(), 17U);
  for (const std::string_view current :
       {"unified_catalog_v4.json", "ps5_cloud_catalog_v8.json",
        "ps5_cloud_library_v2.json", "ps_plus_store_tags_v2.json",
        "ps_plus_store_tags_failure.txt"})
    EXPECT_TRUE(lists(current));
  for (const std::string_view legacy :
       {"ps5_cloud_catalog_v7.json", "unified_catalog_v3.json",
        "ps5_cloud_catalog_v6.json", "ps5_cloud_library.json",
        "psnow_catalog.json", "unified_catalog_v2.json",
        "unified_catalog_v1.json", "ps5_cloud_catalog_v5.json",
        "ps5_cloud_catalog_v4.json", "ps5_cloud_catalog_v3.json",
        "ps5_cloud_catalog_v2.json", "ps5_cloud_catalog.json"})
    EXPECT_TRUE(lists(legacy));
}

TEST_F(CacheOldFiles, AreErasedOnOpenAndNeverRead) {
  for (const auto name : cache::Cache::old_file_names)
    store.legacy_files.emplace(name);
  store.legacy_files.emplace("settings.toml");
  const auto kept = *libchiaking::platform::RecordName::parse(
      "user/1111111111111111111/library");
  const std::string_view bytes = "kept";
  ASSERT_TRUE(store.write_atomic(
      kept, std::as_bytes(std::span{bytes.data(), bytes.size()})));

  cache::Cache opened{store, clock};
  EXPECT_EQ(store.legacy_files.size(), 1U);
  EXPECT_TRUE(store.legacy_files.contains("settings.toml"));
  EXPECT_EQ(store.reads, 0U);
  EXPECT_EQ(store.writes, 1U);
  ASSERT_EQ(store.names().size(), 1U);
  EXPECT_EQ(store.names().front(), kept);

  cache::Cache opened_again{store, clock};
  EXPECT_EQ(store.legacy_files.size(), 1U);
  EXPECT_EQ(store.reads, 0U);
}

class CacheWithoutStorage : public ::testing::Test, protected CacheCases {
 protected:
  BrokenStore store;
  libchiaking::testing::ManualClock clock;
  cache::Cache cache{store, clock};
};

TEST_F(CacheWithoutStorage, LeavesEveryReadUnansweredForStorage) {
  const auto list = cache.read<cache::StreamableListRecord>(locale("en-GB"));
  ASSERT_FALSE(list);
  EXPECT_EQ(list.error().reason(), Reason::storage);
  EXPECT_FALSE(list.error().is_cancelled());
  const auto owned_titles = cache.read<cache::LibraryRecord>(first_account);
  ASSERT_FALSE(owned_titles);
  EXPECT_EQ(owned_titles.error().reason(), Reason::storage);
  const auto tags = cache.read<cache::PremiumCategoryRecord>(locale("en-GB"));
  ASSERT_FALSE(tags);
  EXPECT_EQ(tags.error().reason(), Reason::storage);
  const auto refused = cache.read<cache::RefusalRecord>(first_account);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().reason(), Reason::storage);
}

TEST_F(CacheWithoutStorage, LeavesEveryChangeUnansweredForStorage) {
  const auto written =
      cache.write(cache::LibraryRecord{library(first_account, cache.now())});
  ASSERT_FALSE(written);
  EXPECT_EQ(written.error().reason(), Reason::storage);
  const auto erased = cache.erase<cache::LibraryRecord>(first_account);
  ASSERT_FALSE(erased);
  EXPECT_EQ(erased.error().reason(), Reason::storage);
  const auto forgotten = cache.forget(first_account);
  ASSERT_FALSE(forgotten);
  EXPECT_EQ(forgotten.error().reason(), Reason::storage);
}

TEST(CacheWithFailingWrites, LeavesAWriteUnansweredForStorage) {
  CacheShelf shelf;
  shelf.store.fail_writes = true;
  const auto refused_write = shelf.cache.write(
      CacheCases::refusals(CacheCases::first_account, shelf.now()));
  ASSERT_FALSE(refused_write);
  EXPECT_EQ(refused_write.error().reason(), Reason::storage);
  EXPECT_TRUE(shelf.store.names().empty());
}

}  // namespace
}  // namespace catalog_test
