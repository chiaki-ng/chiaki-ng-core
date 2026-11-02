#include "src/core/psn/session_internals.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/ps/store_app_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>

namespace catalog_test {
namespace {

using libchiaking::users::SignInState;
using UserReason = libchiaking::users::Unanswered::Reason;

class StoreAppTitleTokens : public ::testing::Test, public StoreAppRig {
 protected:
  void sells_the_remaster() {
    replies(by_product, fixture("product_FAKE0056TITLE000.json"));
  }

  auto asks_owned_remaster(std::size_t then_requests)
      -> ::testing::AssertionResult {
    const auto answer = asked(remaster);
    if (!answer)
      return ::testing::AssertionFailure() << "the store did not answer";
    if (!answer.value().owned_per_store())
      return ::testing::AssertionFailure() << "the remaster is not owned";
    if (requests() != then_requests)
      return ::testing::AssertionFailure()
             << requests() << " requests made, expected " << then_requests;
    return ::testing::AssertionSuccess();
  }
};

TEST_F(StoreAppTitleTokens, NeverRenewAUsableToken) {
  holds_a_refresh_token();
  ps_renews("renewed");
  sells_the_remaster();
  for (std::size_t check = 1; check <= 4; ++check)
    EXPECT_TRUE(asks_owned_remaster(check));
  later(std::chrono::minutes{59});
  EXPECT_TRUE(asked(remaster));
  EXPECT_EQ(requests(), 5U);
  EXPECT_EQ(renewals(), 0);
  EXPECT_EQ(rig.http.count(by_product), 5);
  EXPECT_EQ(last_bearer(), "Bearer app");
}

TEST_F(StoreAppTitleTokens, RenewAnExpiredTokenOnceThenReuseIt) {
  holds_a_refresh_token();
  ps_renews("renewed");
  sells_the_remaster();
  later(std::chrono::hours{1});
  EXPECT_EQ(user->sign_in_state(), SignInState::store_access_lapsed);
  EXPECT_TRUE(asks_owned_remaster(2));
  const auto& seen = rig.http.seen;
  ASSERT_EQ(seen.size(), 2U);
  EXPECT_EQ(seen[0].method, "POST");
  EXPECT_NE(seen[0].url.find(token_exchange), std::string::npos);
  EXPECT_NE(seen[1].url.find(by_product), std::string::npos);
  EXPECT_EQ(last_bearer(), "Bearer renewed");
  EXPECT_EQ(user->sign_in_state(), SignInState::signed_in);
  for (std::size_t check = 1; check <= 3; ++check) {
    EXPECT_TRUE(asked(remaster));
    EXPECT_EQ(requests(), 2 + check);
  }
  EXPECT_EQ(renewals(), 1);
  EXPECT_EQ(last_bearer(), "Bearer renewed");
}

TEST_F(StoreAppTitleTokens, RenewATokenWithinItsExpiryMargin) {
  holds_a_refresh_token();
  ps_renews("renewed");
  sells_the_remaster();
  later(std::chrono::hours{1} - std::chrono::seconds{10});
  EXPECT_TRUE(asked(remaster));
  EXPECT_EQ(requests(), 2U);
  EXPECT_EQ(renewals(), 1);
}

TEST_F(StoreAppTitleTokens, RenewATokenWithoutARefreshTokenFromTheSeed) {
  ps_issues_a_token_for_the_seed("seeded");
  sells_the_remaster();
  later(std::chrono::hours{1});
  EXPECT_TRUE(asked(remaster));
  EXPECT_EQ(requests(), 2U);
  EXPECT_EQ(last_bearer(), "Bearer seeded");
  EXPECT_TRUE(asked(remaster));
  EXPECT_EQ(requests(), 3U);
}

TEST_F(StoreAppTitleTokens, RenewARefusedTokenOnceAndAskAgain) {
  holds_a_refresh_token();
  ps_renews("renewed");
  rig.http.on(
      "GET", std::string{by_product},
      {Reply{401, "{}"}, Reply{200, fixture("product_FAKE0056TITLE000.json")}});
  EXPECT_TRUE(asks_owned_remaster(3));
  EXPECT_EQ(renewals(), 1);
  EXPECT_EQ(rig.http.count(by_product), 2);
  EXPECT_EQ(last_bearer(), "Bearer renewed");
  EXPECT_TRUE(asked(remaster));
  EXPECT_EQ(requests(), 4U);
  EXPECT_EQ(renewals(), 1);
}

TEST_F(StoreAppTitleTokens, RenewATokenAlreadyMarkedRefusedBeforeAsking) {
  holds_a_refresh_token();
  ps_renews("renewed");
  sells_the_remaster();
  {
    auto& slot =
        libchiaking::psn::SessionInternals{
            libchiaking::users::PsUser::Core::of(*user).session()}
            .app_token();
    const auto leased = slot.lease();
    ASSERT_TRUE(leased);
    slot.record_use(leased.value(), 401, 0);
  }
  EXPECT_EQ(user->sign_in_state(), SignInState::store_access_lapsed);
  EXPECT_TRUE(asked(remaster));
  EXPECT_EQ(requests(), 2U);
  EXPECT_EQ(renewals(), 1);
  EXPECT_EQ(last_bearer(), "Bearer renewed");
  EXPECT_TRUE(asked(remaster));
  EXPECT_EQ(requests(), 3U);
  EXPECT_EQ(renewals(), 1);
}

TEST_F(StoreAppTitleTokens, ExpireTheSignInWhenRenewalIsRefused) {
  holds_a_refresh_token();
  rig.http.on("POST", std::string{token_exchange},
              {Reply{400, R"({"error":"invalid_grant"})"}});
  sells_the_remaster();
  later(std::chrono::hours{1});
  const auto answer = asked(remaster);
  ASSERT_FALSE(answer);
  EXPECT_TRUE(answer.error().needs_sign_in());
  ASSERT_TRUE(answer.error().cause());
  EXPECT_EQ(answer.error().cause()->reason, UserReason::sign_in_expired);
  EXPECT_TRUE(user->is_expired());
  EXPECT_EQ(rig.http.count(by_product), 0);
  EXPECT_EQ(requests(), 2U);
  const auto again = asked(remaster);
  ASSERT_FALSE(again);
  EXPECT_TRUE(again.error().needs_sign_in());
  ASSERT_TRUE(again.error().cause());
  EXPECT_EQ(again.error().cause()->reason, UserReason::sign_in_expired);
  EXPECT_EQ(requests(), 2U);
}

TEST_F(StoreAppTitleTokens, KeepTheSignInWhenRenewalIsUnanswered) {
  holds_a_refresh_token();
  sells_the_remaster();
  later(std::chrono::hours{1});
  const auto answer = asked(remaster);
  ASSERT_FALSE(answer);
  EXPECT_FALSE(answer.error().needs_sign_in());
  ASSERT_TRUE(answer.error().cause());
  EXPECT_EQ(answer.error().cause()->reason, UserReason::unavailable);
  EXPECT_FALSE(user->is_expired());
  EXPECT_EQ(rig.http.count(by_product), 0);
  EXPECT_EQ(requests(), 1U);
}

TEST(StoreAppTitleSignIn, AnswersASignedOutUserWithoutARequest) {
  PsRig rig;
  const auto home = rig.home_user();
  libchiaking::catalog::ps::StoreAppTitle title{*home, rig.client, rig.wall};
  const auto answer = title.answer_for(PsRig::product(StoreAppRig::remaster));
  ASSERT_FALSE(answer);
  EXPECT_TRUE(answer.error().needs_sign_in());
  ASSERT_TRUE(answer.error().cause());
  EXPECT_EQ(answer.error().cause()->reason, UserReason::signed_out);
  const auto grouped = title.answer_for(PsRig::product(StoreAppRig::remaster),
                                        StoreAppRig::skyline_concept());
  ASSERT_FALSE(grouped);
  ASSERT_TRUE(grouped.error().cause());
  EXPECT_EQ(grouped.error().cause()->reason, UserReason::signed_out);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(StoreAppTitleTokens, ObtainStoreAccessOnceFromTheSeed) {
  user = rig.signed_in.as("NPSSO", 12);
  EXPECT_EQ(user->sign_in_state(), SignInState::store_access_lapsed);
  ps_issues_a_token_for_the_seed("seeded");
  sells_the_remaster();
  EXPECT_TRUE(asks_owned_remaster(2));
  EXPECT_EQ(renewals(), 1);
  EXPECT_EQ(last_bearer(), "Bearer seeded");
  EXPECT_TRUE(asked(remaster));
  EXPECT_EQ(requests(), 3U);
  EXPECT_EQ(renewals(), 1);
}

TEST_F(StoreAppTitleTokens, StillAskTheStoreAfterTheSignInExpired) {
  rig.http.on("GET", "response_type=token",
              {Reply{302, "",
                     PsRig::location(
                         "https://x/grc-response.html?error=login_required")}});
  EXPECT_FALSE(
      libchiaking::users::CatalogCalls{*user}.library_records(rig.client));
  EXPECT_TRUE(user->is_expired());
  sells_the_remaster();
  const auto before = requests();
  EXPECT_TRUE(asks_owned_remaster(before + 1));
  EXPECT_TRUE(user->is_expired());
}

}  // namespace
}  // namespace catalog_test
