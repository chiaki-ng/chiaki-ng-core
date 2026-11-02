#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

namespace catalog_test {
namespace {

using libchiaking::catalog::Explanation;
using libchiaking::catalog::HideReason;
using libchiaking::catalog::TitleNumber;
using libchiaking::users::Tier;

class Explanations : public ValueCases {
 protected:
  Explanations() {
    note.title_number = TitleNumber::from("PPSA07041");
    tier.hides = HideReason::tier_does_not_include;
    tier.tier = Tier::extra;
  }

  Explanation::Step listed{Explanation::Rule::listed_by_service,
                           Explanation::Outcome::passed};
  Explanation::Step note{Explanation::Rule::list_streamability,
                         Explanation::Outcome::noted};
  Explanation::Step tier{Explanation::Rule::tier, Explanation::Outcome::failed};
};

TEST_F(Explanations, ListAProductWhoseStepsAllPass) {
  const auto visible = Explanation::of(product(rivet), {listed, note});
  EXPECT_TRUE(visible.is_listed());
  EXPECT_EQ(visible.steps().size(), 2U);
  EXPECT_FALSE(visible.hide_reason());
  EXPECT_EQ(visible.first_failure(), nullptr);
  EXPECT_EQ(visible.product_id(), product(rivet));
}

TEST_F(Explanations, HideAProductAtItsFirstFailedStep) {
  const auto hidden = Explanation::of(product(rivet), {listed, tier, note});
  EXPECT_FALSE(hidden.is_listed());
  EXPECT_EQ(hidden.hide_reason(), HideReason::tier_does_not_include);
  ASSERT_NE(hidden.first_failure(), nullptr);
  EXPECT_EQ(hidden.first_failure()->tier, Tier::extra);
}

TEST_F(Explanations, FailAnUnlistedProductAtTheListing) {
  const auto unknown = Explanation::not_listed(product(bus));
  EXPECT_FALSE(unknown.is_listed());
  ASSERT_NE(unknown.first_failure(), nullptr);
  EXPECT_EQ(unknown.first_failure()->rule,
            Explanation::Rule::listed_by_service);
  EXPECT_FALSE(unknown.hide_reason());
}

}  // namespace
}  // namespace catalog_test
