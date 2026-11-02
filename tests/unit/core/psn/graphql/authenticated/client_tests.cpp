#include "src/core/psn/graphql/authenticated/client.hpp"
#include "src/core/psn/graphql/authenticated/operations.hpp"
#include "tests/support/psn/fixtures.hpp"
#include "tests/unit/core/psn/graphql/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psn_test {
namespace {

using Kind = GraphqlRig::Kind;
using Reply = ClientRig::Reply;
namespace authenticated = libchiaking::psn::graphql::authenticated;

class AuthenticatedGraphql : public ::testing::Test {
 protected:
  static constexpr std::string_view frozen_subscriptions_url =
      "https://web.np.playstation.com/api/graphql/v1/"
      "op?operationName=getSubscriptions&variables=%7B%7D&extensions="
      "%7B%22persistedQuery%22%3A%7B%22version%22%3A1%2C%22sha256Hash%22%3A%22"
      "36c5aa5049115a8b37008e45815c3cd31414d52660cd94a45fcdb17294800db1%22%7D%"
      "7D";
  static constexpr std::string_view frozen_concept_url =
      "https://m.np.playstation.com/api/graphql/v1/"
      "op?operationName=metGetConceptById&variables="
      "%7B%22conceptId%22%3A%22900002%22%7D&extensions=%7B%22persistedQuery%22%"
      "3A%7B%"
      "22version%22%3A1%2C"
      "%22sha256Hash%22%3A%"
      "2284b113e0c67e763f2f8bb2dbfc8f3d6cbc116d4c54ee33bf5cc42204ffde0feb%22%"
      "7D%"
      "7D";

  static auto has_app_headers(const test_support::psn::Seen& seen)
      -> testing::AssertionResult {
    if (seen.headers.size() < 4)
      return testing::AssertionFailure()
             << "only " << seen.headers.size() << " headers";
    const std::vector<std::pair<std::string_view, std::string_view>> expected{
        {"X-Psn-App-Ver", "PlayStationApp/26.9.2"},
        {"apollographql-client-name", "PlayStationApp"},
        {"apollographql-client-version", "26.9.2"},
        {"disable_query_whitelist", "false"}};
    for (std::size_t index = 0; index < expected.size(); ++index)
      if (seen.headers[index].name != expected[index].first ||
          seen.headers[index].value != expected[index].second)
        return testing::AssertionFailure()
               << "header " << index << " is " << seen.headers[index].name
               << ": " << seen.headers[index].value;
    return testing::AssertionSuccess();
  }

