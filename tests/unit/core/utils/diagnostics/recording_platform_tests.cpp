#include "libchiaking/detail/diagnostics/http_call_log.hpp"
#include "libchiaking/platform/default_factory.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/sign_in.hpp"
#include "src/core/psn/rest/kamaji/kamaji_client.hpp"
#include "src/core/utils/diagnostics/recording_platform.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/unit/core/utils/diagnostics/rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace diagnostics_test {
namespace {

namespace platform = libchiaking::platform;
namespace diagnostics = libchiaking::diagnostics;
using diagnostics::HttpArea;
using diagnostics::HttpCallLog;
using diagnostics::HttpSecretKind;
using platform::HttpCredential;
using Secrets = std::vector<std::string>;

class RecordingPlatform : public ::testing::Test {
 protected:
  static constexpr std::string_view token_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/token";
  static constexpr std::string_view account_url =
      "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token/"
      "remote-access-1";
  static constexpr std::string_view authorize_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/authorize"
      "?client_id=ba495a24-818c-472b-b12d-ff231c1b5745"
      "&redirect_uri=https%3A%2F%2Fremoteplay.dl.playstation.net%2Fremoteplay%"
      "2Fredirect"
      "&scope=psn%3Aclientapp%20referenceDataService%3AcountryConfig.read"
      "%20pushNotification%3AwebSocket.desktop.connect%20sessionManager%"
      "3AremotePlaySession.system.update"
      "&response_type=code&access_type=offline&service_entity=urn%3Aservice-"
      "entity%3Apsn"
      "&duid=000000070041008012345678901234567890123456789012"
      "&smcid=remoteplay&layout_type=popup&PlatformPrivacyWs1=minimal&no_"
      "captcha="
      "true"
      "&cid=12345678-1234-1234-1234-123456789012";
  static constexpr std::string_view kamaji_api =
      "https://psnow.playstation.com/kamaji/api/pcnow/00_09_000";

  static auto reply(int status, std::string body,
                    std::vector<test_support::psn::TestHeader> headers = {})
      -> test_support::psn::Reply {
    test_support::psn::Reply made{status, std::move(body)};
    made.headers = std::move(headers);
    return made;
  }

  static auto requests_carrying(const diagnostics::HttpCallTotals& totals,
                                HttpCredential kind) -> std::uint64_t {
    const auto found =
        std::ranges::find(totals.credentials, kind,
                          &diagnostics::HttpCredentialTally::credential);
    if (found != totals.credentials.end()) return found->tally.requests;
    ADD_FAILURE() << "no tally for " << testing::PrintToString(kind);
    return 0;
  }

  void scripts_the_sign_in() {
    http.on("GET", std::string{authorize_url},
            {reply(302, "",
                   {{"Location",
                     "https://remoteplay.dl.playstation.net/remoteplay/"
                     "redirect?code=remote-code-1"}})});
    http.on(
        "POST", std::string{token_url},
        {reply(
             200,
             R"({"access_token":"remote-access-1","refresh_token":"remote-refresh-1","expires_in":3600,"token_type":"bearer"})"),
         reply(
             200,
             R"({"access_token":"mobile-access-1","refresh_token":"mobile-refresh-1","expires_in":7200})")});
    http.on(
        "GET", std::string{account_url},
        {reply(
            200,
            R"({"user_id":"578437695752307201","online_id":"Tokyo Player"})")});
  }

