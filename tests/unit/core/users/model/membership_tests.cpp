#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/graphql/check.hpp"
#include "src/core/users/ports/membership_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/json/tree.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/psn/fixtures.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace users_test {
namespace {

namespace graphql = libchiaking::psn::graphql;
namespace users = libchiaking::users;
using test_support::JsonTree;
using test_support::psn::Fixtures;
using test_support::psn::Reply;
using users::Membership;
using users::Tier;

class MembershipFromSubscriptions : public ::testing::Test {
 protected:
  static auto entry(std::string_view label, std::string_view sku,
                    std::string_view active, std::string_view type = "PS_PLUS")
      -> std::string {
    return R"({"__typename":"ServiceSubscription","active":)" +
           std::string{active} +
           R"(,"endDate":"2027-01-01T00:00:00.000Z","id":")" +
           std::string{sku} +
           R"(","productName":"Plan","subscriptionId":"SUB001-SUBC00001_00-FIXTURE000000000","subscriptionType":")" +
           std::string{type} + R"(","tierLabel":")" + std::string{label} +
           R"("})";
  }

  static auto enveloped(std::string_view inner) -> std::string {
    return R"({"data":)" + std::string{inner} + "}";
  }

  static auto fetch(std::string body) -> users::MembershipCheck {
    test_support::FakeClock clock;
    test_support::FakeClock wall;
    test_support::psn::FakeHttp fake;
    auto state = std::make_shared<test_support::StopState>();
    libchiaking::platform::StopToken stop{state};
    libchiaking::net::http::Client client{fake, Runtime::steady(), stop};
    test_support::users::SignedIn signed_in{fake, clock, wall};
    const auto user =
        signed_in.as("NPSSO-A", "app", std::chrono::system_clock::time_point{});
    fake.on("GET", std::string{Psn::subscriptions_url},
            {Reply{200, std::move(body)}});
    auto result = users::MembershipCalls{*user}.membership(client);
    EXPECT_EQ(fake.seen.size(), 1U);
    if (result) {
      EXPECT_EQ(result.value().account, user->account_id());
    }
    return result;
  }

  static auto tier_of(std::string body) -> std::optional<Tier> {
    const auto result = fetch(std::move(body));
    return result ? result.value().membership.tier() : std::nullopt;
  }

  static auto parse(const std::string& entries) -> Membership {
    auto result = fetch(enveloped(
        R"({"fetchSubscriptions":{"__typename":"FetchServiceSubscriptionsResponse","subscriptions":[)" +
        entries + "]}}"));
    return result ? result.value().membership : Membership{};
  }
};

class MembershipJson : public MembershipFromSubscriptions {};

TEST_F(MembershipFromSubscriptions, ReadsCapturedSubscriptionsAsPremium) {
  const auto fetched = fetch(Fixtures::text("get_subscriptions.json"));
  ASSERT_TRUE(fetched);
  const auto& result = fetched.value().membership;
  EXPECT_EQ(result.tier(), Tier::premium);
  EXPECT_EQ(result.plan().tier_label, "TIER_30");
  EXPECT_EQ(result.plan().sku_id, "IP0111-PPSA07040_00-FAKE0092TITLE000-E008");
  EXPECT_EQ(result.plan().product_name,
            "PlayStation Plus Premium: 3 Month Subscription");
  EXPECT_EQ(result.plan().end_date, "2030-01-01T00:00:00.000Z");
  EXPECT_EQ(result.is_ps_plus_member(), true);
}

TEST_F(MembershipFromSubscriptions, ReadsAnActiveExtraAsExtra) {
  const auto result = parse(entry("TIER_20", "X-PLUS2T03M-E004", "true"));
  EXPECT_EQ(result.tier(), Tier::extra);
  EXPECT_EQ(result.plan().tier_label, "TIER_20");
  EXPECT_EQ(result.plan().sku_id, "X-PLUS2T03M-E004");
}

TEST_F(MembershipFromSubscriptions, ReadsAnActiveEssentialAsEssential) {
  EXPECT_EQ(parse(entry("TIER_10", "X-PLUS1T03M-E004", "true")).tier(),
            Tier::essential);
}

