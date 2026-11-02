#include "src/core/psn/auth/credentials.hpp"
#include "tests/support/psn/fixtures.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <string>

namespace users_test {
namespace {

namespace users = libchiaking::users;
namespace psn = libchiaking::psn;
using libchiaking::AccountId;
using users::SignInState;
using users::Tier;
using Reply = Psn::Reply;
using Refusal = users::SignedInUsers::SignInAgainRefusal;

class SignIn : public ::testing::Test {
 protected:
  static auto signs_in(SignedInRig& at) {
    return at.signed_in.users.sign_in(psn::auth::Npsso{"synthetic-npsso"},
                                      Psn::own_device(), Psn::correlation);
  }

  static void refuses_the_authorization(SignedInRig& at) {
    at.fake.on("GET", std::string{Psn::remote_authorize_url},
               {Reply{200, "<html></html>"}});
  }

  SignedInRig rig;
};

class SignInAgain : public SignIn {
 protected:
  static constexpr AccountId tokyo{0x0807060504030201ULL};

  static auto again(SignedInRig& at, users::PsUser& user) {
    return at.signed_in.users.sign_in_again(
        user, psn::auth::Npsso{"synthetic-npsso"}, Psn::own_device(),
        Psn::correlation);
  }
};

TEST_F(SignIn, HandsTheSecretOverOnce) {
  rig.answers_a_sign_in_as_tokyo_player();
  const auto signed_in = signs_in(rig);
  ASSERT_TRUE(signed_in);
  EXPECT_EQ(rig.fake.seen.size(), 4U);
  EXPECT_TRUE(rig.fake.unmatched.empty());
  const auto& user = signed_in.value();
  EXPECT_EQ(user->account_id(), AccountId{0x0807060504030201ULL});
  EXPECT_EQ(user->online_id(), "Tokyo Player");
  EXPECT_EQ(user->sign_in_state(), SignInState::signed_in);
  EXPECT_TRUE(user->is_saved());
  EXPECT_EQ(rig.signed_in.users.find(user->account_id()), user);
  const auto kept = rig.signed_in.credentials.saved(user->account_id());
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->npsso.view(), "synthetic-npsso");
  EXPECT_EQ(kept->online_id, "Tokyo Player");
  ASSERT_TRUE(kept->store);
  EXPECT_EQ(kept->store->token.view(), "mobile-access");
  EXPECT_EQ(kept->store->expires_at,
            Psn::at_ms(Psn::wall_start_ms + 7'200'000));
  ASSERT_TRUE(kept->store_refresh);
  EXPECT_EQ(kept->store_refresh->token.view(), "mobile-refresh");
  ASSERT_TRUE(kept->remote_play);
  EXPECT_EQ(kept->remote_play->token.view(), "remote-access");
  EXPECT_EQ(kept->remote_play->expires_at,
            Psn::at_ms(Psn::wall_start_ms + 3'600'000));
  ASSERT_TRUE(kept->remote_play_refresh);
  EXPECT_EQ(kept->remote_play_refresh->token.view(), "remote-refresh");
  ASSERT_TRUE(kept->device);
  EXPECT_EQ(kept->device->view(), Psn::duid);
  EXPECT_EQ(rig.signed_in.random.fills, 0);

  rig.fake.on("GET", std::string{Psn::subscriptions_url},
              {Reply{200, test_support::psn::Fixtures::text(
                              "get_subscriptions.json")}});
  const auto membership = user->get_my_membership();
  ASSERT_TRUE(membership);
  EXPECT_EQ(membership.value().membership.tier(), Tier::premium);
  EXPECT_EQ(rig.fake.seen.back().header("Authorization"),
            "Bearer mobile-access");
}

TEST_F(SignIn, RefusedKeepsNobody) {
  refuses_the_authorization(rig);
  const auto nobody = signs_in(rig);
  ASSERT_FALSE(nobody);
  EXPECT_EQ(nobody.error().kind(), psn::auth::SignInErrorKind::rejected);
  EXPECT_TRUE(rig.signed_in.users.everyone().empty());
  EXPECT_EQ(rig.signed_in.credentials.saves, 0);
}

TEST_F(SignInAgain, AsAnotherAccountChangesNothing) {
  auto& store = rig.signed_in.credentials;
  const auto expired = rig.signed_in.as(rig.saved(9, "NPSSO-OLD", "app-old"));
  const auto state = expired->sign_in_state();
  const auto saves = store.saves;
  const auto removals = store.removals;
  rig.answers_a_sign_in_as_tokyo_player();

  const auto refused = again(rig, *expired);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind(), Refusal::Kind::different_account);
  ASSERT_TRUE(refused.error().signed_in_as());
  EXPECT_EQ(refused.error().signed_in_as()->account, tokyo);
  EXPECT_EQ(refused.error().signed_in_as()->online_id, "Tokyo Player");
  EXPECT_FALSE(refused.error().sign_in_error());
  EXPECT_EQ(rig.fake.seen.size(), 4U);
  EXPECT_EQ(store.saves, saves);
  EXPECT_EQ(store.removals, removals);
  EXPECT_FALSE(store.saved(tokyo));
  EXPECT_FALSE(rig.signed_in.users.find(tokyo));
  const auto kept = store.saved(AccountId{9});
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->npsso.view(), "NPSSO-OLD");
  EXPECT_EQ(kept->online_id, "player");
  ASSERT_TRUE(kept->store);
  EXPECT_EQ(kept->store->token.view(), "app-old");
  EXPECT_EQ(rig.signed_in.users.everyone().size(), 1U);
  EXPECT_EQ(rig.signed_in.users.find(AccountId{9}), expired);
  EXPECT_EQ(expired->sign_in_state(), state);
  EXPECT_NE(expired->sign_in_state(), SignInState::signed_out);
  EXPECT_EQ(expired->online_id(), "player");
  EXPECT_TRUE(expired->is_saved());
}