  test_support::psn::FakeHttp http;
  platform::DefaultPlatformFactory factory{
      platform::PlatformProviders{.http = &http}};
  HttpCallLog log{test_support::process_threading()};
};

TEST_F(RecordingPlatform, TagsTheSignInPathsWithWhatTheyCarried) {
  scripts_the_sign_in();
  diagnostics::RecordingPlatform users{factory, log, HttpArea::users};
  libchiaking::psn::auth::SignIn sign_in{users.recorded_http(),
                                         Calls::monotonic};
  const auto signed_in =
      sign_in.sign_in(libchiaking::psn::auth::Npsso{"synthetic-npsso-1"},
                      "000000070041008012345678901234567890123456789012",
                      "12345678-1234-1234-1234-123456789012");
  EXPECT_TRUE(signed_in);
  EXPECT_TRUE(http.unmatched.empty());
  const auto snapshot = log.snapshot();
  ASSERT_EQ(snapshot.calls.size(), 4U);
  const auto& remote_authorize = *snapshot.calls[3];
  const auto& remote_exchange = *snapshot.calls[2];
  const auto& account = *snapshot.calls[1];
  const auto& app_exchange = *snapshot.calls[0];
  for (const auto& call : snapshot.calls) {
    EXPECT_EQ(call->area(), HttpArea::users);
    EXPECT_EQ(call->attempt(), 1U);
  }
  EXPECT_EQ(remote_authorize.credentials(),
            Calls::only({HttpCredential::sign_in_cookie}));
  EXPECT_EQ(remote_authorize.credential_label(), "npsso");
  EXPECT_EQ(Calls::secrets_in(
                Calls::header(remote_authorize.request_headers(), "Cookie")),
            (Secrets{"synthetic-npsso-1"}));
  EXPECT_EQ(Calls::secrets_in(
                Calls::header(remote_authorize.response_headers(), "Location")),
            (Secrets{"remote-code-1"}));
  EXPECT_EQ(remote_exchange.credentials(),
            Calls::only({HttpCredential::sign_in_grant}));
  const auto exchanged = remote_exchange.request_body();
  const auto exchanged_secrets = Calls::secrets_in(exchanged.text);
  EXPECT_NE(std::ranges::find(exchanged_secrets, "remote-code-1"),
            exchanged_secrets.end());
  EXPECT_EQ(Calls::masked(remote_exchange.response_body().text)
                .find("remote-access-1"),
            std::string::npos);
  EXPECT_EQ(account.credentials(),
            Calls::only({HttpCredential::account_token,
                         HttpCredential::client_credentials}));
  EXPECT_EQ(Calls::masked(account.url()).find("remote-access-1"),
            std::string::npos);
  EXPECT_TRUE(
      Calls::masked(account.url()).ends_with("/oauth/token/***************"));
  ASSERT_FALSE(account.url().spans().empty());
  EXPECT_EQ(account.url().spans().front().kind, HttpSecretKind::credential);
  EXPECT_EQ(app_exchange.credentials(),
            Calls::only({HttpCredential::sign_in_cookie,
                         HttpCredential::client_credentials}));
  EXPECT_EQ(Calls::secrets_in(
                Calls::header(app_exchange.request_headers(), "Authorization"))
                .size(),
            1U);
  EXPECT_TRUE(Calls::masked(Calls::header(app_exchange.request_headers(),
                                          "Authorization"))
                  .starts_with("Basic "));
  EXPECT_EQ(snapshot.totals.anonymous.requests, 0U);
  EXPECT_EQ(requests_carrying(snapshot.totals, HttpCredential::sign_in_cookie),
            2U);
  EXPECT_EQ(requests_carrying(snapshot.totals, HttpCredential::sign_in_grant),
            1U);
  EXPECT_EQ(
      requests_carrying(snapshot.totals, HttpCredential::client_credentials),
      2U);
  EXPECT_EQ(requests_carrying(snapshot.totals, HttpCredential::account_token),
            1U);
  EXPECT_EQ(snapshot.totals.bases.size(), 2U);
}

TEST_F(RecordingPlatform, TagsTheStoreSessionAndItsSignInCode) {
  http.on(
      "POST", std::string{kamaji_api} + "/user/session",
      {reply(
          200,
          R"({"header":{"status_code":"0x0000"},"data":{"country":"GB","language":"en"}})",
          {{"Set-Cookie", "JSESSIONID=STORESESSION1; Path=/"}})});
  http.on(
      "POST", std::string{kamaji_api} + "/user/checkout/buynow/preview",
      {reply(
          200,
          R"({"header":{"status_code":"0x0000"},"data":{"cart":{"total_price_value":0,"items":[{"sku_id":"SKU-SERVER"}]}}})")});
  diagnostics::RecordingPlatform catalog{factory, log, HttpArea::catalog};
  libchiaking::net::http::Client client{
      catalog.recorded_http(), Calls::monotonic, {}};
  libchiaking::psn::rest::KamajiClient kamaji{client};
  EXPECT_TRUE(kamaji.create_session(
      libchiaking::psn::auth::AuthCode{"LEGACY_CLOUD_STREAMCODE1",
                                       "legacy_cloud_stream code"},
      "0000000700410080000102030405060708090a0b0c0d0e0f"));
  EXPECT_TRUE(kamaji.buynow_preview(
      libchiaking::psn::auth::BearerToken{std::string_view{"BEARERFAKE1"},
                                          "commerce bearer"},
      "SKU-A"));
  const auto snapshot = log.snapshot();
  ASSERT_EQ(snapshot.calls.size(), 2U);
  const auto& session = *snapshot.calls[1];
  const auto& preview = *snapshot.calls[0];
  EXPECT_EQ(session.credentials(),
            platform::HttpCredentials{}.with(HttpCredential::sign_in_grant));
  EXPECT_EQ(session.credential_label(), "legacy_cloud_stream code");
  EXPECT_EQ(Calls::secrets_in(session.request_body().text),
            (Secrets{"LEGACY_CLOUD_STREAMCODE1"}));
  EXPECT_EQ(Calls::secrets_in(
                Calls::header(session.response_headers(), "Set-Cookie")),
            (Secrets{"STORESESSION1"}));
  EXPECT_EQ(preview.credentials(), platform::HttpCredentials{}
                                       .with(HttpCredential::account_token)
                                       .with(HttpCredential::store_session));
  EXPECT_EQ(preview.credential_label(), "commerce bearer");
  EXPECT_EQ(
      Calls::secrets_in(Calls::header(preview.request_headers(), "Cookie")),
      (Secrets{"STORESESSION1"}));
  EXPECT_EQ(preview.base(), "https://psnow.playstation.com");
}

TEST_F(RecordingPlatform, NumbersRetriesAndLeavesAnonymousCallsUntagged) {
  http.on("GET", "https://m.np.playstation.com/api/retried",
          {reply(503, ""), reply(429, ""), reply(200, "{}")});
  diagnostics::RecordingPlatform catalog{factory, log, HttpArea::catalog};
  EXPECT_EQ(&catalog.http(), &catalog.recorded_http());
  EXPECT_EQ(catalog.info().capabilities, factory.info().capabilities);
  ASSERT_TRUE(catalog.supplies(platform::PlatformCapability::http));
  libchiaking::net::http::Client client{catalog.http(), Calls::monotonic, {}};
  constexpr libchiaking::net::http::Endpoint retried{.label = "test.retried",
                                                     .retry = {.retries = 3}};
  const auto sent = client.send(libchiaking::net::http::Request{
      retried, "https://m.np.playstation.com/api/retried"});
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value().attempts(), 3U);
  const auto snapshot = log.snapshot();
  ASSERT_EQ(snapshot.calls.size(), 3U);
  EXPECT_EQ(snapshot.calls[2]->attempt(), 1U);
  EXPECT_EQ(snapshot.calls[2]->status(), 503);
  EXPECT_EQ(snapshot.calls[1]->attempt(), 2U);
  EXPECT_EQ(snapshot.calls[1]->status(), 429);
  EXPECT_EQ(snapshot.calls[0]->attempt(), 3U);
  EXPECT_EQ(snapshot.calls[0]->status(), 200);
  for (const auto& call : snapshot.calls) {
    EXPECT_TRUE(call->credentials().anonymous());
    EXPECT_EQ(call->area(), HttpArea::catalog);
    EXPECT_EQ(call->label(), "test.retried");
  }
  EXPECT_EQ(snapshot.totals.anonymous.requests, 3U);
  EXPECT_EQ(snapshot.totals.all.http_errors, 2U);
}

