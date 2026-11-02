#include "src/core/psn/graphql/authenticated/operations.hpp"
#include "tests/unit/core/psn/graphql/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace psn_test {
namespace {

using Auth = GraphqlRig::Auth;
using Kind = GraphqlRig::Kind;
using Operation = GraphqlRig::Operation;
using Reply = ClientRig::Reply;

class GraphqlSender : public ::testing::Test {
 protected:
  static auto never_sent(const Operation& operation)
      -> testing::AssertionResult {
    GraphqlRig fresh;
    const auto result = fresh.send(operation);
    if (!fresh.http.seen.empty())
      return testing::AssertionFailure()
             << fresh.http.seen.size() << " requests went out";
    if (result) return testing::AssertionFailure() << "the operation was sent";
    if (result.error().kind() != Kind::invalid_request)
      return testing::AssertionFailure()
             << "refused as " << result.error().name();
    return testing::AssertionSuccess();
  }

  const Operation required =
      GraphqlRig::with_auth(GraphqlRig::probe, Auth::required);
  GraphqlRig rig;
};

TEST_F(GraphqlSender, SendsOneGetWithDefaultHeadersAndNoCredentials) {
  rig.http.on("GET", GraphqlRig::url_of(GraphqlRig::probe, "{}"),
              {Reply{200, R"({"data":{"x":1}})"}});
  GraphqlRig::Endpoint endpoint;
  endpoint.connect_timeout = std::chrono::milliseconds{6000};
  endpoint.total_timeout = std::chrono::milliseconds{7000};
  const auto result = rig.send(GraphqlRig::probe, endpoint);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().attempts, 1);
  EXPECT_EQ(result.value().http_status, 200);
  EXPECT_EQ(result.value().data, R"({"x":1})");
  ASSERT_FALSE(rig.http.seen.empty());
  const auto& seen = rig.http.seen.at(0);
  EXPECT_EQ(seen.method, "GET");
  EXPECT_TRUE(seen.body.empty());
  EXPECT_EQ(seen.total_timeout, std::chrono::milliseconds{7000});
  EXPECT_EQ(seen.connect_timeout, std::chrono::milliseconds{6000});
  EXPECT_EQ(seen.headers.size(), 2U);
  EXPECT_EQ(seen.header("Content-Type"), "application/json");
  EXPECT_EQ(seen.header("Accept"), "application/json");
}

TEST_F(GraphqlSender, NeverLeasesOrSendsABearerForAPublicOperation) {
  rig.supply("token-1");
  rig.http.on("GET", GraphqlRig::url_of(GraphqlRig::probe, "{}"),
              {Reply{200, R"({"data":{}})"}});
  EXPECT_TRUE(rig.send(GraphqlRig::probe));
  ASSERT_FALSE(rig.http.seen.empty());
  EXPECT_TRUE(rig.http.seen[0].header("Authorization").empty());
  EXPECT_TRUE(rig.http.seen[0].header("X-Psn-App-Ver").empty());
  EXPECT_EQ(rig.app_token_uses(), 0U);
}

TEST_F(GraphqlSender, RefusesAPublicOperationOnTheAuthenticatedPath) {
  rig.supply("token-1");
  rig.http.on("GET", GraphqlRig::url_of(GraphqlRig::probe, "{}"),
              {Reply{200, R"({"data":{}})"}});
  const auto refused = rig.send(GraphqlRig::probe, {}, &rig.slot);
  EXPECT_EQ(GraphqlRig::kind_of(refused), Kind::invalid_request);
  EXPECT_TRUE(rig.http.seen.empty());
  EXPECT_TRUE(rig.stats.entries().empty());
  EXPECT_EQ(rig.app_token_uses(), 0U);
}

TEST_F(GraphqlSender, RefusesAnAuthenticatedOperationOnThePublicPath) {
  rig.supply("token-1");
  for (const auto& operation :
       libchiaking::psn::graphql::authenticated::operations)
    EXPECT_EQ(GraphqlRig::kind_of(rig.send(operation)),
              Kind::missing_credential)
        << operation.name;
  EXPECT_TRUE(rig.http.seen.empty());
  EXPECT_EQ(rig.app_token_uses(), 0U);
}

TEST_F(GraphqlSender, NeverSendsARequiredOperationWithoutACredential) {
  const auto none = rig.send(required, {}, &rig.slot);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().kind(), Kind::missing_credential);
  EXPECT_EQ(none.error().credential_name(), "missing");
  EXPECT_EQ(none.error().attempts(), 0);
  EXPECT_TRUE(rig.http.seen.empty());
  EXPECT_TRUE(rig.stats.entries().empty());
}