  GraphqlRig rig;
};

TEST_F(AuthenticatedGraphql, AsksForSubscriptionsAsThePsAppOnTheWeb) {
  rig.supply("APP-SECRET");
  rig.http.on("GET", std::string{frozen_subscriptions_url},
              {Reply{503, ""}, Reply{200, test_support::psn::Fixtures::text(
                                              "get_subscriptions.json")}});
  libchiaking::psn::graphql::AuthenticatedClient account{rig.client, rig.slot,
                                                         &rig.stats};
  const auto subscription = account.subscriptions();
  ASSERT_TRUE(subscription);
  ASSERT_EQ(subscription.value().active.size(), 1U);
  EXPECT_EQ(subscription.value().active.front().tier_label, "TIER_30");
  EXPECT_EQ(subscription.value().http_status, 200);
  EXPECT_EQ(subscription.value().attempts, 2);
  EXPECT_EQ(rig.http.seen.size(), 2U);
  EXPECT_EQ(rig.waiter.waits, (std::vector<int>{1000}));
  for (const auto& seen : rig.http.seen) {
    EXPECT_EQ(seen.url, frozen_subscriptions_url);
    EXPECT_EQ(seen.total_timeout, std::chrono::milliseconds{30'000});
    EXPECT_EQ(seen.connect_timeout, std::chrono::milliseconds{10'000});
    ASSERT_EQ(seen.headers.size(), 7U);
    EXPECT_TRUE(has_app_headers(seen));
    EXPECT_EQ(seen.headers[4].name, "Content-Type");
    EXPECT_EQ(seen.headers[4].value, "application/json");
    EXPECT_EQ(seen.headers[5].name, "Accept");
    EXPECT_EQ(seen.headers[5].value, "application/json");
    EXPECT_EQ(seen.headers[6].name, "Authorization");
    EXPECT_EQ(seen.headers[6].value, "Bearer APP-SECRET");
  }
  const auto entries = rig.stats.entries();
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].operation, "getSubscriptions");
  EXPECT_EQ(entries[0].requests, 2U);
  EXPECT_EQ(entries[0].failures, 0U);
  EXPECT_EQ(entries[0].last_http_status, 200);
  EXPECT_TRUE(rig.log.contains("auth=app token (supplied)"));
  EXPECT_FALSE(rig.log.contains("APP-SECRET"));
}

TEST_F(AuthenticatedGraphql, NeverAsksForSubscriptionsWithoutAToken) {
  libchiaking::psn::graphql::AuthenticatedClient account{rig.client, rig.slot};
  const auto refused = account.subscriptions();
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind(), Kind::missing_credential);
  EXPECT_EQ(refused.error().operation(), "getSubscriptions");
  EXPECT_EQ(refused.error().attempts(), 0);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(AuthenticatedGraphql, NeverSendsAMobileOperationWithoutAToken) {
  libchiaking::psn::graphql::AuthenticatedClient anonymous{rig.client,
                                                           rig.slot};
  const auto refused = anonymous.concept_by_id("900002", "en-GB");
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind(), Kind::missing_credential);
  EXPECT_TRUE(rig.http.seen.empty());
  EXPECT_EQ(refused.error().credential_name(), "missing");
}

TEST_F(AuthenticatedGraphql, LeasesThePsAppTokenForMobileOperations) {
  rig.supply("APP-SECRET");
  rig.http.on("GET", std::string{frozen_concept_url},
              {Reply{200, R"({"data":{"conceptRetrieve":null}})"}});
  rig.http.on("GET",
              GraphqlRig::url_of(authenticated::concept_pricing,
                                 R"({"conceptId":"900002"})"),
              {Reply{200, R"({"data":{}})"}});
  rig.http.on(
      "GET",
      GraphqlRig::url_of(authenticated::concept_by_product,
                         R"({"productId":"EP0109-PPSA01234_00-GAME 1~"})"),
      {Reply{200, R"({"data":{"productRetrieve":null}})"}});
  libchiaking::psn::graphql::AuthenticatedClient mobile{rig.client, rig.slot,
                                                        &rig.stats};
  EXPECT_EQ(GraphqlRig::kind_of(mobile.concept_by_id("900002", "en-GB")),
            Kind::not_found);
  auto details =
      libchiaking::psn::graphql::TitleDetails::of({.concept_id = "900002"});
  ASSERT_TRUE(details);
  EXPECT_EQ(
      GraphqlRig::kind_of(mobile.concept_pricing(std::move(*details), "en-GB")),
      Kind::malformed);
  EXPECT_EQ(GraphqlRig::kind_of(mobile.concept_by_product(
                "EP0109-PPSA01234_00-GAME 1~", "en-GB")),
            Kind::not_found);
  EXPECT_TRUE(rig.http.unmatched.empty());
  ASSERT_EQ(rig.http.seen.size(), 3U);
  EXPECT_EQ(rig.http.seen[0].url, frozen_concept_url);
  EXPECT_NE(rig.http.seen[2].url.find(
                "&variables=%7B%22productId%22%3A%22EP0109-PPSA01234_00-GAME+1%"
                "7E%"
                "22%7D&"),
            std::string::npos)
      << rig.http.seen[2].url;
  for (const auto& seen : rig.http.seen) {
    SCOPED_TRACE(seen.url);
    EXPECT_TRUE(seen.url.starts_with(
        "https://m.np.playstation.com/api/graphql/v1/op?operationName=met"));
    ASSERT_EQ(seen.headers.size(), 8U);
    EXPECT_TRUE(has_app_headers(seen));
    EXPECT_EQ(seen.headers[4].name, "x-psn-store-locale-override");
    EXPECT_EQ(seen.headers[4].value, "en-GB");
    EXPECT_EQ(seen.headers[5].name, "Content-Type");
    EXPECT_EQ(seen.headers[6].name, "Accept");
    EXPECT_EQ(seen.headers[7].name, "Authorization");
    EXPECT_EQ(seen.headers[7].value, "Bearer APP-SECRET");
  }
  EXPECT_EQ(rig.stats.entries().size(), 3U);
}

TEST_F(AuthenticatedGraphql, ReadsAConceptNamedByANumberAsText) {
  rig.supply("APP-SECRET");
  rig.http.on(
      "GET", std::string{frozen_concept_url},
      {Reply{
          200,
          R"({"data":{"conceptRetrieve":{"id":900002,"name":"Skyline",)"
          R"("releaseDate":{"value":"2017-03-01T00:00:00Z"},"contentRating":"PEGI",)"
          R"("combinedLocalizedGenres":[{"value":"Action"},{"value":7},"odd"]}}})"}});
  libchiaking::psn::graphql::AuthenticatedClient mobile{rig.client, rig.slot};
  const auto details = mobile.concept_by_id("900002", "en-GB");
  ASSERT_TRUE(details);
  EXPECT_EQ(details.value().concept_id(), "900002");
  EXPECT_EQ(details.value().name(), "Skyline");
  EXPECT_EQ(details.value().release_date(), "2017-03-01T00:00:00Z");
  EXPECT_TRUE(details.value().rating().authority.empty());
  EXPECT_EQ(details.value().genres(), (std::vector<std::string>{"Action"}));
  EXPECT_TRUE(details.value().products().empty());
  EXPECT_FALSE(details.value().priced());
}

}  // namespace
}  // namespace psn_test