TEST_F(RecordingPlatform, OffersNoHttpWhenThePlatformHasNone) {
  platform::DefaultPlatformFactory bare;
  diagnostics::RecordingPlatform none{bare, log, HttpArea::other};
  EXPECT_FALSE(none.supplies(platform::PlatformCapability::http));
}

TEST_F(RecordingPlatform, RecordsStartedCallsWhenCollectedAndDropsAsCancelled) {
  http.on("GET", "https://m.np.playstation.com/api/first", {reply(200, "{}")});
  http.on("GET", "https://m.np.playstation.com/api/second", {reply(404, "")});
  diagnostics::RecordingPlatform catalog{factory, log, HttpArea::catalog};
  auto& recorded = catalog.recorded_http();
  auto first = recorded.start(
      Calls::request_to("https://m.np.playstation.com/api/first"), {});
  auto second = recorded.start(
      Calls::request_to("https://m.np.playstation.com/api/second"), {});
  EXPECT_TRUE(log.snapshot().calls.empty());
  const auto later = std::move(second).wait();
  ASSERT_TRUE(later);
  EXPECT_EQ(later.value().status_code, 404);
  const auto sooner = std::move(first).wait();
  ASSERT_TRUE(sooner);
  EXPECT_EQ(sooner.value().status_code, 200);
  {
    auto dropped = recorded.start(
        Calls::request_to("https://m.np.playstation.com/api/dropped"), {});
  }
  const auto snapshot = log.snapshot();
  ASSERT_EQ(snapshot.calls.size(), 3U);
  EXPECT_EQ(snapshot.calls[2]->status(), 404);
  EXPECT_EQ(snapshot.calls[1]->status(), 200);
  EXPECT_EQ(snapshot.calls[0]->transport_error(),
            platform::HttpError::cancelled);
  EXPECT_TRUE(
      Calls::masked(snapshot.calls[0]->url()).ends_with("/api/dropped"));
  for (const auto& call : snapshot.calls)
    EXPECT_EQ(call->area(), HttpArea::catalog);
}

TEST_F(RecordingPlatform, RecordsNothingForAStartThePlatformCannotMake) {
  platform::DefaultPlatformFactory bare;
  diagnostics::RecordingPlatform none{bare, log, HttpArea::other};
  auto unsupported = none.recorded_http().start(
      Calls::request_to("https://m.np.playstation.com/api/none"), {});
  const auto outcome = std::move(unsupported).wait();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error(), platform::HttpError::unsupported);
  EXPECT_TRUE(log.snapshot().calls.empty());
}

}  // namespace
}  // namespace diagnostics_test
