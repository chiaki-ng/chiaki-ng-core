#include "tests/unit/core/psn/auth/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::StreamingPlatform;
using libchiaking::psn::auth::AuthErrorKind;
using libchiaking::psn::auth::BearerToken;
using libchiaking::psn::auth::owned_entitlements_scope;

class AuthorizeRequests : public ::testing::Test {
 protected:
  static constexpr std::string_view title_url =
      "https://ca.account.sony.com/api/v1/oauth/"
      "authorize?smcid=pc:psnow&applicationId=psnow&response_type=code"
      "&scope=kamaji:commerce_native%20kamaji:commerce_container%20kamaji:"
      "lists%"
      "20kamaji:s2s.subscriptionsPremium.get"
      "&client_id=bc6b0777-abb5-40da-92ca-e133cf18e989"
      "&redirect_uri=https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
      "grc-response.html"
      "&service_entity=urn:service-entity:psn&prompt=none"
      "&renderMode=mobilePortrait&hidePageElements=forgotPasswordLink&"
      "displayFooter="
      "none&disableLinks=qriocityLink"
      "&mid=PSNOW&duid="
      "000000070041008000112233445566778899aabbccddeeff"
      "&layout_type=popup&service_logo=ps&tp_psn=true&noEVBlock=true";
  static constexpr std::string_view account_url =
      "https://ca.account.sony.com/api/v1/oauth/"
      "authorize?smcid=pc:psnow&applicationId=psnow&response_type=code"
      "&scope=kamaji:commerce_native%20kamaji:commerce_container%20kamaji:"
      "lists%"
      "20kamaji:s2s.subscriptionsPremium.get"
      "&client_id=bc6b0777-abb5-40da-92ca-e133cf18e989"
      "&redirect_uri=https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
      "grc-response.html"
      "&service_entity=urn:service-entity:psn&prompt=none"
      "&mid=PSNOW&duid="
      "000000070041008000112233445566778899aabbccddeeff"
      "&layout_type=popup&service_logo=ps&tp_psn=true&noEVBlock=true";
  static constexpr std::string_view ps3_account_url =
      "https://ca.account.sony.com/api/v1/oauth/"
      "authorize?smcid=pc:psnow&applicationId=psnow&response_type=code"
      "&scope=kamaji:commerce_native"
      "&client_id=bc6b0777-abb5-40da-92ca-e133cf18e989"
      "&redirect_uri=https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
      "grc-response.html"
      "&service_entity=urn:service-entity:psn&prompt=none"
      "&mid=PSNOW&duid="
      "000000070041008000112233445566778899aabbccddeeff"
      "&layout_type=popup&service_logo=ps&tp_psn=true&noEVBlock=true";

  static auto view_of(const auto& result) -> std::string {
    return result ? std::string{result.value().view()} : std::string{};
  }

  AuthRig rig;
};

