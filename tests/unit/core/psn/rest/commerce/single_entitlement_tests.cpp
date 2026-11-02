#include "tests/support/json/tree.hpp"
#include "tests/unit/core/psn/rest/commerce/commerce_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace psn_test {
namespace {

using libchiaking::psn::rest::CommerceClient;
using libchiaking::psn::rest::CommerceError;
using test_support::JsonTree;

class CommerceSingleEntitlement : public CommerceFixture {
 protected:
  static auto id_of(const glz::raw_json& entitlement) -> std::string {
    const auto parsed = JsonTree::parse(entitlement.str);
    if (!parsed) return {};
    return std::string{JsonTree::text_at(*parsed, "id")};
  }
};

TEST_F(CommerceSingleEntitlement, IsAskedWithoutAnAgent) {
  rig.fake.on("GET", single_url("EP0000-CUSA%2012345"),
              {Reply{200, R"({"id":"EP0000"})"}});
  const auto result =
      client.internal_entitlement(bearer(), "EP0000-CUSA 12345");
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().owned);
  EXPECT_EQ(result.value().http_status, 200);
  ASSERT_TRUE(result.value().entitlement);
  EXPECT_EQ(id_of(*result.value().entitlement), "EP0000");
  ASSERT_FALSE(rig.fake.seen.empty());
  const auto& seen = rig.fake.seen.front();
  EXPECT_EQ(seen.method, "GET");
  ASSERT_EQ(seen.headers.size(), 2U);
  EXPECT_EQ(seen.headers[0].value, "Bearer BEARER-FAKE");
  EXPECT_EQ(seen.headers[1].name, "Accept");
  EXPECT_EQ(seen.headers[1].value, "application/json");
  EXPECT_EQ(seen.total_timeout, libchiaking::net::http::default_total_timeout);
}

TEST_F(CommerceSingleEntitlement, IsMatchedInAListing) {
  rig.fake.on(
      "GET", single_url("PROD"),
      {Reply{
          200,
          R"({"entitlements":[{"id":"OTHER"},{"id":"X","product_id":"PROD"}]})"}});
  rig.fake.on("GET", single_url("MISS"),
              {Reply{200, R"({"entitlements":[{"id":"OTHER"}]})"}});
  const auto found = client.internal_entitlement(bearer(), "PROD");
  ASSERT_TRUE(found);
  ASSERT_TRUE(found.value().entitlement);
  EXPECT_EQ(id_of(*found.value().entitlement), "X");
  const auto missing = client.internal_entitlement(bearer(), "MISS");
  ASSERT_TRUE(missing);
  EXPECT_TRUE(missing.value().owned);
  EXPECT_FALSE(missing.value().entitlement);
}

TEST_F(CommerceSingleEntitlement, CarriesTheAgentWhenGivenOne) {
  rig.fake.on("GET", single_url("ID"), {Reply{404, "{}"}});
  CommerceClient agent_client{rig.http, app_agent};
  const auto result = agent_client.internal_entitlement(bearer(), "ID");
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().owned);
  EXPECT_EQ(result.value().http_status, 404);
  EXPECT_FALSE(result.value().entitlement);
  ASSERT_FALSE(rig.fake.seen.empty());
  const auto& seen = rig.fake.seen.front();
  ASSERT_EQ(seen.headers.size(), 3U);
  EXPECT_EQ(seen.headers[0].name, "Authorization");
  EXPECT_EQ(seen.headers[1].name, "User-Agent");
  EXPECT_EQ(seen.headers[1].value, app_agent);
  EXPECT_EQ(seen.headers[2].name, "Accept");
  EXPECT_EQ(seen.headers[2].value, "application/json");
}

TEST_F(CommerceSingleEntitlement, CutsALongIdToItsBound) {
  const std::string id(400, 'E');
  rig.fake.on(
      "GET",
      single_url(id.substr(
          0, libchiaking::psn::rest::commerce_entitlement_id_capacity - 4)),
      {Reply{404, "{}"}});
  const auto result = client.internal_entitlement(bearer(), id);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().owned);
  EXPECT_EQ(result.value().http_status, 404);
}

TEST_F(CommerceSingleEntitlement, IsOwnedWithoutARecordWhenUnreadable) {
  rig.fake.on("GET", single_url("BAD"), {Reply{200, "not json"}});
  const auto bad = client.internal_entitlement(bearer(), "BAD");
  ASSERT_TRUE(bad);
  EXPECT_TRUE(bad.value().owned);
  EXPECT_FALSE(bad.value().entitlement);
}

TEST_F(CommerceSingleEntitlement, IsUnavailableWhenUnreachable) {
  rig.fake.on("GET", single_url("GONE"), {Reply::transport()});
  const auto gone = client.internal_entitlement(bearer(), "GONE");
  ASSERT_FALSE(gone);
  EXPECT_EQ(gone.error().reason(), CommerceError::Reason::unavailable);
  EXPECT_TRUE(gone.error().failure());
}

TEST_F(CommerceSingleEntitlement, IsRefusedWithA401) {
  rig.fake.on("GET", single_url("DENY"), {Reply{401, ""}});
  const auto deny = client.internal_entitlement(bearer(), "DENY");
  ASSERT_FALSE(deny);
  EXPECT_EQ(deny.error().reason(), CommerceError::Reason::auth_refused);
  EXPECT_EQ(deny.error().http_status(), 401);
}

TEST_F(CommerceSingleEntitlement, IsRejectedWithAnEmptyReply) {
  rig.fake.on("GET", single_url("EMPTY"), {Reply{204, ""}});
  const auto empty = client.internal_entitlement(bearer(), "EMPTY");
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().reason(), CommerceError::Reason::rejected);
  EXPECT_EQ(empty.error().http_status(), 204);
}

TEST_F(CommerceSingleEntitlement, IsCancelledOnceStopped) {
  rig.fake.on("GET", single_url("BAD"), {Reply{200, "not json"}});
  rig.stop();
  const auto stopped = client.internal_entitlement(bearer(), "BAD");
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().reason(), CommerceError::Reason::cancelled);
}

}  // namespace
}  // namespace psn_test