TEST_F(GraphqlSender, NeverSendsARequiredOperationWithAnExpiredCredential) {
  rig.supply("old", GraphqlRig::wall_start_ms + 10);
  const auto stale = rig.send(required, {}, &rig.slot);
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().credential_name(), "expired");
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(GraphqlSender, NeverSendsARequiredOperationWithoutASlot) {
  const auto bare = rig.send(required);
  ASSERT_FALSE(bare);
  EXPECT_EQ(bare.error().kind(), Kind::missing_credential);
  EXPECT_FALSE(bare.error().refusal());
}

TEST_F(GraphqlSender, SendsTheBearerOnEveryAttemptAndCountsOneUse) {
  rig.supply("token-1");
  rig.http.on("GET", GraphqlRig::url_of(required, "{}"),
              {Reply{503, ""}, Reply{200, R"({"data":{}})"}});
  const auto ok = rig.send(required, GraphqlRig::retrying(1), &rig.slot);
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok.value().attempts, 2);
  EXPECT_EQ(rig.http.seen.size(), 2U);
  for (const auto& seen : rig.http.seen)
    EXPECT_EQ(seen.header("Authorization"), "Bearer token-1");
  const auto tokens = rig.snapshot().tokens;
  ASSERT_FALSE(tokens.empty());
  EXPECT_EQ(tokens.front().usage.uses, 1U);
  EXPECT_EQ(tokens.front().usage.retries, 1U);
}

TEST_F(GraphqlSender, MarksARefusedToken) {
  rig.supply("token-1");
  rig.http.on("GET", GraphqlRig::url_of(required, "{}"),
              {Reply{401, R"({"error":"invalid_token"})"}});
  const auto refused = rig.send(required, {}, &rig.slot);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind(), Kind::auth_refused);
  EXPECT_EQ(refused.error().http_status(), 401);
  const auto again = rig.send(required, {}, &rig.slot);
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error().kind(), Kind::missing_credential);
  EXPECT_EQ(again.error().credential_name(), "refused");
  EXPECT_EQ(rig.http.seen.size(), 1U);
}

TEST_F(GraphqlSender, RecordsNothingForACancelledRequest) {
  rig.supply("token-1");
  rig.stop();
  const auto result = rig.send(required, {}, &rig.slot);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), Kind::cancelled);
  EXPECT_EQ(result.error().attempts(), 0);
  EXPECT_TRUE(rig.http.seen.empty());
  EXPECT_TRUE(rig.stats.entries().empty());
  EXPECT_EQ(rig.app_token_uses(), 0U);
}

TEST_F(GraphqlSender, NeverSendsAnInvalidOperation) {
  auto short_hash = GraphqlRig::probe;
  short_hash.sha256 = "abc";
  EXPECT_TRUE(never_sent(short_hash));
  auto upper_hash = GraphqlRig::probe;
  upper_hash.sha256 =
      "F5BF4ED3645ECC3A1063F75992D154A294C2C19B5150774F73C501FC834E368B";
  EXPECT_TRUE(never_sent(upper_hash));
  auto spaced = GraphqlRig::probe;
  spaced.name = "met Get";
  EXPECT_TRUE(never_sent(spaced));
  auto plain = GraphqlRig::probe;
  plain.endpoint = "http://web.np.playstation.com/api/graphql/v1/op";
  EXPECT_TRUE(never_sent(plain));
  auto query = GraphqlRig::probe;
  query.endpoint = "https://web.np.playstation.com/api/graphql/v1/op?x=1";
  EXPECT_TRUE(never_sent(query));
  auto version = GraphqlRig::probe;
  version.version = 0;
  EXPECT_TRUE(never_sent(version));
}

TEST_F(GraphqlSender, LogsAFailureWithoutTheToken) {
  rig.supply("secret-token");
  rig.http.on(
      "GET", GraphqlRig::url_of(required, "{}"),
      {Reply{
          200,
          R"({"errors":[{"message":"x","extensions":{"code":"PERSISTED_QUERY_NOT_FOUND"}}]})"}});
  static_cast<void>(rig.send(required, {}, &rig.slot));
  EXPECT_TRUE(rig.log.contains(
      "psn.graphql.web.metSubscriptionBenefitsQuery graphql_error HTTP 200 "
      "PERSISTED_QUERY_NOT_FOUND after 1 attempts"));
  EXPECT_TRUE(rig.log.contains("auth=app token (supplied)"));
  EXPECT_FALSE(rig.log.contains("secret-token"));
}

TEST_F(GraphqlSender, LogsAnExpiredCredential) {
  rig.supply("old", GraphqlRig::wall_start_ms + 10);
  static_cast<void>(rig.send(required, {}, &rig.slot));
  EXPECT_TRUE(rig.log.contains("no_credential credential expired"));
}

}  // namespace
}  // namespace psn_test
