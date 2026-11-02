#include "tests/unit/core/catalog/rule_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <string>

namespace catalog_test {
namespace {

using libchiaking::catalog::OwnershipId;
using libchiaking::catalog::TitleKey;
using libchiaking::catalog::TitleNumber;

class OwnershipIds : public ::testing::Test, protected RuleCases {};

TEST_F(OwnershipIds, KnowTheTitleAndProductTheyName) {
  const auto owned = OwnershipId::parse("EP0109-PPSA07028_00-FAKE0110TITLE000");
  ASSERT_TRUE(owned);
  EXPECT_EQ(owned->title_number(), *TitleNumber::from("PPSA07028"));
  EXPECT_EQ(owned->title_key(), *TitleKey::of("PPSA07028"));
  EXPECT_EQ(owned->as_product(),
            product("EP0109-PPSA07028_00-FAKE0110TITLE000"));
}

TEST_F(OwnershipIds, RefuseEmptyOrSpacedOrOverlongText) {
  EXPECT_FALSE(OwnershipId::parse(""));
  EXPECT_FALSE(OwnershipId::parse("has space"));
  EXPECT_FALSE(OwnershipId::parse("a b"));
  EXPECT_FALSE(OwnershipId::parse("line\nbreak"));
  EXPECT_FALSE(OwnershipId::parse(std::string(OwnershipId::limit + 1, 'A')));
}

TEST_F(OwnershipIds, AcceptOpaqueTextThatIsNoProduct) {
  const auto odd = OwnershipId::parse("odd:id.with+marks");
  ASSERT_TRUE(odd);
  EXPECT_FALSE(odd->as_product());
}

}  // namespace
}  // namespace catalog_test
