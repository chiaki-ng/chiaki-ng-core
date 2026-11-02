#include "src/core/psn/auth/credentials.hpp"
#include "tests/unit/api/accounts/rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace accounts_test {
namespace {

using libchiaking::AccountId;
using libchiaking::psn::auth::AppToken;

static_assert(account::Client::token_limit == 8192);

class AccountProfile : public AccountClientTest {};

TEST_F(AccountProfile, IsParsedIntoTypedDetails) {
  ScriptedHttp http{
      200,
      R"({"onlineId":"synthetic_player","isPlus":true,"avatars":[)"
      R"({"size":"s","url":"https://static.example/s.png"},{"size":"m","url":"http://static.example/m.png"}]})"};
  account::Client client{http, clock};
  const auto profile =
      client.profile(AppToken{app_token}, AccountId{1234567890123456789ULL});
  ASSERT_TRUE(profile);
  EXPECT_EQ(profile.value().online_id, "synthetic_player");
  EXPECT_EQ(profile.value().avatar_url, "https://static.example/m.png");
  EXPECT_EQ(profile.value().plus, account::PlusStatus::active);
  ASSERT_EQ(http.urls.size(), 1U);
  EXPECT_TRUE(
      http.urls.front().ends_with("/users/1234567890123456789/profiles"))
      << http.urls.front();
  EXPECT_TRUE(
      http.sent_header("Authorization: Bearer " + std::string{app_token}));
}

TEST_F(AccountProfile, LeavesAnUnsaidAvatarAndPlusUnknown) {
  ScriptedHttp bare{200, R"({"onlineId":"plain"})"};
  const auto plain =
      account::Client{bare, clock}.profile(AppToken{app_token}, AccountId{7});
  ASSERT_TRUE(plain);
  EXPECT_FALSE(plain.value().avatar_url);
  EXPECT_EQ(plain.value().plus, account::PlusStatus::unknown);
}

TEST_F(AccountProfile, ReadsALapsedPlusAsAbsent) {
  ScriptedHttp lapsed{200, R"({"onlineId":"plain","isPlus":false})"};
  const auto profile =
      account::Client{lapsed, clock}.profile(AppToken{app_token}, AccountId{7});
  ASSERT_TRUE(profile);
  EXPECT_EQ(profile.value().plus, account::PlusStatus::absent);
}

}  // namespace
}  // namespace accounts_test