TEST_F(AuthorizeRequests, BuildEveryFlowByteForByte) {
  rig.http.on("GET", std::string{AuthRig::owned_url},
              {AuthRig::redirect("https://x/#access_token=OWNED")});
  rig.http.on("GET", std::string{AuthRig::catalog_url},
              {AuthRig::redirect("https://psnow.playstation.com/app/2.2.0/133/"
                                 "5cdcc037d/grc-response.html?code=CATALOG")});
  rig.http.on("GET", std::string{title_url},
              {AuthRig::redirect("https://psnow.playstation.com/app/2.2.0/133/"
                                 "5cdcc037d/grc-response.html?code=TITLE")});
  rig.http.on("GET", std::string{account_url},
              {AuthRig::redirect("https://psnow.playstation.com/app/2.2.0/133/"
                                 "5cdcc037d/grc-response.html?code=ACCOUNT")});
  rig.http.on("GET", std::string{ps3_account_url},
              {AuthRig::redirect("https://psnow.playstation.com/app/2.2.0/133/"
                                 "5cdcc037d/grc-response.html?code=PS3")});
  rig.http.on("GET", AuthRig::checkout_url(AuthRig::gk_duid),
              {AuthRig::redirect("https://x/#access_token=CHECKOUT")});
  EXPECT_EQ(view_of(rig.auth.commerce_token(owned_entitlements_scope)),
            "OWNED");
  EXPECT_EQ(view_of(rig.auth.legacy_cloud_stream_catalog_code(
                "0000000700410080ab:cd")),
            "CATALOG");
  EXPECT_EQ(view_of(rig.auth.legacy_cloud_stream_title_code(AuthRig::gk_duid)),
            "TITLE");
  EXPECT_EQ(view_of(rig.auth.legacy_cloud_stream_account_code(
                AuthRig::gk_duid, StreamingPlatform::ps4)),
            "ACCOUNT");
  EXPECT_EQ(view_of(rig.auth.legacy_cloud_stream_account_code(
                AuthRig::gk_duid, StreamingPlatform::ps5)),
            "ACCOUNT");
  EXPECT_EQ(view_of(rig.auth.legacy_cloud_stream_account_code(
                AuthRig::gk_duid, StreamingPlatform::ps3)),
            "PS3");
  EXPECT_EQ(
      view_of(rig.auth.legacy_cloud_stream_checkout_token(AuthRig::gk_duid)),
      "CHECKOUT");
  EXPECT_TRUE(rig.http.unmatched.empty());
  ASSERT_EQ(rig.http.seen.size(), 7U);
  const std::vector<std::string_view> agents{
      AuthRig::kamaji_agent,
      AuthRig::legacy_cloud_stream_agent,
      AuthRig::legacy_cloud_stream_agent,
      AuthRig::legacy_cloud_stream_agent,
      AuthRig::legacy_cloud_stream_agent,
      AuthRig::legacy_cloud_stream_agent,
      AuthRig::legacy_cloud_stream_agent};
  for (std::size_t index = 0; index < rig.http.seen.size(); ++index) {
    SCOPED_TRACE(index);
    const auto& seen = rig.http.seen[index];
    EXPECT_EQ(seen.method, "GET");
    EXPECT_TRUE(seen.body.empty());
    ASSERT_EQ(seen.headers.size(), 2U);
    EXPECT_EQ(seen.header("User-Agent"), agents[index]);
    EXPECT_EQ(seen.header("Cookie"), "npsso=NPSSO-FAKE");
    EXPECT_EQ(seen.connect_timeout, std::chrono::seconds{10});
    EXPECT_EQ(seen.total_timeout, std::chrono::seconds{30});
  }
  EXPECT_EQ(rig.http.seen[0].headers[0].name, "Cookie");
  for (std::size_t index = 1; index < rig.http.seen.size(); ++index)
    EXPECT_EQ(rig.http.seen[index].headers[0].name, "User-Agent") << index;
  EXPECT_TRUE(rig.log.contains("psn.auth.commerce_token"));
  EXPECT_TRUE(rig.log.contains("psn.auth.legacy_cloud_stream_code"));
}

TEST_F(AuthorizeRequests, SendTheCookieFirstForACommerceToken) {
  const auto result = rig.token(
      AuthRig::redirect("https://psnow.playstation.com/"
                        "grc-response.html#access_token=TOKEN-1&token_type="
                        "bearer&expires_in=1199"));
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().view(), "TOKEN-1");
  ASSERT_EQ(rig.http.seen.size(), 1U);
  const auto& cookie_first = rig.http.seen.front();
  EXPECT_EQ(cookie_first.method, "GET");
  EXPECT_EQ(cookie_first.url, AuthRig::owned_url);
  ASSERT_EQ(cookie_first.headers.size(), 2U);
  EXPECT_EQ(cookie_first.headers[0].name, "Cookie");
  EXPECT_EQ(cookie_first.headers[0].value, "npsso=NPSSO-FAKE");
  EXPECT_EQ(cookie_first.headers[1].name, "User-Agent");
  EXPECT_EQ(cookie_first.headers[1].value, AuthRig::kamaji_agent);
}

TEST_F(AuthorizeRequests, SendTheUserAgentFirstForACatalogCode) {
  const auto coded = rig.code(
      AuthRig::redirect("https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
                        "grc-response.html?code=CODE-1&cid=abc"));
  ASSERT_TRUE(coded);
  EXPECT_EQ(coded.value().view(), "CODE-1");
  ASSERT_FALSE(rig.http.seen.empty());
  const auto& agent_first = rig.http.seen.front();
  EXPECT_EQ(agent_first.url, AuthRig::catalog_url);
  ASSERT_EQ(agent_first.headers.size(), 2U);
  EXPECT_EQ(agent_first.headers[0].name, "User-Agent");
  EXPECT_EQ(agent_first.headers[0].value, AuthRig::legacy_cloud_stream_agent);
  EXPECT_EQ(agent_first.headers[1].name, "Cookie");
  EXPECT_EQ(agent_first.headers[1].value, "npsso=NPSSO-FAKE");
}

