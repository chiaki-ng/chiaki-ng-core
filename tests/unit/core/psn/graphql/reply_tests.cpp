#include "tests/unit/core/psn/graphql/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace psn_test {
namespace {

using Kind = GraphqlRig::Kind;
using Reply = ClientRig::Reply;

TEST(GraphqlReply, IsPartialWhenDataComesWithErrors) {
  const auto partial = GraphqlRig::run({Reply{
      200,
      R"({"data":{"product":null,"other":true},"errors":[{"message":"no product","path":["product"]}]})"}});
  ASSERT_FALSE(partial);
  EXPECT_EQ(partial.error().kind(), Kind::partial);
  EXPECT_EQ(partial.error().name(), "partial");
  ASSERT_EQ(partial.error().errors().size(), 1U);
  EXPECT_EQ(partial.error().errors()[0].message, "no product");
  EXPECT_TRUE(partial.error().code().empty());
}

TEST(GraphqlReply, CarriesTheCodeOfAGraphqlError) {
  const auto missing = GraphqlRig::run({Reply{
      200,
      R"({"errors":[{"message":"PersistedQueryNotFound","extensions":{"code":"PERSISTED_QUERY_NOT_FOUND"}}]})"}});
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind(), Kind::errors);
  EXPECT_EQ(missing.error().name(), "graphql_error");
  EXPECT_EQ(missing.error().code(), "PERSISTED_QUERY_NOT_FOUND");
}

TEST(GraphqlReply, KeepsEveryErrorEvenWithoutAMessageOrCode) {
  const auto required = GraphqlRig::run({Reply{
      200,
      R"({"data":null,"errors":[{"message":"a","extensions":{"code":"PERSISTED_QUERY_ID_REQUIRED"}},)"
      R"({"message":"b"},"odd"]})"}});
  ASSERT_FALSE(required);
  EXPECT_EQ(required.error().kind(), Kind::errors);
  ASSERT_EQ(required.error().errors().size(), 3U);
  EXPECT_EQ(required.error().errors()[1].message, "b");
  EXPECT_TRUE(required.error().errors()[1].code.empty());
  EXPECT_TRUE(required.error().errors()[2].message.empty());
}

TEST(GraphqlReply, IsAnAuthRefusalFor401And403) {
  for (const int code : {401, 403}) {
    SCOPED_TRACE(code);
    const auto result = GraphqlRig::run({Reply{code, R"({"data":{"x":1}})"}});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind(), Kind::auth_refused);
    EXPECT_EQ(result.error().http_status(), code);
  }
}

TEST(GraphqlReply, IsRejectedForAnotherClientError) {
  for (const int code : {400, 404, 429}) {
    SCOPED_TRACE(code);
    const auto result = GraphqlRig::run({Reply{code, ""}});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind(), Kind::rejected);
    EXPECT_EQ(result.error().attempts(), 1);
  }
  const auto bad = GraphqlRig::run({Reply{
      400,
      R"({"errors":[{"message":"x","extensions":{"code":"PERSISTED_QUERY_NOT_FOUND"}}]})"}});
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().kind(), Kind::rejected);
  EXPECT_EQ(bad.error().code(), "PERSISTED_QUERY_NOT_FOUND");
}

TEST(GraphqlReply, IsUnavailableForAServerError) {
  for (const int code : {500, 502, 503}) {
    SCOPED_TRACE(code);
    const auto result = GraphqlRig::run({Reply{code, "<html>"}});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind(), Kind::server_error);
    EXPECT_EQ(result.error().name(), "unavailable");
    EXPECT_EQ(result.error().http_status(), code);
  }
}

TEST(GraphqlReply, CarriesTheTransportFailure) {
  const auto transport = GraphqlRig::run({Reply::transport()});
  ASSERT_FALSE(transport);
  EXPECT_EQ(transport.error().kind(), Kind::transport);
  EXPECT_EQ(transport.error().http_status(), 0);
  ASSERT_TRUE(transport.error().failure());
  EXPECT_EQ(transport.error().failure()->error(),
            libchiaking::platform::HttpError::disconnected);
}

TEST(GraphqlReply, IsInvalidJsonForABodyThatIsNotOneObject) {
  for (const std::string_view body :
       {"<html>", "[]", R"({"data":{}} x)", R"({"junk":[1,},"data":{}})"}) {
    SCOPED_TRACE(body);
    const auto result = GraphqlRig::run({Reply{200, std::string{body}}});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind(), Kind::invalid_json);
    EXPECT_EQ(result.error().name(), "malformed");
  }
}

TEST(GraphqlReply, IsMalformedWithoutDataOrErrors) {
  for (const std::string_view body : {"{}", R"({"data":null,"errors":[]})"}) {
    SCOPED_TRACE(body);
    EXPECT_EQ(
        GraphqlRig::kind_of(GraphqlRig::run({Reply{200, std::string{body}}})),
        Kind::malformed);
  }
  const auto blank = GraphqlRig::run({Reply{204, ""}});
  ASSERT_FALSE(blank);
  EXPECT_EQ(blank.error().name(), "malformed");
}

}  // namespace
}  // namespace psn_test