TEST_F(MembershipFromSubscriptions, ReadsInactiveOrEmptyAsNone) {
  const auto inactive = parse(entry("TIER_30", "X-PLUS3T03M-E008", "false"));
  EXPECT_EQ(inactive.tier(), Tier::none);
  EXPECT_TRUE(inactive.plan().tier_label.empty());
  EXPECT_EQ(inactive.is_ps_plus_member(), false);
  EXPECT_EQ(parse("").tier(), Tier::none);
}

TEST_F(MembershipFromSubscriptions, IgnoresOtherSubscriptionTypes) {
  EXPECT_EQ(
      parse(entry("TIER_30", "X-PLUS3T03M-E008", "true", "EA_PLAY")).tier(),
      Tier::none);
  EXPECT_EQ(
      parse(entry("TIER_30", "X-PLUS3T03M-E008", "true", "ps_plus")).tier(),
      Tier::none);
  EXPECT_EQ(parse(entry("TIER_30", "X-PLUS3T03M-E008", "true", "EA_PLAY") +
                  "," + entry("TIER_20", "X-PLUS2T03M-E004", "true"))
                .tier(),
            Tier::extra);
}

TEST_F(MembershipFromSubscriptions, TakesTheHighestActiveTier) {
  const auto result = parse(entry("TIER_20", "X-PLUS2T03M-E004", "true") + "," +
                            entry("TIER_30", "X-PLUS3T03M-E008", "true") + "," +
                            entry("TIER_10", "X-PLUS1T03M-E004", "true"));
  EXPECT_EQ(result.tier(), Tier::premium);
  EXPECT_EQ(result.plan().sku_id, "X-PLUS3T03M-E008");
  EXPECT_EQ(parse(entry("TIER_30", "X-PLUS3T03M-E008", "false") + "," +
                  entry("TIER_10", "X-PLUS1T03M-E004", "true"))
                .tier(),
            Tier::essential);
}

TEST_F(MembershipFromSubscriptions, LeavesAMalformedEntryUnknown) {
  EXPECT_EQ(parse(entry("TIER_30", "X-PLUS2T03M-E008", "true")).tier(),
            std::nullopt);
  EXPECT_EQ(parse(entry("TIER_30", "X-NOTIER-E008", "true")).tier(),
            Tier::premium);
  EXPECT_EQ(parse(entry("TIER_20", "X-PROMOBUNDLE-E008", "true")).tier(),
            Tier::extra);
  EXPECT_EQ(parse(entry("TIER_40", "X-PLUS3T03M-E008", "true")).tier(),
            std::nullopt);
  EXPECT_EQ(parse(entry("TIER_30", "X-PLUS3T03M-E008", R"("true")")).tier(),
            std::nullopt);
  EXPECT_EQ(parse(entry("TIER_30", "X-PLUS3T03M-E008", "null")).tier(),
            std::nullopt);
  EXPECT_EQ(parse(entry("TIER_30", "X-PLUS3T03M-E008", "1")).tier(),
            std::nullopt);
  EXPECT_EQ(parse(entry("TIER_20", "X-PLUS2T03M-E004", "true") + "," +
                  entry("TIER_30", "X-PLUS2T03M-E008", "true"))
                .tier(),
            std::nullopt);
  EXPECT_EQ(
      parse(R"({"active":true,"tierLabel":"TIER_30","id":"X-PLUS3T03M-E008"})")
          .tier(),
      std::nullopt);
  EXPECT_EQ(
      parse(
          R"({"active":true,"subscriptionType":"PS_PLUS","id":"X-PLUS3T03M-E008"})")
          .tier(),
      std::nullopt);
  EXPECT_EQ(
      parse(
          R"({"active":true,"subscriptionType":"PS_PLUS","tierLabel":"TIER_30"})")
          .tier(),
      std::nullopt);
  EXPECT_EQ(parse(R"("PS_PLUS")").tier(), std::nullopt);
}

