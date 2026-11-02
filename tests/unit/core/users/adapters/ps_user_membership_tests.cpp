#include "tests/support/psn/fixtures.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>

namespace users_test {
namespace {

using libchiaking::AccountId;
using libchiaking::users::SignInState;
using libchiaking::users::Tier;
using libchiaking::users::Unanswered;
using Reply = Psn::Reply;

class PsUserMembership : public ::testing::Test {
 protected:
  void answers_two_tiers() {
    rig.fake.on(
        "GET", std::string{Psn::subscriptions_url},
        {Reply{200, Psn::subscription_entry("TIER_30", "S-PLUS3T03M-E008")},
         Reply{200, Psn::subscription_entry("TIER_20", "S-PLUS2T03M-E004")}});
  }

  SignedInRig rig;
};

TEST_F(PsUserMembership, FailedAnswerIsNotKept) {
  const auto user = rig.signed_in.as(rig.saved(7, "NPSSO-A", "app-a"));
  rig.fake.on(
      "GET", std::string{Psn::subscriptions_url},
      {Reply{400, "{}"},
       Reply{200, Psn::subscription_entry("TIER_30", "S-PLUS3T03M-E008")}});
  const auto failed = user->get_my_membership();
  EXPECT_FALSE(failed);
  EXPECT_EQ(rig.tiers(), 1);
  EXPECT_FALSE(user->membership().tier());
  const auto asked_again = user->get_my_membership();
  ASSERT_TRUE(asked_again);
  EXPECT_FALSE(asked_again.value().cached);
  EXPECT_EQ(rig.tiers(), 2);
  EXPECT_EQ(asked_again.value().membership.tier(), Tier::premium);
  const auto kept = user->get_my_membership();
  ASSERT_TRUE(kept);
  EXPECT_TRUE(kept.value().cached);
  EXPECT_EQ(rig.tiers(), 2);
}

TEST_F(PsUserMembership, ConcurrentQuestionsShareOneRequest) {
  const auto user = rig.signed_in.as(rig.saved(7, "NPSSO-A", "app-a"));
  std::atomic<bool> started{false};
  Reply slow{200, test_support::psn::Fixtures::text("get_subscriptions.json")};
  slow.during = [&started] {
    started = true;
    Runtime::nap(std::chrono::milliseconds{50});
  };
  rig.fake.on("GET", std::string{Psn::subscriptions_url}, {slow});
  std::optional<
      libchiaking::Expected<libchiaking::users::MembershipAnswer, Unanswered>>
      first;
  auto worker = libchiaking::platform::Thread::start(
                    Runtime::threading(), "test",
                    libchiaking::platform::ThreadRole::unspecified,
                    [&] { first.emplace(user->get_my_membership()); })
                    .value();
  while (!started) Runtime::nap(std::chrono::milliseconds{1});
  const auto second = user->get_my_membership();
  static_cast<void>(worker.join());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(*first);
  ASSERT_TRUE(second);
  EXPECT_EQ(rig.tiers(), 1);
  EXPECT_EQ(first->value().membership.tier(), Tier::premium);
  EXPECT_EQ(second.value().membership.tier(), Tier::premium);
  EXPECT_EQ(first->value().account, AccountId{7});
  EXPECT_EQ(second.value().account, AccountId{7});
  const auto third = user->get_my_membership();
  ASSERT_TRUE(third);
  EXPECT_TRUE(third.value().cached);
  EXPECT_EQ(rig.tiers(), 1);
  EXPECT_EQ(user->membership().tier(), Tier::premium);
  rig.clock.advance_ms(61 * 60 * 1000);
  rig.wall.advance_ms(1);
  EXPECT_FALSE(user->membership().tier());
}

TEST_F(PsUserMembership, IsNeverServedToAnotherAccount) {
  const auto first = rig.signed_in.as(rig.saved(1, "NPSSO-A", "app-a"));
  const auto second = rig.signed_in.as(rig.saved(2, "NPSSO-B", "app-b"));
  EXPECT_NE(first, second);
  EXPECT_EQ(rig.signed_in.users.everyone().size(), 2U);
  EXPECT_EQ(rig.signed_in.users.find(AccountId{1}), first);
  EXPECT_EQ(rig.signed_in.users.find(AccountId{2}), second);
  EXPECT_FALSE(rig.signed_in.users.find(AccountId{3}));
  answers_two_tiers();
  const auto premium = first->get_my_membership();
  ASSERT_TRUE(premium);
  EXPECT_EQ(premium.value().account, AccountId{1});
  EXPECT_EQ(premium.value().membership.tier(), Tier::premium);
  EXPECT_EQ(rig.fake.seen.back().header("Authorization"), "Bearer app-a");
  const auto extra = second->get_my_membership();
  ASSERT_TRUE(extra);
  EXPECT_FALSE(extra.value().cached);
  EXPECT_EQ(extra.value().account, AccountId{2});
  EXPECT_EQ(extra.value().membership.tier(), Tier::extra);
  EXPECT_EQ(rig.tiers(), 2);
  EXPECT_EQ(rig.fake.seen.back().header("Authorization"), "Bearer app-b");
  const auto again = first->get_my_membership();
  ASSERT_TRUE(again);
  EXPECT_TRUE(again.value().cached);
  EXPECT_EQ(again.value().account, AccountId{1});
  EXPECT_EQ(again.value().membership.tier(), Tier::premium);
  EXPECT_EQ(rig.tiers(), 2);
  EXPECT_EQ(first->membership().tier(), Tier::premium);
  EXPECT_EQ(second->membership().tier(), Tier::extra);

  const auto replacement = rig.signed_in.as(rig.saved(1, "NPSSO-A", "app-a"));
  EXPECT_EQ(replacement, first);
  EXPECT_EQ(rig.signed_in.users.everyone().size(), 2U);
  EXPECT_TRUE(rig.signed_in.users.sign_out(AccountId{1}));
  const auto returned = rig.signed_in.as(rig.saved(1, "NPSSO-C", "app-c"));
  EXPECT_NE(returned, first);
  EXPECT_EQ(first->sign_in_state(), SignInState::signed_out);
  EXPECT_FALSE(returned->membership().tier());
  const auto asked = first->get_my_membership();
  ASSERT_FALSE(asked);
  EXPECT_EQ(asked.error().reason, Unanswered::Reason::signed_out);
  EXPECT_EQ(rig.tiers(), 2);
}

}  // namespace
}  // namespace users_test