TEST_F(SignInAgain, ThatFailsChangesNothing) {
  const auto waiting = rig.signed_in.as(rig.saved(9, "NPSSO-OLD", "app-old"));
  const auto before = rig.signed_in.credentials.saves;
  refuses_the_authorization(rig);
  const auto rejected = again(rig, *waiting);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().kind(), Refusal::Kind::sign_in_failed);
  ASSERT_TRUE(rejected.error().sign_in_error());
  EXPECT_EQ(rejected.error().sign_in_error()->kind(),
            psn::auth::SignInErrorKind::rejected);
  EXPECT_FALSE(rejected.error().signed_in_as());
  EXPECT_EQ(rig.signed_in.credentials.saves, before);
  EXPECT_EQ(rig.signed_in.users.find(AccountId{9}), waiting);
  const auto kept = rig.signed_in.credentials.saved(AccountId{9});
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->npsso.view(), "NPSSO-OLD");
}

TEST_F(SignInAgain, RefusesAUserItDoesNotKnow) {
  auto& store = rig.signed_in.credentials;
  const auto expired = rig.signed_in.as(rig.saved(9, "NPSSO-OLD", "app-old"));
  const auto saves = store.saves;
  rig.answers_a_sign_in_as_tokyo_player();
  SignedInRig elsewhere;
  const auto waiting =
      elsewhere.signed_in.as(elsewhere.saved(9, "NPSSO-OLD", "app-old"));

  const auto asked = rig.fake.seen.size();
  const auto stranger = again(rig, *waiting);
  ASSERT_FALSE(stranger);
  EXPECT_EQ(stranger.error().kind(), Refusal::Kind::unknown_user);
  EXPECT_TRUE(rig.signed_in.users.sign_out(AccountId{9}));
  const auto gone = again(rig, *expired);
  ASSERT_FALSE(gone);
  EXPECT_EQ(gone.error().kind(), Refusal::Kind::unknown_user);
  EXPECT_EQ(rig.fake.seen.size(), asked);
  EXPECT_EQ(store.saves, saves);
  EXPECT_TRUE(rig.signed_in.users.everyone().empty());
}

TEST_F(SignInAgain, AsTheSameAccountReplacesTheSignIn) {
  auto& store = rig.signed_in.credentials;
  const auto expired =
      rig.signed_in.as(rig.saved(tokyo.value(), "NPSSO-OLD", "app-old"));
  const auto saves = store.saves;
  rig.answers_a_sign_in_as_tokyo_player();
  const auto replaced = again(rig, *expired);
  ASSERT_TRUE(replaced);
  EXPECT_EQ(rig.fake.seen.size(), 4U);
  EXPECT_TRUE(rig.fake.unmatched.empty());
  const auto& user = replaced.value();
  EXPECT_NE(user, expired);
  EXPECT_EQ(user->account_id(), tokyo);
  EXPECT_EQ(user->online_id(), "Tokyo Player");
  EXPECT_EQ(user->sign_in_state(), SignInState::signed_in);
  EXPECT_TRUE(user->is_saved());
  EXPECT_EQ(expired->sign_in_state(), SignInState::signed_out);
  EXPECT_EQ(rig.signed_in.users.everyone().size(), 1U);
  EXPECT_EQ(rig.signed_in.users.find(tokyo), user);
  EXPECT_EQ(store.saves, saves + 1);
  EXPECT_EQ(store.removals, 0);
  const auto kept = store.saved(tokyo);
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->npsso.view(), "synthetic-npsso");
  EXPECT_EQ(kept->online_id, "Tokyo Player");
  ASSERT_TRUE(kept->store);
  EXPECT_EQ(kept->store->token.view(), "mobile-access");
  ASSERT_TRUE(kept->remote_play);
  EXPECT_EQ(kept->remote_play->token.view(), "remote-access");
  ASSERT_TRUE(kept->device);
  EXPECT_EQ(kept->device->view(), Psn::duid);
}

TEST_F(SignInAgain, GivesAHomeUserAPsnAccount) {
  const auto at_home =
      rig.signed_in.users.add_home_user(tokyo, "Living Room Player");
  ASSERT_TRUE(at_home);
  EXPECT_FALSE(at_home.value()->has_psn_account());
  rig.answers_a_sign_in_as_tokyo_player();
  const auto joined = again(rig, *at_home.value());
  ASSERT_TRUE(joined);
  EXPECT_TRUE(joined.value()->has_psn_account());
  EXPECT_TRUE(joined.value()->is_signed_in());
  EXPECT_TRUE(rig.signed_in.credentials.saved(tokyo));
  EXPECT_FALSE(rig.signed_in.credentials.saved_home_user(tokyo));
}

}  // namespace
}  // namespace users_test