TEST_F(MembershipFromSubscriptions, LeavesAMalformedEnvelopeUnknown) {
  EXPECT_EQ(tier_of(enveloped("{}")), std::nullopt);
  EXPECT_EQ(tier_of(enveloped(R"({"fetchSubscriptions":null})")), std::nullopt);
  EXPECT_EQ(tier_of(enveloped(R"({"fetchSubscriptions":{}})")), std::nullopt);
  EXPECT_EQ(
      tier_of(enveloped(R"({"fetchSubscriptions":{"subscriptions":{}}})")),
      std::nullopt);
  EXPECT_EQ(tier_of(enveloped(R"({"fetchSubscriptions":[]})")), std::nullopt);
}

TEST_F(MembershipFromSubscriptions, ReportsAMalformedReplyAsMalformed) {
  const auto malformed = fetch(enveloped(R"({"fetchSubscriptions":null})"));
  ASSERT_FALSE(malformed);
  ASSERT_TRUE(malformed.error().cause());
  EXPECT_EQ(malformed.error().cause()->kind(),
            graphql::GraphqlError::Kind::malformed);
  EXPECT_EQ(malformed.error().status(), graphql::CheckStatus::malformed);
  EXPECT_EQ(malformed.error().operation(), "getSubscriptions");
  EXPECT_EQ(malformed.error().http_status(), 200);
}

TEST_F(MembershipJson, DescribesThePlanWithoutTheSubscriptionId) {
  const auto fetched = fetch(Fixtures::text("get_subscriptions.json"));
  ASSERT_TRUE(fetched);
  auto result = fetched.value();
  result.cached = true;
  result.fetched_at_ms = 1234;
  const auto text = users::MembershipCalls::membership_json(result);
  EXPECT_EQ(text.find("subscriptionId"), std::string::npos);
  EXPECT_EQ(text.find("SUB001"), std::string::npos);
  EXPECT_EQ(text.find("FIXTURE000000000"), std::string::npos);
  EXPECT_EQ(
      text,
      R"({"status":"ok","tier":"premium","tierLabel":"TIER_30",)"
      R"("skuId":"IP0111-PPSA07040_00-FAKE0092TITLE000-E008",)"
      R"("productName":"PlayStation Plus Premium: 3 Month Subscription",)"
      R"("endDate":"2030-01-01T00:00:00.000Z","active":true,"cached":true,"fetchedAtMs":1234})");
  const auto parsed = Fixtures::json(text);
  EXPECT_EQ(JsonTree::text_at(parsed, "status"), "ok");
  EXPECT_EQ(JsonTree::flag_at(parsed, "active"), true);
  EXPECT_FALSE(parsed.contains("plan"));
  EXPECT_FALSE(parsed.contains("renewal"));
  EXPECT_EQ(
      users::MembershipCalls::membership_json(result, false),
      R"({"status":"ok","tier":"premium","tierLabel":"TIER_30",)"
      R"("skuId":"IP0111-PPSA07040_00-FAKE0092TITLE000-E008",)"
      R"("productName":"PlayStation Plus Premium: 3 Month Subscription",)"
      R"("endDate":"2030-01-01T00:00:00.000Z","active":true,"fetchedAtMs":1234,"fresh":false})");
}

TEST_F(MembershipJson, DescribesALapsedMembershipAsNone) {
  const users::MembershipAnswer none{
      libchiaking::AccountId{1},
      parse(entry("TIER_30", "X-PLUS3T03M-E008", "false")), false, 0};
  const auto lapsed_text = users::MembershipCalls::membership_json(none);
  const auto lapsed = Fixtures::json(lapsed_text);
  EXPECT_EQ(JsonTree::text_at(lapsed, "tier"), "none");
  EXPECT_EQ(JsonTree::flag_at(lapsed, "active"), false);
  EXPECT_EQ(lapsed_text.find("SUB001"), std::string::npos);
}

TEST_F(MembershipJson, SummarisesAFailedCheck) {
  const graphql::CheckError failed{graphql::GraphqlError::replied(
      "getSubscriptions", graphql::GraphqlError::Kind::server_error, 500, 2)};
  EXPECT_EQ(
      failed.summary(),
      R"({"status":"unavailable","operation":"getSubscriptions","graphqlStatus":"unavailable",)"
      R"("httpStatus":500,"code":"","attempts":2})");
}

}  // namespace
}  // namespace users_test
