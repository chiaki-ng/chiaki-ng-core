#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "tests/unit/core/catalog/adapters/cache/category_record_rig.hpp"

#include <gtest/gtest.h>

namespace catalog_test {
namespace {

using libchiaking::catalog::HeldAnswers;
using libchiaking::catalog::cache::ForSaleCategoryRecord;

class HeldForSaleCategory : public ::testing::Test {
 protected:
  CategoryRecordLoader loader;
  HeldAnswers held{loader.store, loader.threading, loader.clock, loader.clock};
  libchiaking::catalog::StoreLocale gb = CategoryRecordLoader::locale("en-GB");
  libchiaking::catalog::cloud_stream::ForSaleCategory sold =
      CategoryRecordLoader::for_sale(gb, loader.now(),
                                     {CategoryRecordLoader::game_for_sale(1),
                                      CategoryRecordLoader::game_for_sale(2)});
};

TEST_F(HeldForSaleCategory, IsSharedRatherThanCopied) {
  const auto kept = held.keep(
      gb, 34,
      HeldAnswers::held_from(
          ForSaleCategoryRecord::nothing_held(gb).for_age(34).with_answer(
              sold, loader.now())));
  ASSERT_TRUE(kept);
  EXPECT_EQ(*kept, sold);
  const auto first = held.held_for_sale(gb, 34);
  const auto second = held.held_for_sale(gb, 34);
  EXPECT_EQ(first.category, kept);
  EXPECT_EQ(second.category, kept);
  EXPECT_EQ(first.record, second.record);
  ASSERT_TRUE(first.record);
  EXPECT_EQ(first.category.get(), &*first.record->category());
}

TEST_F(HeldForSaleCategory, IsSharedOnceReadBackFromTheStore) {
  ASSERT_TRUE(held.keep(
      gb, 34,
      HeldAnswers::held_from(
          ForSaleCategoryRecord::nothing_held(gb).for_age(34).with_answer(
              sold, loader.now()))));
  HeldAnswers reopened{loader.store, loader.threading, loader.clock,
                       loader.clock};
  const auto read = reopened.held_for_sale(gb, 34);
  const auto again = reopened.held_for_sale(gb, 34);
  ASSERT_TRUE(read.category);
  EXPECT_EQ(*read.category, sold);
  EXPECT_EQ(read.category, again.category);
  EXPECT_EQ(read.record, again.record);
  ASSERT_TRUE(read.record);
  EXPECT_EQ(read.category.get(), &*read.record->category());
}

}  // namespace
}  // namespace catalog_test
