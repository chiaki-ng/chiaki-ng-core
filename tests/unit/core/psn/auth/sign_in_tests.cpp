#include "src/core/psn/auth/credentials.hpp"
#include "tests/unit/core/psn/auth/sign_in_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::AccountId;
using libchiaking::psn::auth::AppRefreshToken;
using libchiaking::psn::auth::BearerToken;
using libchiaking::psn::auth::Npsso;
using libchiaking::psn::auth::RemotePlayRefreshToken;
using libchiaking::psn::auth::SignInErrorKind;

class SignIn : public ::testing::Test {
 protected:
  static constexpr std::string_view remote_exchange_body =
      "grant_type=authorization_code&code=remote%2Bcode"
      "&redirect_uri=https%3A%2F%2Fremoteplay.dl.playstation.net%2Fremoteplay%"
      "2Fredirect"
      "&scope=psn%3Aclientapp+referenceDataService%3AcountryConfig.read"
      "+pushNotification%3AwebSocket.desktop.connect+sessionManager%"
      "3AremotePlaySession.system.update"
      "&client_id=ba495a24-818c-472b-b12d-ff231c1b5745&client_secret="
      "mvaiZkRsAsI1IBkY";
  static constexpr std::string_view remote_refresh_body =
      "grant_type=refresh_token&refresh_token=old-remote"
      "&redirect_uri=https%3A%2F%2Fremoteplay.dl.playstation.net%2Fremoteplay%"
      "2Fredirect"
      "&scope=psn%3Aclientapp+referenceDataService%3AcountryConfig.read"
      "+pushNotification%3AwebSocket.desktop.connect+sessionManager%"
      "3AremotePlaySession.system.update";
  static constexpr std::string_view app_refresh_body =
      "grant_type=refresh_token&refresh_token=old-mobile"
      "&scope=psn%3Amobile.v2.core+psn%3Aclientapp&token_format=jwt";

  SignInRig rig;
};

