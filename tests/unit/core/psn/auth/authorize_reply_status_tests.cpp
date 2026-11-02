#include "tests/unit/core/psn/auth/rig.hpp"

#include <gtest/gtest.h>

namespace psn_test {
namespace {

using libchiaking::psn::auth::AuthErrorKind;
using Reply = ClientRig::Reply;

class AuthorizeReplyStatus : public ::testing::Test {
 protected:
  AuthRig rig;
};

TEST_F(AuthorizeReplyStatus, IsMalformedForARedirectWithoutALocation) {
  const auto none = rig.token(Reply{302, ""});
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().kind(), AuthErrorKind::malformed);
  EXPECT_TRUE(none.error().denied());
  AuthRig blank_location;
  EXPECT_EQ(AuthRig::kind_of(blank_location.token(AuthRig::redirect(""))),
            AuthErrorKind::malformed);
}

TEST_F(AuthorizeReplyStatus, IsRejectedForAClientErrorOrANonRedirect) {
  const auto refused = rig.token(Reply{400, "{}"});
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind(), AuthErrorKind::rejected);
  EXPECT_EQ(refused.error().http_status(), 400);
  EXPECT_TRUE(refused.error().denied());
  AuthRig forbidden;
  EXPECT_EQ(AuthRig::kind_of(forbidden.token(Reply{403, ""})),
            AuthErrorKind::rejected);
  AuthRig plain;
  EXPECT_EQ(AuthRig::kind_of(plain.token(Reply{200, "<html>"})),
            AuthErrorKind::rejected);
  AuthRig other_redirect;
  Reply moved{301, ""};
  moved.headers.push_back({"Location", "https://x/#access_token=T"});
  EXPECT_EQ(AuthRig::kind_of(other_redirect.token(moved)),
            AuthErrorKind::rejected);
}

TEST_F(AuthorizeReplyStatus, IsAServerErrorThatDoesNotDenyTheNpsso) {
  const auto failed = rig.token(Reply{503, ""});
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().kind(), AuthErrorKind::server_error);
  EXPECT_EQ(failed.error().http_status(), 503);
  EXPECT_FALSE(failed.error().denied());
}

TEST_F(AuthorizeReplyStatus, IsALoggedTransportFailureWhenTheLinkDrops) {
  const auto lost = rig.token(Reply::transport());
  ASSERT_FALSE(lost);
  EXPECT_EQ(lost.error().kind(), AuthErrorKind::transport);
  EXPECT_EQ(lost.error().http_status(), 0);
  EXPECT_TRUE(rig.log.contains(
      "[AUTH] psn.auth.commerce_token failed: transport HTTP 0 auth=npsso"));
}

}  // namespace
}  // namespace psn_test
