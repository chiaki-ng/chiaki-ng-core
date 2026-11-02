#include "src/core/catalog/model/retry_policy.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace catalog_test {
namespace {

using libchiaking::catalog::RetryPolicy;

TEST(RetryPolicy, AsksAShelfTwiceHalfASecondApart) {
  constexpr auto shelf = RetryPolicy::shelf();
  EXPECT_EQ(shelf.attempts(), 2);
  EXPECT_EQ(shelf.pause(), std::chrono::milliseconds{500});
  EXPECT_TRUE(shelf.asks_again(1));
  EXPECT_FALSE(shelf.asks_again(2));
}

TEST(RetryPolicy, AsksACategoryPageThreeTimesAtOnce) {
  constexpr auto page = RetryPolicy::category_page();
  EXPECT_EQ(page.attempts(), 3);
  EXPECT_EQ(page.pause(), std::chrono::milliseconds{0});
  EXPECT_TRUE(page.asks_again(2));
  EXPECT_FALSE(page.asks_again(3));
}

}  // namespace
}  // namespace catalog_test