TEST_F(SignIn, SignsInBothAudiencesAndResolvesTheAccount) {
  rig.http.on("GET", std::string{SignInRig::remote_authorize_url},
              {SignInRig::redirect(std::string{SignInRig::remote_redirect})});
  rig.http.on("POST", std::string{SignInRig::token_url},
              {SignInRig::json(SignInRig::remote_tokens_json),
               SignInRig::json(SignInRig::app_tokens_json)});
  rig.http.on("GET", std::string{SignInRig::account_url},
              {SignInRig::json(SignInRig::account_json)});
  auto result = rig.sign_in.sign_in(SignInRig::npsso(), SignInRig::duid,
                                    SignInRig::correlation);
  ASSERT_TRUE(result);
  const auto& signed_in = result.value();
  EXPECT_EQ(signed_in.remote_play.access.view(), "remote-access");
  EXPECT_EQ(signed_in.remote_play.refresh.view(), "remote-refresh");
  EXPECT_EQ(signed_in.remote_play.expires_in, std::chrono::seconds{3600});
  EXPECT_EQ(signed_in.app.access.view(), "mobile-access");
  EXPECT_EQ(signed_in.app.refresh.view(), "mobile-refresh");
  EXPECT_EQ(signed_in.app.expires_in, std::chrono::seconds{7200});
  EXPECT_EQ(signed_in.account.online_id, "Tokyo Player");
  EXPECT_EQ(signed_in.account.id, AccountId{0x0807060504030201ULL});
  EXPECT_EQ(signed_in.account.id.decimal(), "578437695752307201");
  const auto bytes = signed_in.account.id.bytes();
  for (std::size_t index = 0; index < bytes.size(); ++index)
    EXPECT_EQ(bytes[index], static_cast<std::byte>(index + 1)) << index;
  EXPECT_EQ(AccountId::from_bytes(bytes), signed_in.account.id);

  EXPECT_TRUE(rig.http.unmatched.empty());
  ASSERT_EQ(rig.http.seen.size(), 4U);
  for (const auto& seen : rig.http.seen)
    EXPECT_TRUE(SignInRig::within_limits(seen));

  EXPECT_EQ(rig.http.seen[0].method, "GET");
  EXPECT_EQ(rig.http.seen[0].url, SignInRig::remote_authorize_url);
  EXPECT_TRUE(rig.http.seen[0].body.empty());
  EXPECT_EQ(SignInRig::header_names(rig.http.seen[0]),
            (std::vector<std::string>{"User-Agent", "Cookie"}));
  EXPECT_EQ(rig.http.seen[0].header("User-Agent"), SignInRig::browser_agent);
  EXPECT_EQ(rig.http.seen[0].header("Cookie"), "npsso=synthetic-npsso");

  EXPECT_EQ(rig.http.seen[1].method, "POST");
  EXPECT_EQ(rig.http.seen[1].url, SignInRig::token_url);
  EXPECT_EQ(rig.http.seen[1].body, remote_exchange_body);
  EXPECT_EQ(SignInRig::header_names(rig.http.seen[1]),
            (std::vector<std::string>{"Content-Type"}));
  EXPECT_EQ(rig.http.seen[1].header("Content-Type"),
            SignInRig::form_content_type);

  EXPECT_EQ(rig.http.seen[2].method, "GET");
  EXPECT_EQ(rig.http.seen[2].url, SignInRig::account_url);
  EXPECT_EQ(SignInRig::header_names(rig.http.seen[2]),
            (std::vector<std::string>{"Authorization", "Content-Type"}));
  EXPECT_EQ(rig.http.seen[2].header("Authorization"), SignInRig::remote_basic);
  EXPECT_EQ(rig.http.seen[2].header("Content-Type"), "application/json");

  EXPECT_EQ(rig.http.seen[3].method, "POST");
  EXPECT_EQ(rig.http.seen[3].url, SignInRig::token_url);
  EXPECT_EQ(rig.http.seen[3].body, SignInRig::app_sso_body);
  EXPECT_EQ(SignInRig::header_names(rig.http.seen[3]),
            (std::vector<std::string>{"Content-Type", "Authorization"}));
  EXPECT_EQ(rig.http.seen[3].header("Authorization"), SignInRig::app_basic);

  EXPECT_FALSE(rig.log.messages.empty());
  for (const std::string_view secret :
       {SignInRig::npsso_value, std::string_view{"remote-access"},
        std::string_view{"remote-refresh"}, std::string_view{"mobile-access"},
        std::string_view{"mobile-refresh"}, std::string_view{"remote%2Bcode"},
        std::string_view{"remote+code"}, std::string_view{"mobile-code"}})
    EXPECT_FALSE(rig.log.contains(secret)) << secret;
  EXPECT_TRUE(
      rig.log.contains("psn.auth.sign_in.account GET "
                       "auth.api.sonyentertainmentnetwork.com/2.0/oauth/token/"
                       "***"));
}

