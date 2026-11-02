#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/session_internals.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "src/core/users/ports/console_calls.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace users_test {
namespace {

namespace psn = libchiaking::psn;
using libchiaking::AccountId;
using libchiaking::users::SignInState;
using libchiaking::users::Unanswered;
using Reply = Psn::Reply;

class PsUserSignInState : public ::testing::Test {
 protected:
  static constexpr std::string_view renewed_mobile =
      R"({"access_token":"new-mobile","refresh_token":"next-mobile","expires_in":2000})";

  auto with_app_refresh() -> std::shared_ptr<libchiaking::users::PsUser> {
    auto saved = rig.saved(9, "NPSSO-A", "old-access");
    saved.store_refresh.emplace(psn::auth::AppRefreshToken{"old-mobile"});
    return rig.signed_in.as(std::move(saved));
  }

  SignedInRig rig;
};

TEST_F(PsUserSignInState, FollowsTheCredentials) {
  auto saved = rig.saved(9, "NPSSO-A", "old-access");
  saved.store_refresh.emplace(psn::auth::AppRefreshToken{"old-mobile"});
  saved.remote_play.emplace(psn::auth::BearerToken{"old-remote-access"},
                            Psn::at_ms(Psn::wall_start_ms + 3'600'000));
  saved.remote_play_refresh.emplace(
      psn::auth::RemotePlayRefreshToken{"old-remote"});
  const auto user = rig.signed_in.as(std::move(saved));
  EXPECT_EQ(user->sign_in_state(), SignInState::signed_in);
  EXPECT_TRUE(user->is_signed_in());
  EXPECT_FALSE(user->is_expired());
  EXPECT_TRUE(user->is_saved());
  const auto untouched = user->stay_signed_in();
  ASSERT_TRUE(untouched);
  EXPECT_EQ(untouched.value(), SignInState::signed_in);
  EXPECT_TRUE(rig.fake.seen.empty());

  rig.wall.advance_ms(3'600'000);
  EXPECT_EQ(user->sign_in_state(), SignInState::store_access_lapsed);
  EXPECT_TRUE(user->is_signed_in());
  const auto lapsed = user->get_my_membership();
  ASSERT_FALSE(lapsed);
  EXPECT_EQ(lapsed.error().reason, Unanswered::Reason::store_access_lapsed);
  ASSERT_EQ(rig.fake.seen.size(), 1U);
  EXPECT_TRUE(Psn::exchanges_npsso(rig.fake.seen.front()));
  EXPECT_EQ(user->sign_in_state(), SignInState::store_access_lapsed);
  rig.fake.seen.clear();

  rig.fake.on("POST", std::string{Psn::token_url},
              {Reply{200, std::string{renewed_mobile}}});
  rig.fake.on(
      "POST", std::string{Psn::legacy_token_url},
      {Reply{
          200,
          R"({"access_token":"new-remote","refresh_token":"next-remote","expires_in":1000,"token_type":"Bearer"})"}});
  const auto saves = rig.signed_in.credentials.saves;
  const auto renewed = user->stay_signed_in();
  ASSERT_TRUE(renewed);
  EXPECT_EQ(renewed.value(), SignInState::signed_in);
  ASSERT_EQ(rig.fake.seen.size(), 2U);
  EXPECT_TRUE(Psn::exchanges_npsso(rig.fake.seen[0]));
  EXPECT_EQ(rig.fake.seen[1].url, Psn::legacy_token_url);
  EXPECT_EQ(rig.fake.seen[1].body, Psn::remote_refresh_body);
  EXPECT_EQ(rig.signed_in.credentials.saves, saves + 1);
  EXPECT_TRUE(user->is_saved());
  const auto kept = rig.signed_in.credentials.saved(AccountId{9});
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->npsso.view(), "NPSSO-A");
  ASSERT_TRUE(kept->store);
  EXPECT_EQ(kept->store->token.view(), "new-mobile");
  EXPECT_EQ(kept->store->expires_at,
            Psn::at_ms(Psn::wall_start_ms + 3'600'000 + 2'000'000));
  ASSERT_TRUE(kept->store_refresh);
  EXPECT_EQ(kept->store_refresh->token.view(), "next-mobile");
  ASSERT_TRUE(kept->remote_play);
  EXPECT_EQ(kept->remote_play->token.view(), "new-remote");
  ASSERT_TRUE(kept->remote_play_refresh);
  EXPECT_EQ(kept->remote_play_refresh->token.view(), "next-remote");

