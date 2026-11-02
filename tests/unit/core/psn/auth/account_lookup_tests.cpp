#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/auth/sign_in.hpp"
#include "tests/unit/core/psn/auth/sign_in_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

namespace psn_test {
namespace {

using libchiaking::psn::auth::BearerToken;
using libchiaking::psn::auth::SignInErrorKind;
using Reply = ClientRig::Reply;

class AccountLookup : public ::testing::Test {
 protected:
  auto lookup(Reply reply)
      -> libchiaking::Expected<libchiaking::psn::auth::Account,
                               SignInRig::SignInError> {
    rig.reset();
    rig.http.on("GET", std::string{SignInRig::account_url}, {std::move(reply)});
    libchiaking::psn::auth::SignIn sign_in{rig.http, ClientRig::steady_clock()};
    auto result = sign_in.account(BearerToken{"remote-access"});
    EXPECT_EQ(rig.http.seen.size(), 1U);
    return result;
  }

  static auto named(std::string_view online_id) -> Reply {
    return SignInRig::json(R"({"user_id":"12","online_id":")" +
                           std::string{online_id} + R"("})");
  }

  SignInRig rig;
};

TEST_F(AccountLookup, ReadsTheLargestAccountId) {
  const auto found = lookup(SignInRig::json(
      R"({"user_id":"18446744073709551615","online_id":"Edge"})"));
  ASSERT_TRUE(found);
  EXPECT_EQ(found.value().id.value(), 18446744073709551615ULL);
  EXPECT_EQ(found.value().online_id, "Edge");
}

TEST_F(AccountLookup, RefusesAMalformedAccount) {
  for (const std::string_view body :
       {R"({"user_id":"18446744073709551616","online_id":"Edge"})",
        R"({"user_id":"123456789012345678901","online_id":"Edge"})",
        R"({"user_id":"12a","online_id":"Edge"})",
        R"({"user_id":"","online_id":"Edge"})",
        R"({"user_id":12,"online_id":"Edge"})",
        R"({"user_id":"12","online_id":""})", R"({"user_id":"12"})", R"([])"})
    EXPECT_EQ(SignInRig::kind_of(lookup(SignInRig::json(body))),
              SignInErrorKind::malformed)
        << body;
}

TEST_F(AccountLookup, LimitsTheOnlineIdTo256Characters) {
  EXPECT_EQ(SignInRig::kind_of(lookup(named(std::string(257, 'a')))),
            SignInErrorKind::malformed);
  EXPECT_TRUE(lookup(named(std::string(256, 'a'))));
}

TEST_F(AccountLookup, StopsTextAtANulAndIgnoresExtraFields) {
  const auto cut = lookup(SignInRig::json(
      R"({"user_id":"12\u0000junk","online_id":"Edge\u0000junk","avatar":{"size":[1,2]},"plus":7})"));
  ASSERT_TRUE(cut);
  EXPECT_EQ(cut.value().id.value(), 12U);
  EXPECT_EQ(cut.value().online_id, "Edge");
}

TEST_F(AccountLookup, MapsARefusalAndARateLimit) {
  EXPECT_EQ(SignInRig::kind_of(lookup(Reply{401, ""})),
            SignInErrorKind::rejected);
  EXPECT_EQ(SignInRig::kind_of(lookup(Reply{429, ""})),
            SignInErrorKind::rate_limited);
}

TEST_F(AccountLookup, EscapesTheTokenInThePath) {
  rig.http.on("GET",
              "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token/"
              "a%2Fb%3Dc~d",
              {SignInRig::json(SignInRig::account_json)});
  EXPECT_TRUE(rig.sign_in.account(BearerToken{"a/b=c~d"}));
}

}  // namespace
}  // namespace psn_test
