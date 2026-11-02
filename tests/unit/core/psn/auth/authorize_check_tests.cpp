#include "tests/unit/core/psn/auth/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

namespace psn_test {
namespace {

using libchiaking::psn::StreamingService;
using libchiaking::psn::auth::AuthErrorKind;
using Reply = ClientRig::Reply;

class AuthorizeCheck : public ::testing::Test {
 protected:
  static constexpr std::string_view body =
      R"({"client_id":"19ae39c4-3f88-4d11-a792-94e4f52c996d","scope":"id_token:psn.basic_claims kamaji:s2s.subscriptionsPremium.get id_token:duid id_token:online_id openid psn:s2s","redirect_uri":"gaikai://local","response_type":"code","service_entity":"urn:service-entity:psn","duid":"000000070041008000112233445566778899aabbccddeeff"})";

  static void answers(AuthRig& rig, Reply reply) {
    rig.http.on("POST", std::string{AuthRig::authorize_check_url},
                {std::move(reply)});
  }

  static auto check(
      AuthRig& rig,
      StreamingService service = StreamingService::legacy_cloud_stream,
      std::string_view duid = AuthRig::gk_duid) {
    return rig.auth.authorize_check(service, duid);
  }
};

TEST_F(AuthorizeCheck, PostsTheBodyWithTheNpssoCookie) {
  for (const int status : {200, 204, 299}) {
    SCOPED_TRACE(status);
    AuthRig rig;
    answers(rig, Reply{status, ""});
    EXPECT_TRUE(check(rig, StreamingService::cloud_stream));
    ASSERT_FALSE(rig.http.seen.empty());
    const auto& seen = rig.http.seen.front();
    EXPECT_EQ(seen.method, "POST");
    EXPECT_EQ(seen.body, body);
    ASSERT_EQ(seen.headers.size(), 3U);
    EXPECT_EQ(seen.headers[0].name, "Content-Type");
    EXPECT_EQ(seen.headers[0].value, "application/json; charset=UTF-8");
    EXPECT_EQ(seen.headers[1].name, "User-Agent");
    EXPECT_EQ(seen.headers[1].value, AuthRig::cloud_stream_agent);
    EXPECT_EQ(seen.headers[2].name, "Cookie");
    EXPECT_EQ(seen.headers[2].value, "npsso=NPSSO-FAKE");
  }
}

TEST_F(AuthorizeCheck, RejectsAnyStatusOutsideSuccess) {
  for (const int status : {199, 300, 401, 500}) {
    SCOPED_TRACE(status);
    AuthRig rig;
    answers(rig, Reply{status, ""});
    const auto result = check(rig);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind(), AuthErrorKind::rejected);
    EXPECT_EQ(result.error().http_status(), status);
    ASSERT_FALSE(rig.http.seen.empty());
    EXPECT_EQ(rig.http.seen.front().header("User-Agent"),
              AuthRig::legacy_cloud_stream_agent);
  }
}

TEST_F(AuthorizeCheck, ReportsATransportFailure) {
  AuthRig lost;
  answers(lost, Reply::transport());
  EXPECT_EQ(AuthRig::kind_of(check(lost)), AuthErrorKind::transport);
}

TEST_F(AuthorizeCheck, IsCancelledBeforeTheNpssoIsChecked) {
  AuthRig stopped{"BAD TOKEN"};
  stopped.stop();
  EXPECT_EQ(AuthRig::kind_of(check(stopped)), AuthErrorKind::cancelled);
  EXPECT_TRUE(stopped.http.seen.empty());
}

TEST_F(AuthorizeCheck, RefusesAnInvalidNpssoBeforeSending) {
  AuthRig invalid{"BAD TOKEN"};
  EXPECT_EQ(AuthRig::kind_of(check(invalid)), AuthErrorKind::invalid_npsso);
  EXPECT_TRUE(invalid.http.seen.empty());
}

TEST_F(AuthorizeCheck, RefusesAnInvalidDuidOrServiceBeforeSending) {
  AuthRig duid;
  EXPECT_EQ(AuthRig::kind_of(
                check(duid, StreamingService::legacy_cloud_stream, "short")),
            AuthErrorKind::invalid_request);
  EXPECT_EQ(AuthRig::kind_of(check(duid, static_cast<StreamingService>(9))),
            AuthErrorKind::invalid_request);
  EXPECT_TRUE(duid.http.seen.empty());
}

TEST_F(AuthorizeCheck, KeepsItsReplyAfterAStop) {
  AuthRig rig;
  answers(rig, rig.stopping_during(Reply{204, ""}));
  EXPECT_TRUE(check(rig));
}

}  // namespace
}  // namespace psn_test
