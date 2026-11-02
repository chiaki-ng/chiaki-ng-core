#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/cache/shelf_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>

namespace catalog_test {
namespace {

namespace cache = libchiaking::catalog::cache;

class CacheAccounts : public ::testing::Test, protected CacheCases {
 protected:
  static constexpr std::string_view my_library =
      "user/1111111111111111111/library";
  static constexpr std::string_view my_refusals =
      "user/1111111111111111111/refusals";
  static constexpr std::string_view their_library =
      "user/2222222222222222222/library";
  static constexpr std::string_view their_refusals =
      "user/2222222222222222222/refusals";

  void SetUp() override {
    ASSERT_TRUE(shelf.cache.write(cache::LibraryRecord{first_library}));
    ASSERT_TRUE(shelf.cache.write(refusals(first_account, shelf.now())));
  }

  CacheShelf shelf;
  libchiaking::catalog::Library first_library{
      library(first_account, shelf.now())};
  libchiaking::catalog::Library second_library{
      libchiaking::catalog::Library::of(
          second_account, {}, {}, shelf.now() + std::chrono::milliseconds{1})
          .value()};
};

TEST_F(CacheAccounts, NeverServeOneAccountsAnswerToAnother) {
  const auto other_library =
      shelf.cache.read<cache::LibraryRecord>(second_account);
  const auto other_refusals =
      shelf.cache.read<cache::RefusalRecord>(second_account);
  ASSERT_TRUE(other_library);
  EXPECT_FALSE(other_library.value());
  ASSERT_TRUE(other_refusals);
  EXPECT_FALSE(other_refusals.value());
  EXPECT_EQ(shelf.store.names().size(), 2U);
}

TEST_F(CacheAccounts, KeepEachAccountsLibraryApart) {
  ASSERT_TRUE(shelf.cache.write(cache::LibraryRecord{second_library}));
  const auto mine = shelf.cache.read<cache::LibraryRecord>(first_account);
  const auto theirs = shelf.cache.read<cache::LibraryRecord>(second_account);
  ASSERT_TRUE(mine);
  ASSERT_TRUE(mine.value());
  EXPECT_EQ(mine.value()->library(), first_library);
  EXPECT_EQ(mine.value()->library().account(), first_account);
  EXPECT_EQ(mine.value()->library().size(), 4U);
  ASSERT_TRUE(theirs);
  ASSERT_TRUE(theirs.value());
  EXPECT_EQ(theirs.value()->library(), second_library);
  EXPECT_EQ(theirs.value()->library().account(), second_account);
  EXPECT_EQ(theirs.value()->library().size(), 0U);
}

TEST_F(CacheAccounts, EraseAnotherAccountsRecordsFiledUnderTheirName) {
  ASSERT_TRUE(shelf.cache.write(cache::LibraryRecord{second_library}));
  shelf.put(their_library, shelf.text(my_library));
  shelf.put(their_refusals, shelf.text(my_refusals));
  const auto theirs = shelf.cache.read<cache::LibraryRecord>(second_account);
  const auto other_refusals =
      shelf.cache.read<cache::RefusalRecord>(second_account);
  ASSERT_TRUE(theirs);
  EXPECT_FALSE(theirs.value());
  ASSERT_TRUE(other_refusals);
  EXPECT_FALSE(other_refusals.value());
  EXPECT_FALSE(shelf.has(their_library));
  EXPECT_FALSE(shelf.has(their_refusals));
  const auto mine = shelf.cache.read<cache::LibraryRecord>(first_account);
  const auto my_answers = shelf.cache.read<cache::RefusalRecord>(first_account);
  ASSERT_TRUE(mine);
  EXPECT_TRUE(mine.value());
  ASSERT_TRUE(my_answers);
  ASSERT_TRUE(my_answers.value());
  EXPECT_EQ(my_answers.value()->refusals().size(), 3U);
}

TEST_F(CacheAccounts, ForgetOnlyTheForgottenAccount) {
  ASSERT_TRUE(shelf.cache.write(cache::LibraryRecord{second_library}));
  EXPECT_TRUE(shelf.cache.forget(first_account));
  const auto mine = shelf.cache.read<cache::LibraryRecord>(first_account);
  const auto theirs = shelf.cache.read<cache::LibraryRecord>(second_account);
  ASSERT_TRUE(mine);
  EXPECT_FALSE(mine.value());
  ASSERT_TRUE(theirs);
  EXPECT_TRUE(theirs.value());
  EXPECT_EQ(shelf.store.names().size(), 1U);
  EXPECT_TRUE(shelf.cache.forget(first_account));
}

class CacheAccountlessRecords : public ::testing::Test, protected CacheCases {
 protected:
  static constexpr std::string_view name = "user/1111111111111111111/library";

  void SetUp() override {
    ASSERT_TRUE(shelf.cache.write(
        cache::LibraryRecord{library(first_account, shelf.now())}));
    text = shelf.text(name);
  }

  auto erased_after(const std::string& damaged) -> ::testing::AssertionResult {
    shelf.put(name, damaged);
    const auto read = shelf.cache.read<cache::LibraryRecord>(first_account);
    if (!read) return ::testing::AssertionFailure() << "the read failed";
    if (read.value()) return ::testing::AssertionFailure() << "it was served";
    if (!shelf.store.names().empty())
      return ::testing::AssertionFailure() << "it was not erased";
    return ::testing::AssertionSuccess();
  }

  CacheShelf shelf;
  std::string text;
};

TEST_F(CacheAccountlessRecords, AreErasedWithoutAnAccount) {
  EXPECT_TRUE(
      erased_after(replaced(text, R"("account":"1111111111111111111",)", "")));
}

TEST_F(CacheAccountlessRecords, AreErasedWithANumericAccount) {
  EXPECT_TRUE(erased_after(replaced(text, R"("account":"1111111111111111111")",
                                    R"("account":1111111111111111111)")));
}

TEST_F(CacheAccountlessRecords, AreErasedWithARegionInsteadOfAnAccount) {
  EXPECT_TRUE(erased_after(replaced(text, R"("account":"1111111111111111111")",
                                    R"("region":"en-GB")")));
}

}  // namespace
}  // namespace catalog_test