TEST_F(SignIn, RefreshesBothAudiences) {
  rig.http.on(
      "POST", std::string{SignInRig::legacy_token_url},
      {SignInRig::json(
          R"({"access_token":"new-remote","refresh_token":"next-remote","expires_in":1000,"token_type":"Bearer"})")});
  rig.http.on(
      "POST", std::string{SignInRig::token_url},
      {SignInRig::json(
          R"({"access_token":"new-mobile","refresh_token":"next-mobile","expires_in":2000,"token_type":"bearer"})")});
  auto remote = rig.sign_in.refresh(RemotePlayRefreshToken{"old-remote"});
  auto app = rig.sign_in.refresh(AppRefreshToken{"old-mobile"});
  ASSERT_TRUE(remote);
  ASSERT_TRUE(app);
  EXPECT_EQ(remote.value().access.view(), "new-remote");
  EXPECT_EQ(remote.value().refresh.view(), "next-remote");
  EXPECT_EQ(remote.value().expires_in, std::chrono::seconds{1000});
  EXPECT_EQ(app.value().access.view(), "new-mobile");
  EXPECT_EQ(app.value().refresh.view(), "next-mobile");
  EXPECT_EQ(app.value().expires_in, std::chrono::seconds{2000});
  ASSERT_EQ(rig.http.seen.size(), 2U);
  EXPECT_EQ(rig.http.seen[0].url, SignInRig::legacy_token_url);
  EXPECT_EQ(rig.http.seen[0].method, "POST");
  EXPECT_EQ(rig.http.seen[0].body, remote_refresh_body);
  EXPECT_EQ(SignInRig::header_names(rig.http.seen[0]),
            (std::vector<std::string>{"Content-Type", "Authorization"}));
  EXPECT_EQ(rig.http.seen[0].header("Content-Type"),
            SignInRig::form_content_type);
  EXPECT_EQ(rig.http.seen[0].header("Authorization"), SignInRig::remote_basic);
  EXPECT_EQ(rig.http.seen[1].url, SignInRig::token_url);
  EXPECT_EQ(rig.http.seen[1].method, "POST");
  EXPECT_EQ(rig.http.seen[1].body, app_refresh_body);
  EXPECT_EQ(rig.http.seen[1].header("Authorization"), SignInRig::app_basic);
  for (const auto& seen : rig.http.seen)
    EXPECT_TRUE(SignInRig::within_limits(seen));
}

TEST_F(SignIn, ExchangesAnNpssoForAppTokens) {
  rig.http.on("POST", std::string{SignInRig::token_url},
              {SignInRig::json(SignInRig::app_tokens_json)});
  EXPECT_TRUE(rig.sign_in.app_tokens(SignInRig::npsso(), SignInRig::duid));
  ASSERT_EQ(rig.http.seen.size(), 1U);
  EXPECT_EQ(rig.http.seen[0].body, SignInRig::app_sso_body);
}

TEST_F(SignIn, AcceptsAppTokensWithoutARefreshToken) {
  rig.http.on(
      "POST", std::string{SignInRig::token_url},
      {SignInRig::json(
          R"({"access_token":"mobile-access","token_type":"bearer","expires_in":3599})")});
  const auto issued =
      rig.sign_in.app_tokens(SignInRig::npsso(), SignInRig::duid);
  ASSERT_TRUE(issued);
  EXPECT_EQ(issued.value().access.view(), "mobile-access");
  EXPECT_TRUE(issued.value().refresh.empty());
  EXPECT_EQ(issued.value().expires_in, std::chrono::seconds{3599});
  EXPECT_FALSE(issued.value().refresh_expires_in);
}

