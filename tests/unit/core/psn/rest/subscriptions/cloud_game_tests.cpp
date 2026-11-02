#include "src/core/psn/rest/subscriptions/subscriptions_client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::auth::BearerToken;
using libchiaking::psn::rest::Cause;
using libchiaking::psn::rest::CloudGameFeature;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;
using libchiaking::psn::rest::SubscriptionsClient;
using Reply = RestRig::Reply;

class CloudGameBenefits : public ::testing::Test {
 protected:
  static constexpr std::string_view url =
      "https://m.np.playstation.com/api/subscriptions/v2/users/me/premium/"
      "features/"
      "cloudgame";

  static auto client(RestRig& rig) -> SubscriptionsClient {
    return SubscriptionsClient{
        rig.http, BearerToken{std::string_view{"APP-FAKE"}, "app bearer"}};
  }

  static auto benefits(RestRig& rig, std::string body, int status = 200)
      -> libchiaking::Expected<
          CloudGameFeature,
          libchiaking::psn::rest::SubscriptionsClient::CloudGameError> {
    rig.fake.on("GET", std::string{url}, {Reply{status, std::move(body)}});
    return client(rig).cloud_game();
  }

  auto benefits(std::string body, int status = 200) -> libchiaking::Expected<
      CloudGameFeature,
      libchiaking::psn::rest::SubscriptionsClient::CloudGameError> {
    return benefits(rig, std::move(body), status);
  }

  RestRig rig;
};

TEST_F(CloudGameBenefits, AreAskedWithTheAppBearerAndNoOptions) {
  const auto read =
      benefits(R"({"authorization":true,"cloudgameOwnerInfos":[]})");
  EXPECT_TRUE(read);
  EXPECT_TRUE(rig.fake.unmatched.empty());
  ASSERT_EQ(rig.fake.seen.size(), 1U);
  const auto& seen = rig.fake.seen.front();
  EXPECT_EQ(seen.method, "GET");
  EXPECT_EQ(seen.url, url);
  ASSERT_EQ(seen.headers.size(), 2U);
  EXPECT_EQ(seen.headers[0].name, "Authorization");
  EXPECT_EQ(seen.headers[0].value, "Bearer APP-FAKE");
  EXPECT_EQ(seen.headers[1].name, "Accept");
  EXPECT_EQ(seen.headers[1].value, "application/json");
}

TEST_F(CloudGameBenefits, KeepEveryOwnerOfAGrantedReply) {
  const auto read = benefits(
      R"({"authorization":true,"justification":"SUBSCRIBED","cloudgameOwnerInfos":[)"
      R"({"ownerAccountId":"1111","tierLabel":"TIER_30","benefitPlanIds":["PLAN-A","PLAN-B"],)"
      R"("hasEstBenefit":true,"hasSgodBenefit":false},)"
      R"({"ownerAccountId":2222,"tierLabel":"TIER_30","benefitPlanIds":["PLAN-C"],"hasSgodBenefit":true}]})");
  ASSERT_TRUE(read);
  const auto& feature = read.value();
  EXPECT_EQ(feature.authorization, true);
  EXPECT_EQ(feature.justification, "SUBSCRIBED");
  ASSERT_EQ(feature.owners.size(), 2U);
  const auto& first = feature.owners[0];
  EXPECT_EQ(first.owner_account_id, "1111");
  EXPECT_EQ(first.tier_label, "TIER_30");
  EXPECT_EQ(first.benefit_plan_ids,
            (std::vector<std::string>{"PLAN-A", "PLAN-B"}));
  EXPECT_TRUE(first.has_est_benefit);
  EXPECT_FALSE(first.has_sgod_benefit);
  const auto& second = feature.owners[1];
  EXPECT_EQ(second.owner_account_id, "2222");
  EXPECT_EQ(second.benefit_plan_ids, (std::vector<std::string>{"PLAN-C"}));
  EXPECT_FALSE(second.has_est_benefit);
  EXPECT_TRUE(second.has_sgod_benefit);
}