  rig.fake.on("GET", std::string{Psn::subscriptions_url},
              {Reply{401, R"({"error":"invalid_token"})"}});
  const auto refused = user->get_my_membership();
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().reason, Unanswered::Reason::store_access_lapsed);
  EXPECT_EQ(refused.error().http_status, 401);
  EXPECT_EQ(rig.fake.seen.back().header("Authorization"), "Bearer new-mobile");
  EXPECT_EQ(user->sign_in_state(), SignInState::store_access_lapsed);
  EXPECT_FALSE(user->is_expired());

  rig.fake.rules.clear();
  rig.fake.on("POST", std::string{Psn::token_url},
              {Reply{400, R"({"error":"invalid_grant"})"}});
  const auto requests = rig.fake.seen.size();
  const auto expired = user->stay_signed_in();
  ASSERT_FALSE(expired);
  EXPECT_EQ(expired.error().reason, Unanswered::Reason::sign_in_expired);
  ASSERT_EQ(rig.fake.seen.size(), requests + 2);
  EXPECT_TRUE(Psn::exchanges_npsso(rig.fake.seen[requests]));
  EXPECT_EQ(rig.fake.seen.back().method, "POST");
  EXPECT_EQ(rig.fake.seen.back().url, Psn::token_url);
  EXPECT_NE(rig.fake.seen.back().body.find("grant_type=refresh_token"),
            std::string::npos);
  EXPECT_EQ(user->sign_in_state(), SignInState::expired);
  EXPECT_TRUE(user->is_expired());
  EXPECT_FALSE(user->is_signed_in());

  const auto removals = rig.signed_in.credentials.removals;
  EXPECT_TRUE(rig.signed_in.users.sign_out(AccountId{9}));
  EXPECT_EQ(rig.signed_in.credentials.removals, removals + 1);
  EXPECT_FALSE(rig.signed_in.credentials.saved(AccountId{9}));
  EXPECT_EQ(user->sign_in_state(), SignInState::signed_out);
  EXPECT_FALSE(user->is_expired());
  EXPECT_FALSE(user->is_signed_in());
  EXPECT_FALSE(rig.signed_in.users.find(AccountId{9}));
  const auto after = rig.fake.seen.size();
  const auto stayed = user->stay_signed_in();
  ASSERT_FALSE(stayed);
  EXPECT_EQ(stayed.error().reason, Unanswered::Reason::signed_out);
  const auto profile = user->get_my_profile();
  ASSERT_FALSE(profile);
  EXPECT_EQ(profile.error().reason, Unanswered::Reason::signed_out);
  libchiaking::net::http::Client http{rig.fake, Runtime::steady(), {}};
  const auto consoles =
      libchiaking::users::ConsoleCalls{*user}.account_consoles(http, 0);
  ASSERT_FALSE(consoles);
  EXPECT_EQ(consoles.error().reason, Unanswered::Reason::signed_out);
  EXPECT_EQ(rig.fake.seen.size(), after);
  EXPECT_EQ(user->snapshot_json().value().find("new-mobile"),
            std::string::npos);
}

TEST_F(PsUserSignInState, ExpiresWhenTheNpssoExchangeIsRefused) {
  const auto user = rig.signed_in.as(rig.saved(4, "NPSSO-A", "app-a"));
  const psn::auth::Grant grant{"client", "scope"};
  const auto exchanged =
      psn::SessionInternals{
          libchiaking::users::PsUser::Core::of(*user).session()}
          .tokens()
          .acquire(grant, "label", {},
                   [](const psn::auth::Npsso&)
                       -> libchiaking::Expected<psn::auth::Exchanged,
                                                psn::auth::AuthError> {
                     return libchiaking::unexpected(psn::auth::AuthError{
                         psn::auth::AuthErrorKind::refused, 401});
                   });
  EXPECT_FALSE(exchanged);
  EXPECT_EQ(user->sign_in_state(), SignInState::expired);
  EXPECT_TRUE(user->is_expired());
}

TEST_F(PsUserSignInState, ProfileQuestionRenewsOnlyATokenThatCannotBeUsed) {
  const auto user = with_app_refresh();
  rig.fake.on("POST", std::string{Psn::token_url},
              {Reply{200, std::string{renewed_mobile}}});
  static_cast<void>(user->get_my_profile());
  static_cast<void>(user->get_my_profile());
  EXPECT_EQ(rig.fake.seen.size(), 2U);
  EXPECT_EQ(rig.fake.count(Psn::token_url), 0);
  EXPECT_EQ(rig.fake.seen.back().header("Authorization"), "Bearer old-access");

  rig.wall.advance_ms(3'600'000);
  static_cast<void>(user->get_my_profile());
  ASSERT_EQ(rig.fake.seen.size(), 4U);
  EXPECT_TRUE(Psn::exchanges_npsso(rig.fake.seen[2]));
  EXPECT_EQ(rig.fake.seen.back().header("Authorization"), "Bearer new-mobile");
  static_cast<void>(user->get_my_profile());
  EXPECT_EQ(rig.fake.seen.size(), 5U);
  EXPECT_EQ(rig.fake.count(Psn::token_url), 1);
}

TEST_F(PsUserSignInState, OldAppRefreshTokenStandsInForARefusedNpsso) {
  const auto kept_on = with_app_refresh();
  rig.fake.on("POST", std::string{Psn::token_url},
              {Reply{400, R"({"error":"invalid_grant"})"},
               Reply{200, std::string{renewed_mobile}}});
  rig.wall.advance_ms(3'600'000);
  const auto renewed = kept_on->stay_signed_in();
  EXPECT_TRUE(renewed);
  ASSERT_EQ(rig.fake.seen.size(), 2U);
  EXPECT_TRUE(Psn::exchanges_npsso(rig.fake.seen[0]));
  EXPECT_EQ(rig.fake.seen[1].body, Psn::app_refresh_body);
  EXPECT_EQ(kept_on->sign_in_state(), SignInState::signed_in);
}

TEST_F(PsUserSignInState, OldAppRefreshTokenIsDroppedWhenTheNpssoIsExchanged) {
  const auto moved = with_app_refresh();
  rig.fake.on(
      "POST", std::string{Psn::token_url},
      {Reply{200, R"({"access_token":"android-app","expires_in":2000})"}});
  rig.wall.advance_ms(3'600'000);
  EXPECT_TRUE(moved->stay_signed_in());
  ASSERT_EQ(rig.fake.seen.size(), 1U);
  EXPECT_TRUE(Psn::exchanges_npsso(rig.fake.seen[0]));
  const auto kept = rig.signed_in.credentials.saved(AccountId{9});
  ASSERT_TRUE(kept);
  ASSERT_TRUE(kept->store);
  EXPECT_EQ(kept->store->token.view(), "android-app");
  EXPECT_FALSE(kept->store_refresh);
}

}  // namespace
}  // namespace users_test
