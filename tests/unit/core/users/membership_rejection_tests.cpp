#include "src/core/users/ports/membership_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <string>

namespace users_test {
namespace {

using libchiaking::users::MembershipCalls;
using libchiaking::users::SignInState;
using libchiaking::users::Tier;
using Reply = Psn::Reply;

TEST(RejectedMembership, IsForgottenAtOnce) {
  SignedInRig rig;
  const auto user = rig.signed_in.as(rig.saved(50, "NPSSO-A", "app-a"));
  rig.fake.on("GET", std::string{Psn::subscriptions_url},
              {Reply{200, Psn::subscription_entry(
                              "TIER_30", "IP9101-NPIA90006_00-PLUS3T12M")},
               Reply{200, Psn::subscription_entry(
                              "TIER_10", "IP9101-NPIA90006_00-PLUS1T12M")}});
  const auto premium = user->get_my_membership();
  ASSERT_TRUE(premium);
  EXPECT_EQ(premium.value().membership.tier(), Tier::premium);
  EXPECT_EQ(user->membership().tier(), Tier::premium);
  EXPECT_EQ(rig.fake.seen.size(), 1U);
  MembershipCalls calls{*user};
  EXPECT_TRUE(calls.remembered_membership());
  const auto unchanged = user->get_my_membership();
  ASSERT_TRUE(unchanged);
  EXPECT_EQ(unchanged.value().membership.tier(), Tier::premium);
  EXPECT_EQ(rig.fake.seen.size(), 1U);

  calls.membership_rejected_by_service();
  EXPECT_FALSE(calls.remembered_membership());
  EXPECT_FALSE(user->membership().tier());
  EXPECT_EQ(rig.fake.seen.size(), 1U);
  EXPECT_EQ(user->sign_in_state(), SignInState::signed_in);

  const auto asked_again = user->get_my_membership();
  ASSERT_TRUE(asked_again);
  EXPECT_EQ(asked_again.value().membership.tier(), Tier::essential);
  EXPECT_EQ(rig.fake.seen.size(), 2U);
  EXPECT_EQ(user->membership().tier(), Tier::essential);
}

}  // namespace
}  // namespace users_test
