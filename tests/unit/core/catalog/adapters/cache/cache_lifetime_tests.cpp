#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/cache/shelf_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string_view>

namespace catalog_test {
namespace {

namespace cache = libchiaking::catalog::cache;

class CacheLifetime : public ::testing::Test, protected CacheCases {
 protected:
  void write_all() {
    ASSERT_TRUE(shelf.cache.write(
        *cache::StreamableListRecord::of(streamable(shelf.now()))));
    ASSERT_TRUE(shelf.cache.write(
        cache::LibraryRecord{library(first_account, shelf.now())}));
    ASSERT_TRUE(shelf.cache.write(refusals(first_account, shelf.now())));
  }

  static constexpr std::string_view library_name =
      "user/1111111111111111111/library";

  CacheShelf shelf;
};

TEST_F(CacheLifetime, ServesAListALibraryAndRefusalsForADay) {
  ASSERT_NO_FATAL_FAILURE(write_all());
  const auto reads = shelf.store.reads;
  shelf.clock.advance(std::chrono::hours{24});
  const auto list =
      shelf.cache.read<cache::StreamableListRecord>(locale("en-GB"));
  const auto owned_titles =
      shelf.cache.read<cache::LibraryRecord>(first_account);
  const auto refused = shelf.cache.read<cache::RefusalRecord>(first_account);
  ASSERT_TRUE(list);
  EXPECT_TRUE(list.value());
  ASSERT_TRUE(owned_titles);
  EXPECT_TRUE(owned_titles.value());
  ASSERT_TRUE(refused);
  EXPECT_TRUE(refused.value());
  EXPECT_EQ(shelf.store.reads, reads + 3);
  EXPECT_EQ(shelf.store.names().size(), 3U);
}

TEST_F(CacheLifetime, ErasesEachRecordAMillisecondAfterADay) {
  ASSERT_NO_FATAL_FAILURE(write_all());
  shelf.clock.advance(std::chrono::hours{24} + std::chrono::milliseconds{1});
  const auto old_list =
      shelf.cache.read<cache::StreamableListRecord>(locale("en-GB"));
  ASSERT_TRUE(old_list);
  EXPECT_FALSE(old_list.value());
  EXPECT_FALSE(shelf.has("streamable/gb-en"));
  const auto old_library =
      shelf.cache.read<cache::LibraryRecord>(first_account);
  ASSERT_TRUE(old_library);
  EXPECT_FALSE(old_library.value());
  EXPECT_FALSE(shelf.has(library_name));
  const auto old_refusals =
      shelf.cache.read<cache::RefusalRecord>(first_account);
  ASSERT_TRUE(old_refusals);
  EXPECT_FALSE(old_refusals.value());
  EXPECT_TRUE(shelf.store.names().empty());
}

TEST_F(CacheLifetime, IgnoresAnOldFileTimeForAFreshRecord) {
  ASSERT_TRUE(shelf.cache.write(
      cache::LibraryRecord{library(first_account, shelf.now())}));
  shelf.store.modified_at =
      std::chrono::system_clock::time_point{std::chrono::seconds{1}};
  shelf.put(library_name, shelf.text(library_name));
  const auto read = shelf.cache.read<cache::LibraryRecord>(first_account);
  ASSERT_TRUE(read);
  EXPECT_TRUE(read.value());
}

TEST_F(CacheLifetime, IgnoresANewFileTimeForAStaleRecord) {
  ASSERT_TRUE(shelf.cache.write(
      cache::LibraryRecord{library(first_account, shelf.now())}));
  shelf.store.modified_at = std::chrono::system_clock::time_point{
      std::chrono::seconds{1'800'090'000}};
  shelf.put(library_name, shelf.text(library_name));
  shelf.clock.advance(std::chrono::hours{25});
  const auto read = shelf.cache.read<cache::LibraryRecord>(first_account);
  ASSERT_TRUE(read);
  EXPECT_FALSE(read.value());
}

TEST_F(CacheLifetime, ErasesARecordWrittenAfterTheClocksNow) {
  ASSERT_TRUE(shelf.cache.write(
      cache::LibraryRecord{library(first_account, shelf.now())}));
  shelf.clock.now_us -= 1'000;
  const auto read = shelf.cache.read<cache::LibraryRecord>(first_account);
  ASSERT_TRUE(read);
  EXPECT_FALSE(read.value());
  EXPECT_TRUE(shelf.store.names().empty());
}

}  // namespace
}  // namespace catalog_test
