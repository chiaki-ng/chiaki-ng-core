#include "tests/unit/core/catalog/adapters/cache/category_record_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace catalog_test {
namespace {

using libchiaking::catalog::cache::ForSaleCategoryRecord;
using libchiaking::catalog::cache::PremiumCategoryRecord;
using libchiaking::catalog::cloud_stream::ForSaleCategory;
using libchiaking::catalog::cloud_stream::PremiumEntitlement;
using libchiaking::catalog::cloud_stream::PremiumProduct;
using libchiaking::catalog::cloud_stream::PremiumSku;
using Demand = CategoryRecordLoader::Demand;
using Step = CategoryRecordLoader::Step;
using Time = CategoryRecordLoader::Time;

class ForSaleCategoryRecords : public ::testing::Test {
 protected:
  CategoryRecordLoader loader;
};

TEST_F(ForSaleCategoryRecords, RoundTripOnTheirOwnShelf) {
  const auto gb = CategoryRecordLoader::locale("en-GB");
  const auto fetched_at = loader.now();
  const auto sold =
      CategoryRecordLoader::for_sale(gb, fetched_at,
                                     {CategoryRecordLoader::game_for_sale(1),
                                      CategoryRecordLoader::game_for_sale(2)});
  const auto entry =
      ForSaleCategoryRecord::nothing_held(gb).for_age(34).with_answer(
          sold, fetched_at);
  ASSERT_TRUE(entry.category());
  EXPECT_EQ(*entry.category(), sold);
  EXPECT_FALSE(entry.failed_at());
  EXPECT_TRUE(loader.cache.write(entry));
  EXPECT_EQ(ForSaleCategoryRecord::kind, "for_sale");
  EXPECT_EQ(ForSaleCategoryRecord::name_of(gb)->text(), "for_sale/en-gb");
  EXPECT_EQ(ForSaleCategoryRecord::lifetime, PremiumCategoryRecord::lifetime);
  EXPECT_EQ(ForSaleCategoryRecord::failure_back_off,
            PremiumCategoryRecord::failure_back_off);
  const auto read = loader.cache.read<ForSaleCategoryRecord>(gb);
  ASSERT_TRUE(read);
  ASSERT_TRUE(read.value());
  EXPECT_EQ(*read.value(), entry);
  EXPECT_EQ(read.value()->age(), 34);
  ASSERT_TRUE(read.value()->category());
  EXPECT_EQ(read.value()->category()->products()[1].facts().name, "Sold 2");
  EXPECT_EQ(read.value()->next_step(fetched_at, Demand::when_stale),
            Step::serve);
  const auto premium = loader.cache.read<PremiumCategoryRecord>(gb);
  ASSERT_TRUE(premium);
  EXPECT_FALSE(premium.value());
  EXPECT_EQ(loader.store.names().size(), 1U);
}

TEST_F(ForSaleCategoryRecords, IncludeNoCategoryWithoutAStreamableFullGame) {
  const auto gb = CategoryRecordLoader::locale("en-GB");
  const Time fetched_at{std::chrono::milliseconds{5'000}};
  auto trial = CategoryRecordLoader::game_for_sale(3);
  auto facts = trial.facts();
  auto entitlements = facts.skus.front().entitlements();
  auto entitlement = entitlements.front().facts();
  entitlement.feature = PremiumEntitlement::Feature::trial;
  facts.skus = {PremiumSku{
      facts.skus.front().id(), false, {PremiumEntitlement{entitlement}}}};
  const auto empty =
      CategoryRecordLoader::for_sale(gb, fetched_at, {PremiumProduct{facts}});
  const auto record =
      ForSaleCategoryRecord::nothing_held(gb).with_answer(empty, fetched_at);
  EXPECT_FALSE(record.category());
  EXPECT_EQ(record.failed_at(), fetched_at);
}

TEST_F(ForSaleCategoryRecords, RefuseAnUnfinishedScan) {
  const auto gb = CategoryRecordLoader::locale("en-GB");
  const Time fetched_at{std::chrono::milliseconds{5'000}};
  auto partial = CategoryRecordLoader::for_sale(
      gb, fetched_at, {CategoryRecordLoader::game_for_sale(4)});
  const ForSaleCategory unfinished{
      gb,
      partial.products(),
      {.server_total = 9, .products_read = 1, .complete = false},
      fetched_at};
  EXPECT_FALSE(ForSaleCategoryRecord::nothing_held(gb).accepts(unfinished));
}

}  // namespace
}  // namespace catalog_test
