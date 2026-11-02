#include "tests/unit/core/catalog/rule_cases.hpp"

#include <gtest/gtest.h>

namespace catalog_test {
namespace {

class StorefrontEdition : public ::testing::Test, public RuleCases {};

TEST_F(StorefrontEdition, AnswersOnlyForItsOwnProduct) {
  const auto asked = product("UP0112-PPSA01234_00-GAME000000000000-E001");
  EXPECT_TRUE(asked.is_answered_by(asked));
  EXPECT_TRUE(
      asked.is_answered_by(product("UP0112-PPSA01234_00-GAME000000000000")));
  EXPECT_FALSE(
      asked.is_answered_by(product("EP0100-PPSA01234_00-FAKE0086TITLE000")));
  EXPECT_FALSE(
      product("UP0112-PPSA01234_00-GAME000000000000").is_answered_by(asked));
}

TEST_F(StorefrontEdition, SharesTheTitleNumberOfOtherEditions) {
  EXPECT_EQ(product("UP0112-PPSA01234_00-GAME000000000000-E001").title_number(),
            product("EP0100-PPSA01234_00-FAKE0086TITLE000").title_number());
}

}  // namespace
}  // namespace catalog_test
