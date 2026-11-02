#include "src/core/psn/auth/qr_sign_in.hpp"
#include "tests/unit/core/psn/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace psn_test {
namespace {

using libchiaking::psn::auth::ApplicationToken;
using libchiaking::psn::auth::SignInError;
using libchiaking::psn::auth::SignInErrorKind;
using libchiaking::psn::auth::TicketApproved;
using libchiaking::psn::auth::TicketDeclined;
using libchiaking::psn::auth::TicketHandle;
using libchiaking::psn::auth::TicketLapsed;
using libchiaking::psn::auth::TicketNeedsVerification;
using libchiaking::psn::auth::TicketPending;
using libchiaking::psn::auth::TicketSlowDown;
using Qr = libchiaking::psn::auth::QrSignIn;
using Reply = ClientRig::Reply;

class QrSignInFixture : public ::testing::Test {
 protected:
  static constexpr std::string_view page_url =
      "https://my.account.sony.com/sonyacct/signin/";
  static constexpr std::string_view bundle_url =
      "https://my.account.sony.com/sonyacct/signin/dsb_signin_1/assets/"
      "app.bundle.js";
  static constexpr std::string_view token_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/token";
  static constexpr std::string_view ticket_url =
      "https://ca.account.sony.com/api/authn/v3/sso/ticket/webSignin";
  static constexpr std::string_view poll_url =
      "https://ca.account.sony.com/api/authn/v3/sso";
  static constexpr std::string_view page =
      R"(<html><head><script src="vendor.js"></script>)"
      R"(<script defer="defer" src="dsb_signin_1/assets/app.bundle.js"></script>)"
      R"(</head></html>)";
  static constexpr std::string_view encoded_id =
      "PT1BWnAxQ2R1Vldhc05XTGpsR2RsaEdkdWwzYw==";
  static constexpr std::string_view encoded_secret =
      "PVFYWnlOV1p6MUNkdVZXYXNOV0xqbEdkbGhHZHVsM2M=";

  static auto bundle(
      std::string_view grant = "client_credentials",
      std::string_view scopes =
          R"(["openid","oauth:create_authn_ticket_for_web_signin"])")
      -> std::string {
    return std::string{"var a={x:1};var cfg={passwordFreeSignin:{clientId:\""} +
           std::string{encoded_id} + "\",clientSecret:\"" +
           std::string{encoded_secret} + "\",grantType:\"" +
           std::string{grant} + "\",scopes:" + std::string{scopes} +
           "},other:{y:2}};";
  }

  static auto ticket_reply(std::string_view code = "012345") -> Reply {
    Reply reply{
        201,
        std::string{
            R"({"authentication_ticket":"synthetic-handle","user_code":")"} +
            std::string{code} + R"(","interval":5,"expires_in":300})"};
    reply.headers.push_back({"X-Psn-Correlation-Id", "synthetic-correlation"});
    return reply;
  }

  static auto registration() -> libchiaking::psn::auth::HostedRegistration {
    return libchiaking::psn::auth::HostedRegistration{
        "synthetic-client-id",
        libchiaking::psn::auth::ClientSecret{"synthetic-client-secret"},
        "openid oauth:create_authn_ticket_for_web_signin"};
  }

  template <class Value>
  static auto kind_of(const libchiaking::Expected<Value, SignInError>& result)
      -> std::optional<SignInErrorKind> {
    if (result) return std::nullopt;
    return result.error().kind();
  }

  auto layout_of(std::string_view html, const std::string& script)
      -> std::optional<SignInErrorKind> {
    rig.http.rules.clear();
    rig.http.on("GET", std::string{page_url}, {Reply{200, std::string{html}}});
    rig.http.on("GET", std::string{bundle_url}, {Reply{200, script}});
    Qr fresh{rig.http, clock, {}};
    return kind_of(fresh.registration());
  }

  auto ticket_refusal(Reply reply) -> std::optional<SignInErrorKind> {
    rig.http.rules.clear();
    rig.http.on("POST", std::string{ticket_url}, {std::move(reply)});
    Qr fresh{rig.http, clock, {}};
    return kind_of(fresh.ticket(app_token, "my device"));
  }

