#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/cache/shelf_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>

namespace catalog_test {
namespace {

namespace cache = libchiaking::catalog::cache;

class CachedPremiumCategory : public ::testing::Test, protected CacheCases {
 protected:
  static auto includes(
      const libchiaking::catalog::cloud_stream::PremiumCategory& category,
      std::string_view number) -> bool {
    return std::ranges::any_of(
        category.products(), [number](const auto& offered) {
          return offered.id().title_number() ==
                     libchiaking::catalog::TitleNumber::from(number) &&
                 offered.included_offer().has_value();
        });
  }

  CacheShelf shelf;
  libchiaking::catalog::StoreLocale gb{locale("en-GB")};
  libchiaking::catalog::cloud_stream::PremiumCategory titles{
      included(gb, shelf.now() - std::chrono::hours{2})};
  cache::PremiumCategoryRecord record{
      cache::PremiumCategoryRecord::nothing_held(gb).with_answer(titles,
                                                                 shelf.now())};
};

TEST_F(CachedPremiumCategory, RoundTripsWithEveryProduct) {
  EXPECT_EQ(record.category(), titles);
  EXPECT_FALSE(record.failed_at());
  ASSERT_TRUE(shelf.cache.write(record));
  const auto read = shelf.cache.read<cache::PremiumCategoryRecord>(gb);
  ASSERT_TRUE(read);
  ASSERT_TRUE(read.value());
  EXPECT_EQ(*read.value(), record);
  ASSERT_TRUE(read.value()->category());
  const auto& back = *read.value()->category();
  EXPECT_EQ(back.products().size(), 90U);
  EXPECT_EQ(back.locale(), gb);
  EXPECT_EQ(back.fetched_at(), titles.fetched_at());
  EXPECT_TRUE(includes(back, "PPSA10000"));
  EXPECT_FALSE(includes(back, "PPSA27001"));
}

TEST_F(CachedPremiumCategory, IsKeptByRegionWithoutAnAccount) {
  ASSERT_TRUE(shelf.cache.write(record));
  EXPECT_NE(shelf.text("included/en-gb").find(R"("region":"en-GB")"),
            std::string::npos);
  EXPECT_EQ(shelf.text("included/en-gb").find("\"account\""),
            std::string::npos);
}

TEST_F(CachedPremiumCategory, KeepsAFailureBesideTheAnswer) {
  ASSERT_TRUE(shelf.cache.write(record));
  const auto failed = record.with_failure(shelf.now());
  ASSERT_TRUE(shelf.cache.write(failed));
  const auto read = shelf.cache.read<cache::PremiumCategoryRecord>(gb);
  ASSERT_TRUE(read);
  ASSERT_TRUE(read.value());
  EXPECT_EQ(*read.value(), failed);
  EXPECT_EQ(read.value()->failed_at(), shelf.now());
  EXPECT_EQ(read.value()->category(), titles);
}

TEST_F(CachedPremiumCategory, KeepsAFailureWithoutAnAnswer) {
  const auto only_failure =
      cache::PremiumCategoryRecord::nothing_held(gb).with_failure(shelf.now());
  ASSERT_TRUE(shelf.cache.write(only_failure));
  const auto read = shelf.cache.read<cache::PremiumCategoryRecord>(gb);
  ASSERT_TRUE(read);
  ASSERT_TRUE(read.value());
  EXPECT_EQ(*read.value(), only_failure);
  EXPECT_FALSE(read.value()->category());
}

TEST_F(CachedPremiumCategory, IsWrittenByteForByte) {
  ASSERT_TRUE(shelf.cache.write(
      cache::PremiumCategoryRecord::nothing_held(gb)
          .with_answer(included(gb, shelf.now() - std::chrono::hours{2}, 80),
                       shelf.now())
          .with_failure(shelf.now())
          .for_age(34)));
  std::string products;
  for (int index = 0; index < 80; ++index) {
    const auto id = "EP1-PPSA" + std::to_string(10000 + index) + "_00-GAME";
    products +=
        (index == 0 ? "" : ",") + std::string{R"({"id":")"} + id +
        R"(","name":"Game","masterImage":"","skus":[{"id":")" + id +
        R"(-E001","branded":true,"entitlements":[{"id":")" + id +
        R"(","unreadableId":false,"packageType":"PSGD","featureType":"full_game",)"
        R"("streamingSupported":true}]}]})";
  }
  EXPECT_EQ(
      shelf.text("included/en-gb"),
      R"({"record":"included","schema":2,"writtenAtMs":1800000000000,"region":"en-GB",)"
      R"("body":{"fetchedAtMs":1799992800000,"scan":{"serverTotal":80,"productsRead":80,)"
      R"("complete":true},"products":[)" +
          products + R"(],"failedAtMs":1800000000000,"age":34}})");
}

}  // namespace
}  // namespace catalog_test