TEST_F(SignIn, IsCancelledBeforeSendingByAFiredStop) {
  rig.stop();
  EXPECT_EQ(SignInRig::kind_of(rig.sign_in.sign_in(
                SignInRig::npsso(), SignInRig::duid, SignInRig::correlation)),
            SignInErrorKind::cancelled);
  EXPECT_EQ(SignInRig::kind_of(rig.sign_in.sign_in(
                Npsso{"bad;token"}, SignInRig::duid, SignInRig::correlation)),
            SignInErrorKind::cancelled);
  EXPECT_EQ(
      SignInRig::kind_of(rig.sign_in.refresh(AppRefreshToken{"old-mobile"})),
      SignInErrorKind::cancelled);
  EXPECT_EQ(
      SignInRig::kind_of(rig.sign_in.account(BearerToken{"remote-access"})),
      SignInErrorKind::cancelled);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(SignIn, RefusesAnUnsafeNpssoBeforeSending) {
  for (const std::string_view bad :
       {"bad;token", "bad,token", "bad\"token", "bad\\token", "bad token",
        "bad\ntoken", ""})
    EXPECT_EQ(SignInRig::kind_of(rig.sign_in.sign_in(
                  Npsso{bad}, SignInRig::duid, SignInRig::correlation)),
              SignInErrorKind::invalid_request)
        << bad;
  EXPECT_EQ(SignInRig::kind_of(
                rig.sign_in.sign_in(Npsso{std::string(4097, 'a')},
                                    SignInRig::duid, SignInRig::correlation)),
            SignInErrorKind::invalid_request);
  EXPECT_EQ(SignInRig::kind_of(
                rig.sign_in.app_tokens(Npsso{"bad;token"}, SignInRig::duid)),
            SignInErrorKind::invalid_request);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(SignIn, RefusesABadDuidBeforeSending) {
  EXPECT_EQ(SignInRig::kind_of(
                rig.sign_in.app_tokens(SignInRig::npsso(), "bad-duid")),
            SignInErrorKind::invalid_request);
  for (const std::string_view bad :
       {"", "0000000700410080",
        "00000007004100801234567890123456789012345678901g",
        "0000000700410080123456789012345678901234567890123",
        "0000000700410080ABCDEF78901234567890123456789012",
        "111111111111111112345678901234567890123456789012"})
    EXPECT_EQ(SignInRig::kind_of(rig.sign_in.sign_in(SignInRig::npsso(), bad,
                                                     SignInRig::correlation)),
              SignInErrorKind::invalid_request)
        << bad;
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(SignIn, RefusesABadCorrelationIdBeforeSending) {
  for (const std::string_view bad : {"", "12345678123412341234123456789012",
                                     "12345678-1234-1234-1234-12345678901g",
                                     "12345678-1234-1234-1234-1234567890123",
                                     "12345678_1234-1234-1234-123456789012"})
    EXPECT_EQ(SignInRig::kind_of(rig.sign_in.sign_in(SignInRig::npsso(),
                                                     SignInRig::duid, bad)),
              SignInErrorKind::invalid_request)
        << bad;
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(SignIn, RefusesAnUnsafeRefreshOrBearerTokenBeforeSending) {
  EXPECT_EQ(
      SignInRig::kind_of(rig.sign_in.refresh(AppRefreshToken{"bad;token"})),
      SignInErrorKind::invalid_request);
  EXPECT_EQ(SignInRig::kind_of(rig.sign_in.refresh(RemotePlayRefreshToken{""})),
            SignInErrorKind::invalid_request);
  EXPECT_EQ(SignInRig::kind_of(rig.sign_in.refresh(
                RemotePlayRefreshToken{std::string(8193, 'a')})),
            SignInErrorKind::invalid_request);
  EXPECT_EQ(SignInRig::kind_of(rig.sign_in.account(BearerToken{"bad token"})),
            SignInErrorKind::invalid_request);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST(PsnCredential, NamesWhyItsSecretIsRefused) {
  using Refusal = Npsso::Refusal;
  EXPECT_FALSE(Npsso::refusal_of("abc"));
  EXPECT_EQ(Npsso::refusal_of(""), Refusal::empty);
  EXPECT_EQ(Npsso::refusal_of(std::string(4097, 'n')), Refusal::too_long);
  EXPECT_FALSE(Npsso::refusal_of(std::string(4096, 'n')));
  for (const auto* bad : {"a b", "a\"b", "a,b", "a;b", "a\\b", "a\x7F"})
    EXPECT_EQ(Npsso::refusal_of(bad), Refusal::unsendable) << bad;
  EXPECT_EQ(Npsso{std::string_view{"a b"}}.refusal(), Refusal::unsendable);
  EXPECT_FALSE(Npsso{std::string_view{"ab"}}.refusal());
}

TEST(PsnCredential, IsAdmittedOnlyWithAnAcceptableSecret) {
  const auto admitted =
      BearerToken::admit(libchiaking::SecretString{"token"}, "admitted");
  ASSERT_TRUE(admitted);
  EXPECT_EQ(admitted.value().view(), "token");
  EXPECT_EQ(admitted.value().label(), "admitted");
  const auto refused = BearerToken::admit(libchiaking::SecretString{"t\r\n"});
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), BearerToken::Refusal::unsendable);
  EXPECT_EQ(BearerToken::admit(libchiaking::SecretString{}).error(),
            BearerToken::Refusal::empty);
}

}  // namespace
}  // namespace psn_test
