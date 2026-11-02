#include "src/core/psn/rest/accounts/accounts_client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::auth::BearerToken;
using libchiaking::psn::rest::AccountsClient;
using Reply = RestRig::Reply;

class AccountAttributes : public ::testing::Test {
 protected:
  static constexpr std::string_view url =
      "https://accounts.api.playstation.com/api/v2/accounts/me/attributes";
  static constexpr std::string_view app_agent = "AGENT/1.0";

  static auto bearer() -> BearerToken {
    return BearerToken{std::string_view{"BEARER-FAKE"}, "commerce bearer"};
  }

  RestRig rig;
};

TEST_F(AccountAttributes, AreValidated) {
  rig.fake.on("POST", std::string{url}, {Reply{204, ""}});
  AccountsClient client{rig.http, app_agent};
  const auto accepted = client.validate_attributes(bearer());
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted.value().http_status, 204);
  ASSERT_FALSE(rig.fake.seen.empty());
  const auto& seen = rig.fake.seen.front();
  EXPECT_EQ(seen.method, "POST");
  ASSERT_EQ(seen.headers.size(), 4U);
  EXPECT_EQ(seen.headers[0].name, "Authorization");
  EXPECT_EQ(seen.headers[0].value, "Bearer BEARER-FAKE");
  EXPECT_EQ(seen.headers[1].name, "User-Agent");
  EXPECT_EQ(seen.headers[1].value, app_agent);
  EXPECT_EQ(seen.headers[2].name, "Accept");
  EXPECT_EQ(seen.headers[2].value, "application/json");
  EXPECT_EQ(seen.headers[3].name, "Content-Type");
  EXPECT_EQ(seen.headers[3].value, "application/json");
  EXPECT_EQ(
      seen.body,
      R"({"attributes":["ONLINE_ID","BIRTH_DATE","CITY","REAL_NAME","PRIVACY_SETTING_ACTIVITYSTREAM","PRIVACY_SETTING_FRIENDSLIST","PRIVACY_SETTING_FRIENDREQUESTS","PRIVACY_SETTING_MESSAGES","PRIVACY_SETTING_TRUENAME","PRIVACY_SETTING_SEARCH","PRIVACY_SETTING_RECOMMENDUSERS","PRIVACY_SETTING_BROADCAST"]})");
  EXPECT_EQ(seen.total_timeout, libchiaking::net::http::default_total_timeout);
}

TEST_F(AccountAttributes, ReportThePrivacyUpgrade) {
  rig.fake.on(
      "POST", std::string{url},
      {Reply{
           403,
           R"({"error":{"validationErrors":[{"missingElements":[{"name":"BIRTH_DATE"},{"name":""},{"name":"CITY"}]},)"
           R"({"missingElements":"x"},{"missingElements":[{"name":"REAL_NAME"}]}]}})"},
       Reply{401, "{}"}});
  AccountsClient client{rig.http};
  const auto refused = client.validate_attributes(bearer());
  ASSERT_FALSE(refused);
  EXPECT_FALSE(refused.error().failure());
  EXPECT_EQ(refused.error().http_status(), 403);
  EXPECT_TRUE(refused.error().privacy_upgrade_required());
  EXPECT_TRUE(refused.error().auth_refused());
  EXPECT_EQ(refused.error().missing_elements(),
            (std::vector<std::string>{"BIRTH_DATE", "CITY", "REAL_NAME"}));
  const auto denied = client.validate_attributes(bearer());
  ASSERT_FALSE(denied);
  EXPECT_TRUE(denied.error().auth_refused());
  EXPECT_FALSE(denied.error().privacy_upgrade_required());
}

TEST_F(AccountAttributes, ReportATransportFailure) {
  rig.fake.on("POST", std::string{url}, {Reply::transport()});
  AccountsClient client{rig.http};
  const auto failed = client.validate_attributes(bearer());
  ASSERT_FALSE(failed);
  EXPECT_TRUE(failed.error().failure());
  EXPECT_FALSE(failed.error().auth_refused());
  ASSERT_FALSE(rig.fake.seen.empty());
  EXPECT_EQ(rig.fake.seen.front().headers.size(), 3U);
}

}  // namespace
}  // namespace psn_test
