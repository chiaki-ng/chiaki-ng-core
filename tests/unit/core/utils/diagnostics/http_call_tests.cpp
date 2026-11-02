#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/unit/core/utils/diagnostics/rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace diagnostics_test {
namespace {

namespace platform = libchiaking::platform;
namespace diagnostics = libchiaking::diagnostics;
using diagnostics::HttpCallLog;
using diagnostics::HttpSecretKind;
using libchiaking::SecretString;
using Secrets = std::vector<std::string>;

class HttpCallMarking : public ::testing::Test {
 protected:
  void SetUp() override {
    auto request = Calls::request_to(
        "https://ca.account.sony.com/api/authz/v3/oauth/"
        "token?code=QUERYSECRET1&client_id=visible#access_token="
        "FRAGMENTSECRET");
    request.headers.push_back(
        {"Authorization", SecretString{"Bearer BEARERSECRET123"}});
    request.headers.push_back(
        {"Cookie", SecretString{"npsso=NPSSOSECRET123; locale=en"}});
    request.headers.push_back(
        {"X-Gaikai-SessionId", SecretString{"GKSESSION1"}});
    request.headers.push_back({"Accept", SecretString{"application/json"}});
    request.body.assign(std::string_view{
        "grant_type=refresh_token&refresh_token=FORMSECRET123&scope=psn"});
    platform::HttpResponse response;
    response.status_code = 200;
    response.headers.push_back(
        {"Set-Cookie", SecretString{"JSESSIONID=SETCOOKIESECRET; Path=/"}});
    response.body.assign(std::string_view{
        R"({"access_token":"JSONSECRET123","expires_in":3600,"nested":{"refreshToken":"NESTEDSECRET1"},"echo":"GKSESSION1"})"});
    Calls::record(log, request, Calls::Outcome{std::move(response)});
    const auto snapshot = log.snapshot();
    ASSERT_FALSE(snapshot.calls.empty());
    call = snapshot.calls.front();
  }

  HttpCallLog log{test_support::process_threading()};
  std::shared_ptr<const diagnostics::HttpCall> call;
};

TEST_F(HttpCallMarking, MarksQueryAndFragmentSecretsInTheUrl) {
  const auto url = call->url();
  EXPECT_EQ(Calls::secrets_in(url),
            (Secrets{"QUERYSECRET1", "FRAGMENTSECRET"}));
  EXPECT_NE(Calls::masked(url).find("client_id=visible"), std::string::npos);
}

TEST(HttpCallParameters, AreSplitLikeTheCaptureSplitsThem) {
  HttpCallLog log{test_support::process_threading()};
  Calls::record(log,
                Calls::request_to("https://h/p?CODE=s1&c%6Fde=s2#a=1?npsso=s3"
                                  "&&=x&token&Password==p"),
                Calls::answered(200));
  const auto snapshot = log.snapshot();
  ASSERT_EQ(snapshot.calls.size(), 1U);
  EXPECT_EQ(Calls::secrets_in(snapshot.calls.front()->url()),
            (Secrets{"s1", "s3", "=p"}));
}

TEST_F(HttpCallMarking, MarksSecretRequestHeadersByKind) {
  const auto sent = call->request_headers();
  EXPECT_EQ(sent.size(), 4U);
  const auto& authorization = Calls::header(sent, "Authorization");
  EXPECT_EQ(Calls::secrets_in(authorization), (Secrets{"BEARERSECRET123"}));
  ASSERT_FALSE(authorization.spans().empty());
  EXPECT_EQ(authorization.spans().front().kind, HttpSecretKind::credential);
  EXPECT_TRUE(Calls::masked(authorization).starts_with("Bearer "));
  const auto& cookie = Calls::header(sent, "Cookie");
  EXPECT_EQ(Calls::secrets_in(cookie), (Secrets{"NPSSOSECRET123", "en"}));
  ASSERT_FALSE(cookie.spans().empty());
  EXPECT_EQ(cookie.spans().front().kind, HttpSecretKind::cookie);
  const auto& session = Calls::header(sent, "X-Gaikai-SessionId");
  EXPECT_EQ(Calls::secrets_in(session), (Secrets{"GKSESSION1"}));
  ASSERT_FALSE(session.spans().empty());
  EXPECT_EQ(session.spans().front().kind, HttpSecretKind::session);
  EXPECT_TRUE(Calls::header(sent, "Accept").spans().empty());
}

TEST_F(HttpCallMarking, MarksSecretFormParameters) {
  const auto form = call->request_body();
  EXPECT_FALSE(form.binary);
  EXPECT_FALSE(form.truncated);
  EXPECT_EQ(Calls::secrets_in(form.text), (Secrets{"FORMSECRET123"}));
  ASSERT_FALSE(form.text.spans().empty());
  EXPECT_EQ(form.text.spans().front().kind, HttpSecretKind::parameter);
}

TEST_F(HttpCallMarking, MarksTheCookieAResponseSets) {
  const auto received = call->response_headers();
  EXPECT_EQ(Calls::secrets_in(Calls::header(received, "Set-Cookie")),
            (Secrets{"SETCOOKIESECRET"}));
}

TEST_F(HttpCallMarking, MarksSecretJsonFieldsInAPrettyBody) {
  const auto json = call->response_body();
  EXPECT_NE(json.text.text().find("\n  \"access_token\": \"JSONSECRET123\""),
            std::string_view::npos);
  EXPECT_EQ(Calls::secrets_in(json.text),
            (Secrets{"JSONSECRET123", "NESTEDSECRET1", "GKSESSION1"}));
  ASSERT_EQ(json.text.spans().size(), 3U);
  EXPECT_EQ(json.text.spans()[0].kind, HttpSecretKind::field);
  EXPECT_EQ(json.text.spans()[2].kind, HttpSecretKind::session);
  const auto shown = Calls::masked(json.text);
  EXPECT_NE(shown.find("3600"), std::string::npos);
  for (const std::string_view secret :
       {"JSONSECRET123", "NESTEDSECRET1", "GKSESSION1"})
    EXPECT_EQ(shown.find(secret), std::string::npos) << secret;
}

TEST(HttpCallBody, MarksAnUnparsedJsonBodyWhole) {
  HttpCallLog broken{test_support::process_threading()};
  Calls::record(broken, Calls::request_to("https://m.np.playstation.com/api"),
                Calls::answered(200, R"({"token":"CUT)"));
  const auto snapshot = broken.snapshot();
  ASSERT_FALSE(snapshot.calls.empty());
  const auto unparsed = snapshot.calls.front()->response_body();
  ASSERT_EQ(unparsed.text.spans().size(), 1U);
  EXPECT_EQ(unparsed.text.spans().front().kind, HttpSecretKind::body);
  EXPECT_EQ(unparsed.text.spans().front().length, unparsed.text.text().size());
}

TEST(HttpCallBody, ShowsNoTextForABinaryBody) {
  HttpCallLog binary{test_support::process_threading()};
  const std::string bytes{"\x89PNG\r\n\x1a\n\0\xff", 10};
  Calls::record(
      binary, Calls::request_to("https://image.api.playstation.com/cover.png"),
      Calls::answered(200, bytes));
  const auto snapshot = binary.snapshot();
  ASSERT_FALSE(snapshot.calls.empty());
  const auto image = snapshot.calls.front()->response_body();
  EXPECT_TRUE(image.binary);
  EXPECT_TRUE(image.text.text().empty());
  EXPECT_EQ(image.size, bytes.size());
}

}  // namespace
}  // namespace diagnostics_test
