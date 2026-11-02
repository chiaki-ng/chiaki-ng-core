#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <chrono>

namespace catalog_test {
namespace {

using libchiaking::catalog::CloudAccess;

class CloudAccessAnswers : public ValueCases {};

TEST_F(CloudAccessAnswers, CarryTheListing) {
  const auto access = CloudAccess::available(
      account, {120, true, libchiaking::catalog::ListOrigin::account_service},
      at_ms(1000));
  EXPECT_TRUE(access.is_available());
  ASSERT_TRUE(access.listing());
  EXPECT_EQ(access.listing()->title_count, 120U);
  EXPECT_FALSE(access.has_no_titles());
  EXPECT_TRUE(CloudAccess::available(account, {}, at_ms(1000)).has_no_titles());
}

TEST_F(CloudAccessAnswers, AreFreshForTheirLifetime) {
  const auto access = CloudAccess::available(
      account, {120, true, libchiaking::catalog::ListOrigin::account_service},
      at_ms(1000));
  EXPECT_TRUE(access.is_fresh_at(at_ms(1000)));
  EXPECT_FALSE(access.is_fresh_at(
      at_ms(1000) + std::chrono::duration_cast<std::chrono::milliseconds>(
                        CloudAccess::lifetime)));
}

TEST_F(CloudAccessAnswers, NeedAListingWhenAvailable) {
  EXPECT_FALSE(CloudAccess::without_listing(
      account, CloudAccess::Availability::available, at_ms(1000)));
}

TEST_F(CloudAccessAnswers, CarryAnUnavailableReasonWithoutAListing) {
  const auto expired = CloudAccess::without_listing(
      account, CloudAccess::Availability::sign_in_expired, at_ms(1000));
  ASSERT_TRUE(expired);
  EXPECT_FALSE(expired->is_available());
  EXPECT_FALSE(expired->listing());
  EXPECT_FALSE(expired->has_no_titles());
  EXPECT_EQ(expired->availability(),
            CloudAccess::Availability::sign_in_expired);
  EXPECT_EQ(expired->account(), account);
}

}  // namespace
}  // namespace catalog_test
