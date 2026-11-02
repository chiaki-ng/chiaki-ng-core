#include "src/core/psn/auth/credentials.hpp"
#include "tests/unit/core/psn/auth/sign_in_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string_view>

namespace psn_test {
namespace {

using libchiaking::psn::auth::SignInErrorKind;
using Reply = ClientRig::Reply;

class TokenReply : public ::testing::Test {
 protected:
  static constexpr std::chrono::system_clock::time_point obtained{
      std::chrono::seconds{100}};

  SignInRig rig;
};

TEST_F(TokenReply, IsMalformedWhenAFieldIsMissingOrWrong) {
  for (
      const std::string_view body :
      {R"({"access_token":"access","refresh_token":"refresh","expires_in":0,"token_type":"bearer"})",
       R"({"access_token":"access","refresh_token":"refresh","expires_in":-5,"token_type":"bearer"})",
       R"({"access_token":"access","refresh_token":"refresh","expires_in":3600.5,"token_type":"bearer"})",
       R"({"access_token":"access","refresh_token":"refresh","expires_in":"3600","token_type":"bearer"})",
       R"({"access_token":"access","refresh_token":"refresh","expires_in":315360001,"token_type":"bearer"})",
       R"({"access_token":"access","refresh_token":"refresh","expires_in":3600})",
       R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"token_type":"mac"})",
       R"({"access_token":"access","refresh_token":"refresh","token_type":"bearer"})",
       R"({"access_token":"access","expires_in":3600,"token_type":"bearer"})",
       R"({"access_token":"","refresh_token":"refresh","expires_in":3600,"token_type":"bearer"})",
       R"({"access_token":"acc;ess","refresh_token":"refresh","expires_in":3600,"token_type":"bearer"})",
       R"({"access_token":42,"refresh_token":"refresh","expires_in":3600,"token_type":"bearer"})",
       R"([])", "not json"}) {
    SCOPED_TRACE(body);
    const auto result = rig.remote_refresh_outcome(SignInRig::json(body));
    EXPECT_EQ(SignInRig::kind_of(result), SignInErrorKind::malformed);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().http_status(), 200);
  }
}

TEST_F(TokenReply, AcceptsTheLongestLifetimeAndAnyBearerCase) {
  EXPECT_TRUE(rig.remote_refresh_outcome(SignInRig::json(
      R"({"access_token":"access","refresh_token":"refresh","expires_in":315360000,"token_type":"BEARER"})")));
}

TEST_F(TokenReply, NeedsABearerTypeForAppTokensOnlyWhenOneIsStated) {
  EXPECT_EQ(
      SignInRig::kind_of(rig.app_refresh_outcome(SignInRig::json(
          R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"token_type":"mac"})"))),
      SignInErrorKind::malformed);
  EXPECT_EQ(
      SignInRig::kind_of(rig.app_refresh_outcome(SignInRig::json(
          R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"token_type":null})"))),
      SignInErrorKind::malformed);
  EXPECT_TRUE(rig.app_refresh_outcome(SignInRig::json(
      R"({"access_token":"access","refresh_token":"refresh","expires_in":3600})")));
}

TEST_F(TokenReply, IgnoresExtraFieldsAndDecodesEscapes) {
  const auto extra = rig.remote_refresh_outcome(SignInRig::json(
      R"({"scope":7,"access_token":"access","id_token":{"a":[1]},"refresh_token":"refresh","expires_in":3600,"token_type":"bearer\u0000x"})"));
  ASSERT_TRUE(extra);
  EXPECT_EQ(extra.value().access.view(), "access");
  EXPECT_EQ(extra.value().expires_in, std::chrono::seconds{3600});
  const auto escaped = rig.remote_refresh_outcome(SignInRig::json(
      R"({"access_token":"abc","refresh_token":"refresh","expires_in":3600,"token_type":"bearer"})"));
  ASSERT_TRUE(escaped);
  EXPECT_EQ(escaped.value().access.view(), "abc");
}

TEST_F(TokenReply, ReadsARefreshLifetimeThatPsStates) {
  const auto stated = rig.remote_refresh_outcome(SignInRig::json(
      R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"refresh_token_expires_in":5184000,"token_type":"bearer"})"));
  ASSERT_TRUE(stated);
  EXPECT_EQ(stated.value().refresh_expires_in, std::chrono::seconds{5'184'000});
  EXPECT_EQ(stated.value().expires_in, std::chrono::seconds{3600});
  EXPECT_EQ(stated.value().refresh_expires_at(obtained),
            obtained + std::chrono::seconds{5'184'000});
  const auto app = rig.app_refresh_outcome(SignInRig::json(
      R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"refresh_token_expires_in":315360000})"));
  ASSERT_TRUE(app);
  EXPECT_EQ(app.value().refresh_expires_in, std::chrono::seconds{315'360'000});
}

TEST_F(TokenReply, LeavesAnUnusableRefreshLifetimeUnstated) {
  for (
      const std::string_view body :
      {R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"token_type":"bearer"})",
       R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"refresh_token_expires_in":0,"token_type":"bearer"})",
       R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"refresh_token_expires_in":-5,"token_type":"bearer"})",
       R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"refresh_token_expires_in":"5184000","token_type":"bearer"})",
       R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"refresh_token_expires_in":86400.5,"token_type":"bearer"})",
       R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"refresh_token_expires_in":null,"token_type":"bearer"})",
       R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"refresh_token_expires_in":315360001,"token_type":"bearer"})"}) {
    SCOPED_TRACE(body);
    const auto unstated = rig.remote_refresh_outcome(SignInRig::json(body));
    ASSERT_TRUE(unstated);
    EXPECT_FALSE(unstated.value().refresh_expires_in);
    EXPECT_FALSE(unstated.value().refresh_expires_at(obtained));
    EXPECT_EQ(unstated.value().refresh.view(), "refresh");
  }
}

TEST_F(TokenReply, MapsStatusesAndTransport) {
  for (const int code : {400, 401, 403}) {
    SCOPED_TRACE(code);
    const auto result = rig.remote_refresh_outcome(Reply{code, "{}"});
    EXPECT_EQ(SignInRig::kind_of(result), SignInErrorKind::rejected);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().http_status(), code);
  }
  EXPECT_EQ(SignInRig::kind_of(rig.remote_refresh_outcome(Reply{429, "{}"})),
            SignInErrorKind::rate_limited);
  EXPECT_EQ(SignInRig::kind_of(rig.remote_refresh_outcome(Reply{404, "{}"})),
            SignInErrorKind::unavailable);
  EXPECT_EQ(SignInRig::kind_of(rig.remote_refresh_outcome(Reply{503, "{}"})),
            SignInErrorKind::unavailable);
  EXPECT_EQ(SignInRig::kind_of(rig.remote_refresh_outcome(Reply{204, "{}"})),
            SignInErrorKind::unavailable);
  EXPECT_EQ(SignInRig::kind_of(rig.remote_refresh_outcome(Reply::transport())),
            SignInErrorKind::transport);
}

}  // namespace
}  // namespace psn_test
