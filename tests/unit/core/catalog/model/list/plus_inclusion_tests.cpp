#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

namespace catalog_test {
namespace {

using libchiaking::users::Membership;
using libchiaking::users::Tier;

class PlusInclusions : public ValueCases {};

TEST_F(PlusInclusions, AreNotMadeFromNoTier) {
  EXPECT_FALSE(PlusInclusion::from_tier(Tier::none, Source::store_tags));
}

TEST_F(PlusInclusions, KeepTheTierATitleNeeds) {
  const auto extra = *PlusInclusion::from_tier(Tier::extra, Source::store_tags);
  EXPECT_TRUE(extra.is_included());
  EXPECT_EQ(extra.lowest_tier(), Tier::extra);
  EXPECT_EQ(extra.source(), Source::store_tags);
  EXPECT_EQ(PlusInclusion::with_premium(Source::old_service).lowest_tier(),
            Tier::premium);
}

TEST_F(PlusInclusions, CoverMembershipsAtOrAboveTheTier) {
  const auto extra = *PlusInclusion::from_tier(Tier::extra, Source::store_tags);
  EXPECT_EQ(extra.covers(Membership{Tier::premium}), true);
  EXPECT_EQ(extra.covers(Membership{Tier::extra}), true);
  EXPECT_EQ(extra.covers(Membership{Tier::essential}), false);
  EXPECT_EQ(extra.covers(Membership{Tier::none}), false);
}

TEST_F(PlusInclusions, CannotSayForAnUnknownTier) {
  const auto extra = *PlusInclusion::from_tier(Tier::extra, Source::store_tags);
  EXPECT_FALSE(extra.covers(Membership{}).has_value());
  EXPECT_FALSE(extra.covers(Membership::member_of_unknown_tier()).has_value());
}

TEST_F(PlusInclusions, CoverNobodyWhenNotIncluded) {
  const auto absent = PlusInclusion::not_included(Source::store_lists);
  EXPECT_FALSE(absent.is_included());
  EXPECT_FALSE(absent.lowest_tier());
  EXPECT_EQ(absent.covers(Membership{}), false);
}

TEST_F(PlusInclusions, CombineToTheLowestTier) {
  const auto absent = PlusInclusion::not_included(Source::store_lists);
  const auto listed = PlusInclusion::with_premium(Source::store_lists);
  EXPECT_EQ(absent.combined_with(listed), listed);
  EXPECT_EQ(listed.combined_with(absent), listed);
  EXPECT_EQ(listed
                .combined_with(*PlusInclusion::from_tier(Tier::essential,
                                                         Source::store_lists))
                .lowest_tier(),
            Tier::essential);
}

TEST_F(PlusInclusions, LetUntaggedStoreTagsOverrideTheLists) {
  const auto listed = PlusInclusion::with_premium(Source::store_lists);
  const auto untagged = PlusInclusion::not_included(Source::store_tags);
  EXPECT_EQ(listed.combined_with(untagged), untagged);
  EXPECT_EQ(untagged.combined_with(listed), untagged);
}

TEST_F(PlusInclusions, StreamAtPremium) {
  EXPECT_EQ(libchiaking::catalog::streaming_tier, Tier::premium);
}

}  // namespace
}  // namespace catalog_test
