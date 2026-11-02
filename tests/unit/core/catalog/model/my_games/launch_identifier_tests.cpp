#include "tests/unit/core/catalog/rule_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

namespace catalog_test {
namespace {

using libchiaking::catalog::LaunchIdentifier;
using libchiaking::catalog::OwnershipId;
using libchiaking::catalog::TitleNumber;

class LaunchIdentifiers : public ::testing::Test, protected RuleCases {};

TEST_F(LaunchIdentifiers, CarryTheProductText) {
  const auto by_product =
      LaunchIdentifier::of(product("EP0109-PPSA07028_00-FAKE0110TITLE000"));
  EXPECT_EQ(by_product.text(), "EP0109-PPSA07028_00-FAKE0110TITLE000");
  EXPECT_TRUE(by_product.is_ps5_native());
  EXPECT_EQ(by_product.title_number(), *TitleNumber::from("PPSA07028"));
}

TEST_F(LaunchIdentifiers, AreTheSameForAProductAndItsOwnership) {
  const auto owned = OwnershipId::parse("EP0109-PPSA07028_00-FAKE0110TITLE000");
  ASSERT_TRUE(owned);
  EXPECT_EQ(
      LaunchIdentifier::of(*owned),
      LaunchIdentifier::of(product("EP0109-PPSA07028_00-FAKE0110TITLE000")));
}

TEST_F(LaunchIdentifiers, AreNotPs5NativeForAPs4Product) {
  EXPECT_FALSE(
      LaunchIdentifier::of(product("UP0114-CUSA07009_00-FAKE0051TITLE000"))
          .is_ps5_native());
}

}  // namespace
}  // namespace catalog_test