TEST_F(CloudGameBenefits, KeepAnOwnerWithEmptyPlans) {
  const auto plans = benefits(
      R"({"authorization":true,"cloudgameOwnerInfos":[{"ownerAccountId":"1","benefitPlanIds":[]}]})");
  ASSERT_TRUE(plans);
  EXPECT_EQ(plans.value().authorization, true);
  ASSERT_EQ(plans.value().owners.size(), 1U);
  EXPECT_TRUE(plans.value().owners[0].benefit_plan_ids.empty());
}

TEST_F(CloudGameBenefits, ReadADenialWithItsJustification) {
  const auto refused =
      benefits(R"({"authorization":false,"justification":{"code":7}})");
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused.value().authorization, false);
  EXPECT_TRUE(refused.value().owners.empty());
  EXPECT_EQ(refused.value().justification, R"({"code":7})");
}

TEST_F(CloudGameBenefits, LeaveAnUnstatedAuthorizationUnset) {
  const auto unstated = benefits(
      R"({"cloudgameOwnerInfos":[{"ownerAccountId":"1","benefitPlanIds":["P"]}]})");
  ASSERT_TRUE(unstated);
  EXPECT_FALSE(unstated.value().authorization);
  EXPECT_FALSE(unstated.value().justification);
  EXPECT_EQ(unstated.value().owners.size(), 1U);
}

TEST_F(CloudGameBenefits, ReadNullsAsNothing) {
  const auto nothing = benefits(
      R"({"authorization":null,"cloudgameOwnerInfos":null,"justification":null})");
  ASSERT_TRUE(nothing);
  EXPECT_FALSE(nothing.value().authorization);
  EXPECT_TRUE(nothing.value().owners.empty());
  EXPECT_FALSE(nothing.value().justification);
}

TEST_F(CloudGameBenefits, GiveAnOwnerWithoutPlansNone) {
  const auto planless = benefits(
      R"({"authorization":true,"cloudgameOwnerInfos":[{"ownerAccountId":"1"}]})");
  ASSERT_TRUE(planless);
  ASSERT_FALSE(planless.value().owners.empty());
  EXPECT_TRUE(planless.value().owners.at(0).benefit_plan_ids.empty());
}

TEST_F(CloudGameBenefits, AreResponseErrorsWhenMalformed) {
  for (
      const std::string_view body :
      {"not json", "[]", "true", R"({"authorization":"true"})",
       R"({"authorization":1})",
       R"({"authorization":true,"cloudgameOwnerInfos":{}})",
       R"({"authorization":true,"cloudgameOwnerInfos":["owner"]})",
       R"({"authorization":true,"cloudgameOwnerInfos":[{"benefitPlanIds":"PLAN"}]})",
       R"({"authorization":true,"cloudgameOwnerInfos":[{"benefitPlanIds":["PLAN",7]}]})"}) {
    RestRig odd;
    const auto read = benefits(odd, std::string{body});
    ASSERT_FALSE(read) << body;
    EXPECT_EQ(read.error().cause(), Cause::response) << body;
  }
}

TEST_F(CloudGameBenefits, KeepTheBearerOutOfARefusal) {
  const auto denied = benefits(R"({"error":"APP-FAKE is bad"})", 401);
  ASSERT_FALSE(denied);
  EXPECT_TRUE(denied.error().auth_refused());
  EXPECT_EQ(std::string_view{denied.error().excerpt()}.find("APP-FAKE"),
            std::string_view::npos);
}

TEST_F(CloudGameBenefits, ReportAServerError) {
  const auto failed = benefits("", 503);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().http_status(), 503);
}

TEST_F(CloudGameBenefits, ReportAnUnreachableServer) {
  rig.fake.on("GET", std::string{url}, {Reply::transport()});
  const auto lost = client(rig).cloud_game();
  ASSERT_FALSE(lost);
  EXPECT_TRUE(lost.error().failure());
}

}  // namespace
}  // namespace psn_test