  test_support::FakeClock clock;
  ClientRig rig;
  Qr qr{rig.http, clock, rig.stop_token, &rig.log};
  const ApplicationToken app_token{"synthetic-app-token"};
};

class QrRegistration : public QrSignInFixture {};
class QrApplicationToken : public QrSignInFixture {};
class QrTicket : public QrSignInFixture {};
class QrPoll : public QrSignInFixture {};
class QrSignIn : public QrSignInFixture {};

TEST(QrSignInErrors, KeepTheirWireNumbering) {
  static_assert(static_cast<int>(SignInErrorKind::malformed) == 6);
  static_assert(static_cast<int>(SignInErrorKind::declined) == 7);
  static_assert(static_cast<int>(SignInErrorKind::verification_required) == 8);
  static_assert(static_cast<int>(SignInErrorKind::expired) == 9);
  static_assert(static_cast<int>(SignInErrorKind::layout_changed) == 10);
  static_assert(SignInError{SignInErrorKind::layout_changed}.name() ==
                "layout_changed");
  EXPECT_EQ(SignInError{SignInErrorKind::layout_changed}.name(),
            "layout_changed");
}

TEST_F(QrRegistration, IsReadFromTheSignInBundle) {
  rig.http.on("GET", std::string{page_url}, {Reply{200, std::string{page}}});
  rig.http.on("GET", std::string{bundle_url}, {Reply{200, bundle()}});
  const auto read = qr.registration();
  ASSERT_TRUE(read);
  EXPECT_EQ(read.value().client_id, "synthetic-client-id");
  EXPECT_EQ(read.value().client_secret.view(), "synthetic-client-secret");
  EXPECT_EQ(read.value().scope,
            "openid oauth:create_authn_ticket_for_web_signin");
  ASSERT_EQ(rig.http.seen.size(), 2U);
  EXPECT_EQ(rig.http.seen[1].maximum_response_bytes, 4U * 1024U * 1024U);
  EXPECT_FALSE(rig.log.contains("synthetic-client-secret"));
}

TEST_F(QrRegistration, FollowsARootedBundlePath) {
  rig.http.on(
      "GET", std::string{page_url},
      {Reply{
          200,
          R"(<script src="/sonyacct/signin/dsb_signin_1/assets/app.bundle.js"></script>)"}});
  rig.http.on("GET", std::string{bundle_url}, {Reply{200, bundle()}});
  EXPECT_TRUE(Qr(rig.http, clock, {}).registration());
}

TEST_F(QrRegistration, RefusesAPageThatMovedTheBundle) {
  EXPECT_EQ(layout_of("<html></html>", bundle()),
            SignInErrorKind::layout_changed);
  EXPECT_EQ(
      layout_of(
          R"(<script src="https://example.com/assets/app.bundle.js"></script>)",
          bundle()),
      SignInErrorKind::layout_changed);
  EXPECT_EQ(
      layout_of(
          R"(<script src="//my.account.sony.com.example.com/assets/app.bundle.js"></script>)",
          bundle()),
      SignInErrorKind::layout_changed);
  EXPECT_EQ(
      layout_of(R"(<script src="../../evil/assets/app.bundle.js"></script>)",
                bundle()),
      SignInErrorKind::layout_changed);
  EXPECT_EQ(
      layout_of(
          R"(<script src="a/assets/app.bundle.js"></script><script src="b/assets/app.bundle.js"></script>)",
          bundle()),
      SignInErrorKind::layout_changed);
}

TEST_F(QrRegistration, RefusesABundleThatChangedItsConfiguration) {
  EXPECT_EQ(layout_of(page, "var nothing={};"),
            SignInErrorKind::layout_changed);
  EXPECT_EQ(layout_of(page, bundle("authorization_code")),
            SignInErrorKind::layout_changed);
  EXPECT_EQ(layout_of(page, bundle("client_credentials", R"(["openid"])")),
            SignInErrorKind::layout_changed);
  EXPECT_EQ(layout_of(page, bundle() + bundle()),
            SignInErrorKind::layout_changed);
  EXPECT_EQ(layout_of(page, "passwordFreeSignin:{nested:{a:1},clientId:\"x\"}"),
            SignInErrorKind::layout_changed);
  EXPECT_EQ(layout_of(page,
                      "passwordFreeSignin:{clientId:\"!!\",clientSecret:\"!!\","
                      "grantType:\"client_credentials\",scopes:[\"oauth:create_"
                      "authn_ticket_for_web_signin\"]}"),
            SignInErrorKind::layout_changed);
}

