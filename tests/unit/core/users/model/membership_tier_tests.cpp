#include "tests/support/enum_printing.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <optional>

namespace users_test {
namespace {

using libchiaking::users::Membership;
using libchiaking::users::Tier;

TEST(MembershipTier, UnknownIsNeitherATierNorAnAnswer) {
  const Membership unknown;
  EXPECT_FALSE(unknown.tier());
  EXPECT_FALSE(unknown.is_ps_plus_member());
}

TEST(MembershipTier, MemberOfUnknownTierIsAMember) {
  const auto member = Membership::member_of_unknown_tier();
  EXPECT_FALSE(member.tier());
  EXPECT_EQ(member.is_ps_plus_member(), true);
}

TEST(MembershipTier, NoneIsNotAMemberAndHasNoPlan) {
  EXPECT_EQ(Membership{Tier::none}.is_ps_plus_member(), false);
  EXPECT_TRUE((Membership{Tier::none, {.tier_label = "TIER_30"}}
                   .plan()
                   .tier_label.empty()));
}

TEST(MembershipTier, EveryPaidTierIsAMember) {
  for (const auto tier : {Tier::essential, Tier::extra, Tier::premium}) {
    EXPECT_EQ(Membership{tier}.is_ps_plus_member(), true);
    EXPECT_EQ(Membership{tier}.tier(), tier);
  }
}

TEST(MembershipTier, IsNamed) {
  EXPECT_EQ(Membership::name_of(Tier::none), "none");
  EXPECT_EQ(Membership::name_of(Tier::essential), "essential");
  EXPECT_EQ(Membership::name_of(Tier::extra), "extra");
  EXPECT_EQ(Membership::name_of(Tier::premium), "premium");
  EXPECT_EQ(Membership::name_of(std::nullopt), "unknown");
}

}  // namespace
}  // namespace users_test
