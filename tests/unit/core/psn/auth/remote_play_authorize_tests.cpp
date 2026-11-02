#include "src/core/psn/auth/credentials.hpp"
#include "tests/unit/core/psn/auth/sign_in_rig.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace psn_test {
namespace {

using libchiaking::psn::auth::SignInErrorKind;
using Reply = ClientRig::Reply;

class RemotePlayAuthorize : public ::testing::Test {
 protected:
  static constexpr std::string_view base =
      "https://remoteplay.dl.playstation.net/remoteplay/redirect";

  static auto to_base(std::string_view suffix) -> Reply {
    return SignInRig::redirect(std::string{base} + std::string{suffix});
  }

  auto kind_after(Reply reply) -> std::optional<SignInErrorKind> {
    return SignInRig::kind_of(rig.remote_authorize_outcome(std::move(reply)));
  }

  SignInRig rig;
};

TEST_F(RemotePlayAuthorize, RefusesARedirectToAnotherHost) {
  EXPECT_EQ(kind_after(SignInRig::redirect(
                "https://attacker.example/remoteplay/redirect?code=stolen")),
            SignInErrorKind::malformed);
}

TEST_F(RemotePlayAuthorize, IsRejectedByAnErrorOrANonRedirect) {
  EXPECT_EQ(kind_after(to_base("?%65rror=denied&code=unsafe")),
            SignInErrorKind::rejected);
  EXPECT_EQ(kind_after(to_base("?error=login_required")),
            SignInErrorKind::rejected);
  EXPECT_EQ(kind_after(Reply{200, "<html></html>"}), SignInErrorKind::rejected);
}

TEST_F(RemotePlayAuthorize, RefusesARedirectWithoutOneCleanCode) {
  for (const std::string_view suffix :
       {"", "?", "?state=1", "?code", "?code=", "?code=a&code=b",
        "?code=a#frag", "/?code=a", "?code=a+b", "?code=%2", "?code=%zz",
        "?code=a%20b", "?&code=a", "x?code=a"})
    EXPECT_EQ(kind_after(to_base(suffix)), SignInErrorKind::malformed)
        << suffix;
  EXPECT_EQ(kind_after(Reply{302, ""}), SignInErrorKind::malformed);
}

TEST_F(RemotePlayAuthorize, FollowsEverySeeOtherStyleRedirect) {
  for (const int status : {302, 303, 307, 308})
    EXPECT_TRUE(rig.remote_authorize_outcome(
        SignInRig::redirect(std::string{SignInRig::remote_redirect}, status)))
        << status;
  EXPECT_TRUE(rig.remote_authorize_outcome(to_base("?state=1&code=abc&")));
}

TEST_F(RemotePlayAuthorize, RefusesAnOverlongCode) {
  const auto too_long =
      rig.remote_authorize_outcome(to_base("?code=" + std::string(8192, 'a')));
  EXPECT_EQ(SignInRig::kind_of(too_long), SignInErrorKind::malformed);
  ASSERT_FALSE(too_long);
  EXPECT_EQ(too_long.error().http_status(), 302);
}

TEST_F(RemotePlayAuthorize, MapsStatusesAndTransport) {
  EXPECT_EQ(kind_after(Reply{429, ""}), SignInErrorKind::rate_limited);
  EXPECT_EQ(kind_after(Reply{403, ""}), SignInErrorKind::rejected);
  EXPECT_EQ(kind_after(Reply{500, ""}), SignInErrorKind::unavailable);
  EXPECT_EQ(kind_after(Reply{301, ""}), SignInErrorKind::unavailable);
  EXPECT_EQ(kind_after(Reply::transport()), SignInErrorKind::transport);
}

}  // namespace
}  // namespace psn_test