TEST_F(AuthorizeRequests, SendEachNamedFlowWithoutLoggingSecrets) {
  rig.http.on("GET", std::string{title_url},
              {AuthRig::redirect("https://psnow.playstation.com/app/2.2.0/133/"
                                 "5cdcc037d/grc-response.html?code=TITLE")});
  rig.http.on("GET", std::string{account_url},
              {AuthRig::redirect("https://psnow.playstation.com/app/2.2.0/133/"
                                 "5cdcc037d/grc-response.html?code=ACCOUNT")});
  rig.http.on("GET", AuthRig::checkout_url(AuthRig::gk_duid),
              {AuthRig::redirect("https://x/#access_token=CHECKOUT")});
  rig.http.on("GET", std::string{AuthRig::owned_url},
              {AuthRig::redirect("https://x/#access_token=OWNED")});
  EXPECT_EQ(view_of(rig.auth.legacy_cloud_stream_title_code(AuthRig::gk_duid)),
            "TITLE");
  EXPECT_EQ(view_of(rig.auth.legacy_cloud_stream_account_code(
                AuthRig::gk_duid, StreamingPlatform::ps4)),
            "ACCOUNT");
  auto checkout = rig.auth.legacy_cloud_stream_checkout_token(AuthRig::gk_duid);
  ASSERT_TRUE(checkout);
  EXPECT_EQ(checkout.value().view(), "CHECKOUT");
  EXPECT_FALSE(rig.auth.renewable(checkout.value()));
  auto owned = rig.auth.commerce_token(owned_entitlements_scope);
  ASSERT_TRUE(owned);
  EXPECT_EQ(owned.value().view(), "OWNED");
  EXPECT_EQ(rig.http.seen.size(), 4U);
  EXPECT_TRUE(rig.log.contains("no token cache"));
  EXPECT_FALSE(rig.log.contains("NPSSO-FAKE"));
  EXPECT_FALSE(rig.log.contains("OWNED"));
  EXPECT_FALSE(rig.log.contains("CHECKOUT"));
  EXPECT_EQ(AuthRig::kind_of(rig.auth.refresh(BearerToken{"UNKNOWN"}, 401)),
            AuthErrorKind::invalid_request);
  EXPECT_EQ(rig.http.seen.size(), 4U);
}

TEST_F(AuthorizeRequests, RefuseAnInvalidNpssoBeforeSending) {
  for (const auto bad :
       {std::string_view{}, std::string_view{"with space"},
        std::string_view{"with\r\nnewline"}, std::string_view{"token;other=1"},
        std::string_view{"a,b"}}) {
    SCOPED_TRACE(bad);
    AuthRig refused{bad};
    const auto token =
        refused.token(AuthRig::redirect("https://x/#access_token=T"));
    ASSERT_FALSE(token);
    EXPECT_EQ(token.error().kind(), AuthErrorKind::invalid_npsso);
    EXPECT_TRUE(token.error().denied());
    EXPECT_EQ(AuthRig::kind_of(refused.code(
                  AuthRig::redirect("https://psnow.playstation.com/app/2.2.0/"
                                    "133/5cdcc037d/grc-response.html?code=C"))),
              AuthErrorKind::invalid_npsso);
    EXPECT_TRUE(refused.http.seen.empty());
  }
}

TEST_F(AuthorizeRequests, AcceptAnNpssoOfUpTo4096Characters) {
  const std::string longest(4096, 'x');
  AuthRig accepted{longest};
  EXPECT_TRUE(accepted.code(
      AuthRig::redirect("https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
                        "grc-response.html?code=C")));
  AuthRig refused{longest + "x"};
  EXPECT_EQ(AuthRig::kind_of(refused.code(
                AuthRig::redirect("https://psnow.playstation.com/app/2.2.0/133/"
                                  "5cdcc037d/grc-response.html?code=C"))),
            AuthErrorKind::invalid_npsso);
  EXPECT_TRUE(refused.http.seen.empty());
}

TEST_F(AuthorizeRequests, AreCancelledBeforeSendingByAFiredStop) {
  rig.stop();
  const auto stopped =
      rig.token(AuthRig::redirect("https://x/#access_token=T"));
  EXPECT_EQ(AuthRig::kind_of(stopped), AuthErrorKind::cancelled);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(AuthorizeRequests, DropATokenThatArrivesAfterAStop) {
  const auto result = rig.token(
      rig.stopping_during(AuthRig::redirect("https://x/#access_token=T")));
  EXPECT_EQ(AuthRig::kind_of(result), AuthErrorKind::cancelled);
}

TEST_F(AuthorizeRequests, DropACodeThatArrivesAfterAStop) {
  rig.http.on("GET", std::string{title_url},
              {rig.stopping_during(AuthRig::redirect(
                  "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
                  "grc-response.html?code=TITLE"))});
  EXPECT_EQ(AuthRig::kind_of(
                rig.auth.legacy_cloud_stream_title_code(AuthRig::gk_duid)),
            AuthErrorKind::cancelled);
}

}  // namespace
}  // namespace psn_test
