#include "tests/unit/core/psn/auth/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace psn_test {
namespace {

using libchiaking::psn::auth::AuthErrorKind;
using libchiaking::psn::auth::BearerToken;

class CachedTokenInvalidation : public ::testing::Test {
 protected:
  CachedAuthRig rig;
};

TEST_F(CachedTokenInvalidation, DropsOnlyTheStaleToken) {
  rig.owned_tokens({AuthRig::redirect("https://x/#access_token=T1"),
                    AuthRig::redirect("https://x/#access_token=T2"),
                    AuthRig::redirect("https://x/#access_token=T3")});
  EXPECT_EQ(rig.owned_view(), "T1");
  rig.cache.invalidate(AuthRig::owned_grant(), BearerToken{"OTHER"});
  EXPECT_EQ(rig.owned_view(), "T1");
  rig.cache.invalidate(AuthRig::owned_grant(), BearerToken{"T1"});
  EXPECT_EQ(rig.owned_view(), "T2");
  rig.cache.invalidate(AuthRig::owned_grant());
  EXPECT_EQ(rig.owned_view(), "T3");
  EXPECT_EQ(rig.exchanges(), 3);
}

TEST_F(CachedTokenInvalidation, ClearForgetsEveryToken) {
  rig.owned_tokens({AuthRig::redirect("https://x/#access_token=T1"),
                    AuthRig::redirect("https://x/#access_token=T2")});
  EXPECT_EQ(rig.owned_view(), "T1");
  rig.cache.clear();
  EXPECT_EQ(rig.owned_view(), "T2");
  EXPECT_EQ(rig.exchanges(), 2);
}

TEST_F(CachedTokenInvalidation, ClearDuringAnExchangeDiscardsItsToken) {
  auto reply = AuthRig::redirect("https://x/#access_token=T1");
  reply.during = [this] { rig.cache.clear(); };
  rig.owned_tokens(
      {std::move(reply), AuthRig::redirect("https://x/#access_token=T2")});
  EXPECT_EQ(rig.owned_view(), "T1");
  EXPECT_EQ(rig.owned_view(), "T2");
}

TEST_F(CachedTokenInvalidation, AFiredStopSendsNothing) {
  rig.stop();
  EXPECT_EQ(AuthRig::kind_of(rig.owned()), AuthErrorKind::cancelled);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST(UncachedTokenRefresh, RenewsOnlyTheLatestToken) {
  AuthRig rig;
  rig.http.on("GET", AuthRig::checkout_url(AuthRig::gk_duid),
              {AuthRig::redirect("https://x/#access_token=FIRST"),
               AuthRig::redirect("https://x/#access_token=SECOND"),
               AuthRig::redirect("https://x/#access_token=THIRD")});
  const auto first =
      rig.auth.legacy_cloud_stream_checkout_token(AuthRig::gk_duid);
  const auto second =
      rig.auth.legacy_cloud_stream_checkout_token(AuthRig::gk_duid);
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value().view(), "FIRST");
  ASSERT_TRUE(second);
  EXPECT_EQ(second.value().view(), "SECOND");
  EXPECT_EQ(AuthRig::kind_of(rig.auth.refresh(first.value(), 401)),
            AuthErrorKind::invalid_request);
  EXPECT_EQ(rig.http.seen.size(), 2U);
  const auto renewed = rig.auth.refresh(second.value(), 401);
  ASSERT_TRUE(renewed);
  EXPECT_EQ(renewed.value().view(), "THIRD");
  EXPECT_EQ(rig.http.seen.size(), 3U);
}

}  // namespace
}  // namespace psn_test