TEST_F(QrRegistration, IsUnavailableWhenThePageFails) {
  rig.http.on("GET", std::string{page_url}, {Reply{503, ""}});
  Qr fresh{rig.http, clock, {}};
  EXPECT_EQ(kind_of(fresh.registration()), SignInErrorKind::unavailable);
}

TEST_F(QrApplicationToken, IsObtainedWithTheClientCredentials) {
  rig.http.on(
      "POST", std::string{token_url},
      {Reply{
           200,
           R"({"access_token":"synthetic-app-token","token_type":"bearer","expires_in":3599})"},
       Reply{429, ""}, Reply{200, R"({"token_type":"bearer"})"}});
  const auto token = qr.application_token(registration());
  ASSERT_TRUE(token);
  EXPECT_EQ(token.value().view(), "synthetic-app-token");
  EXPECT_EQ(rig.http.seen.back().body,
            "grant_type=client_credentials&client_id=synthetic-client-id"
            "&client_secret=synthetic-client-secret"
            "&scope=openid+oauth%3Acreate_authn_ticket_for_web_signin");
  EXPECT_EQ(kind_of(qr.application_token(registration())),
            SignInErrorKind::rate_limited);
  EXPECT_EQ(kind_of(qr.application_token(registration())),
            SignInErrorKind::malformed);
  EXPECT_FALSE(rig.log.contains("synthetic-client-secret"));
  EXPECT_FALSE(rig.log.contains("synthetic-app-token"));
}

TEST_F(QrTicket, OpensWithASixDigitCode) {
  rig.http.on("POST", std::string{ticket_url}, {ticket_reply()});
  const auto ticket = qr.ticket(app_token, "my device");
  ASSERT_TRUE(ticket);
  EXPECT_EQ(ticket.value().handle.view(), "synthetic-handle");
  EXPECT_EQ(ticket.value().user_code.view(), "012345");
  EXPECT_EQ(ticket.value().interval, std::chrono::seconds{5});
  EXPECT_EQ(ticket.value().expires_in, std::chrono::seconds{300});
  EXPECT_EQ(ticket.value().correlation_id, "synthetic-correlation");
  EXPECT_EQ(rig.http.seen.back().header("Authorization"),
            "Bearer synthetic-app-token");
  EXPECT_EQ(rig.http.seen.back().body, R"({"device_name":"my device"})");
  EXPECT_EQ(Qr::approval_url(ticket.value()).view(),
            "https://id.sonyentertainmentnetwork.com/id/PSsignin/"
            "?code=012345&cid=synthetic-correlation"
            "&service_entity=urn%3Aservice-entity%3Apsn"
            "&elements_visibility=psportal");
}

TEST_F(QrTicket, RefusesAMalformedOrUnexpectedReply) {
  EXPECT_EQ(ticket_refusal(ticket_reply("12345")), SignInErrorKind::malformed);
  EXPECT_EQ(ticket_refusal(ticket_reply("12a456")), SignInErrorKind::malformed);
  EXPECT_EQ(ticket_refusal(ticket_reply("1234567")),
            SignInErrorKind::malformed);
  Reply anonymous = ticket_reply();
  anonymous.headers.clear();
  EXPECT_EQ(ticket_refusal(anonymous), SignInErrorKind::malformed);
  Reply stalled = ticket_reply();
  stalled.body =
      R"({"authentication_ticket":"h","user_code":"000000","interval":0,"expires_in":300})";
  EXPECT_EQ(ticket_refusal(stalled), SignInErrorKind::malformed);
  EXPECT_EQ(ticket_refusal(Reply{200, "{}"}), SignInErrorKind::unavailable);
}

TEST_F(QrTicket, NeedsAnApplicationToken) {
  EXPECT_EQ(kind_of(qr.ticket(ApplicationToken{""}, "my device")),
            SignInErrorKind::invalid_request);
}

TEST_F(QrTicket, WritesTheDeviceNameAsJson) {
  rig.http.on("POST", std::string{ticket_url}, {ticket_reply()});
  ASSERT_TRUE(qr.ticket(app_token, "Akira \"Living room\" Switch"));
  EXPECT_EQ(rig.http.seen.back().body,
            R"({"device_name":"Akira \"Living room\" Switch"})");
}

TEST_F(QrTicket, RefusesADeviceNameThatIsNotUtf8BeforeSending) {
  EXPECT_EQ(kind_of(qr.ticket(app_token, "\xff")),
            SignInErrorKind::invalid_request);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(QrPoll, ReadsEveryAnswer) {
  Reply slow{429, ""};
  slow.headers.push_back({"Retry-After", "12"});
  rig.http.on("POST", std::string{poll_url},
              {Reply{400, R"({"error_code":5104})"},
               Reply{200, R"({"npsso":"synthetic-npsso"})"}, slow,
               Reply{429, ""}, Reply{400, R"({"error_code":5106})"},
               Reply{400, R"({"error_code":4621})"},
               Reply{401, R"({"error_code":5102})"},
               Reply{400, R"({"error_code":4097})"},
               Reply{400, R"({"error_code":9999})"}, Reply{500, "oops"},
               Reply{200, R"({"npsso":""})"}});
  const TicketHandle handle{"synthetic-handle"};
  const auto answer = [&] { return qr.poll(app_token, handle); };
  const auto pending = answer();
  ASSERT_TRUE(pending);
  EXPECT_TRUE(std::holds_alternative<TicketPending>(pending.value()));
  EXPECT_EQ(
      rig.http.seen.back().body,
      R"({"authentication_type":"authentication_ticket","authentication_ticket":"synthetic-handle"})");
  EXPECT_EQ(rig.http.seen.back().header("Authorization"),
            "Bearer synthetic-app-token");
  const auto approved = answer();
  ASSERT_TRUE(approved);
  ASSERT_TRUE(std::holds_alternative<TicketApproved>(approved.value()));
  EXPECT_EQ(std::get<TicketApproved>(approved.value()).npsso.view(),
            "synthetic-npsso");
  const auto slowed = answer();
  ASSERT_TRUE(slowed);
  ASSERT_TRUE(std::holds_alternative<TicketSlowDown>(slowed.value()));
  EXPECT_EQ(std::get<TicketSlowDown>(slowed.value()).retry_after,
            std::chrono::seconds{12});
  EXPECT_EQ(kind_of(answer()), SignInErrorKind::rate_limited);
  const auto declined = answer();
  ASSERT_TRUE(declined);
  EXPECT_TRUE(std::holds_alternative<TicketDeclined>(declined.value()));
  const auto lapsed = answer();
  ASSERT_TRUE(lapsed);
  EXPECT_TRUE(std::holds_alternative<TicketLapsed>(lapsed.value()));
  const auto lapsed_again = answer();
  ASSERT_TRUE(lapsed_again);
  EXPECT_TRUE(std::holds_alternative<TicketLapsed>(lapsed_again.value()));
  const auto verify = answer();
  ASSERT_TRUE(verify);
  EXPECT_TRUE(std::holds_alternative<TicketNeedsVerification>(verify.value()));
  const auto unknown = answer();
  EXPECT_EQ(kind_of(unknown), SignInErrorKind::unavailable);
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().http_status(), 400);
  EXPECT_TRUE(rig.log.contains("error code 9999"));
  EXPECT_EQ(kind_of(answer()), SignInErrorKind::malformed);
  EXPECT_EQ(kind_of(answer()), SignInErrorKind::malformed);
  for (const auto* secret :
       {"synthetic-app-token", "synthetic-handle", "synthetic-npsso"})
    EXPECT_FALSE(rig.log.contains(secret)) << secret;
}

TEST_F(QrSignIn, StopsBeforeSending) {
  rig.stop();
  EXPECT_EQ(kind_of(qr.registration()), SignInErrorKind::cancelled);
  EXPECT_EQ(kind_of(qr.poll(ApplicationToken{"t"}, TicketHandle{"h"})),
            SignInErrorKind::cancelled);
  EXPECT_TRUE(rig.http.seen.empty());
}

}  // namespace
}  // namespace psn_test
