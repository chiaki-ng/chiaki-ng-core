#include "tests/unit/core/utils/diagnostics/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace diagnostics_test {
namespace {

using libchiaking::diagnostics::ApiCapture;

class ApiCaptureTest : public ::testing::Test {
 protected:
  static auto secret_request() -> libchiaking::platform::HttpRequest {
    libchiaking::platform::HttpRequest request;
    request.method = libchiaking::platform::HttpMethod::post;
    request.url = libchiaking::SecretString{
        "https://ca.account.sony.com/api/authz/v3/oauth/"
        "authorize?client_id=public&code=SECRET_CODE&state=kept"};
    request.headers.push_back(
        {"Authorization", libchiaking::SecretString{"Bearer SECRET_BEARER"}});
    request.headers.push_back(
        {"cookie", libchiaking::SecretString{"npsso=SECRET_COOKIE"}});
    request.headers.push_back(
        {"X-Gaikai-SessionKey", libchiaking::SecretString{"SECRET_GK"}});
    request.headers.push_back(
        {"Accept", libchiaking::SecretString{"application/json"}});
    request.body = libchiaking::SecretBytes{std::string_view{
        R"({"access_token":"SECRET_AT","npsso":"SECRET_NP","name":"café kept","nested":{"refresh_token":"SECRET_RT","clientSecret":"SECRET_CS","count":3},"list":["kept"]})"}};
    return request;
  }

  static auto secret_response() -> libchiaking::platform::HttpResponse {
    libchiaking::platform::HttpResponse response;
    response.status_code = 302;
    response.reason = "caf\xc3\xa9 \xff";
    response.final_url = libchiaking::SecretString{
        "https://psnow.playstation.com/?npsso=SECRET_URL_NPSSO"};
    response.headers.push_back(
        {"Set-Cookie", libchiaking::SecretString{"JSESSIONID=SECRET_SESSION"}});
    response.headers.push_back(
        {"Location", libchiaking::SecretString{
                         "https://psnow.playstation.com/"
                         "#access_token=SECRET_FRAGMENT&expires_in=3600"}});
    response.body = libchiaking::SecretBytes{std::string_view{
        "code=SECRET_FORM&client_id=kept&token=SECRET_FORM_TOKEN"}};
    return response;
  }

  void capture_one_call(ApiCapture& into) {
    const auto request = secret_request();
    const auto response = secret_response();
    const auto id = into.begin(request, 3);
    into.finish(3, id, response.clone(), std::chrono::microseconds{1});
    outgoing = Captures::drain(into);
    incoming = Captures::drain(into);
  }

  static auto request_to_capture() -> libchiaking::platform::HttpRequest {
    libchiaking::platform::HttpRequest request;
    request.url = libchiaking::SecretString{
        "https://web.np.playstation.com/api/sessionManager/v1/"
        "remotePlaySessions/SECRET/sessionMessage"};
    request.label = "psn.session_manager.send_session_message";
    return request;
  }

  ApiCapture capture;
  std::string outgoing;
  std::string incoming;
};

TEST(ApiCaptureParameters, MatchSensitiveNamesInAnyCase) {
  EXPECT_EQ(
      ApiCapture::redact_url("https://h/p?CODE=s1&Access_Token=s2&a=k").view(),
      "https://h/p?CODE=[redacted]&Access_Token=[redacted]&a=k");
}

TEST(ApiCaptureParameters, LeaveEncodedNamesAsTheyAre) {
  EXPECT_EQ(ApiCapture::redact_url("https://h/p?c%6Fde=s&npsso=n").view(),
            "https://h/p?c%6Fde=s&npsso=[redacted]");
}

TEST(ApiCaptureParameters, SplitAtEveryQuestionMarkHashAndAmpersand) {
  EXPECT_EQ(ApiCapture::redact_url("https://h/#state=1?code=s#token=t").view(),
            "https://h/#state=1?code=[redacted]#token=[redacted]");
  EXPECT_EQ(ApiCapture::redact_params("a=1&&code=s&=x&code&password==p").view(),
            "a=1&&code=[redacted]&=x&code&password=[redacted]");
  EXPECT_EQ(ApiCapture::redact_url("https://h/code=s").view(),
            "https://h/code=s");
}

TEST_F(ApiCaptureTest, RedactsCredentialsByDefault) {
  capture.enable(true);
  capture_one_call(capture);
  for (const auto& text : {outgoing, incoming}) {
    EXPECT_EQ(text.find("SECRET"), std::string::npos) << text;
    EXPECT_NE(text.find("[redacted]"), std::string::npos) << text;
  }
}

TEST_F(ApiCaptureTest, KeepsWhatIsNotACredentialInTheRequest) {
  capture.enable(true);
  capture_one_call(capture);
  EXPECT_NE(outgoing.find("client_id=public"), std::string::npos);
  EXPECT_NE(outgoing.find("state=kept"), std::string::npos);
  EXPECT_NE(outgoing.find("application/json"), std::string::npos);
  EXPECT_NE(outgoing.find(Captures::base64(
                "{\"access_token\":\"[redacted]\",\"npsso\":\"[redacted]\","
                "\"name\":\"caf\xc3\xa9 kept\",\"nested\":{\"refresh_token\":"
                "\"[redacted]\",\"clientSecret\":\"[redacted]\",\"count\":3},"
                "\"list\":[\"kept\"]}")),
            std::string::npos)
      << outgoing;
}

TEST_F(ApiCaptureTest, KeepsWhatIsNotACredentialInTheResponse) {
  capture.enable(true);
  capture_one_call(capture);
  EXPECT_NE(incoming.find("expires_in=3600"), std::string::npos);
  EXPECT_NE(incoming.find(Captures::base64(
                "code=[redacted]&client_id=kept&token=[redacted]")),
            std::string::npos)
      << incoming;
  EXPECT_NE(incoming.find("caf\xc3\xa9 \xc3\xbf"), std::string::npos);
}

TEST_F(ApiCaptureTest, RedactsJsonItCannotParseWhole) {
  EXPECT_EQ(ApiCapture::redact_json("{\"token\":").view(),
            ApiCapture::redacted);
}

TEST_F(ApiCaptureTest, RedactsOnlyTextValuesUnderSecretKeys) {
  EXPECT_EQ(ApiCapture::redact_json(
                R"({"code":7,"key":["k"],"secret":{"inner":"v"}})")
                .view(),
            R"({"code":7,"key":["k"],"secret":{"inner":"v"}})");
}

TEST_F(ApiCaptureTest, KeepsCredentialsWhenRawCaptureIsAllowed) {
  capture.enable(true);
  capture.allow_raw(true);
  capture_one_call(capture);
  EXPECT_NE(outgoing.find("SECRET_BEARER"), std::string::npos);
  EXPECT_NE(outgoing.find("SECRET_CODE"), std::string::npos);
  EXPECT_NE(incoming.find("SECRET_SESSION"), std::string::npos);
}

TEST_F(ApiCaptureTest, CapturesNothingUntilEnabled) {
  capture.allow_raw(true);
  EXPECT_EQ(capture.begin(secret_request(), 3), 0U);
  EXPECT_EQ(capture.begin(request_to_capture(), 9), 0U);
  EXPECT_EQ(capture.read({}), 0U);
}

TEST_F(ApiCaptureTest, FinishesACallBegunBeforeItWasDisabled) {
  capture.enable(true);
  const auto request = request_to_capture();
  const auto failed_call = capture.begin(request, 9);
  capture.enable(false);
  capture.finish(
      9, failed_call,
      libchiaking::unexpected(libchiaking::platform::HttpError::timed_out),
      std::chrono::microseconds{123});
  static_cast<void>(Captures::take(capture));
  const auto failed_response = Captures::take(capture);
  EXPECT_NE(failed_response.find("\"transport_error\":"), std::string::npos);
  EXPECT_NE(failed_response.find("\"duration_us\":123"), std::string::npos);
  EXPECT_EQ(capture.begin(request, 9), 0U);
}

TEST_F(ApiCaptureTest, LeavesOutABodyOverTheLimit) {
  capture.enable(true);
  auto request = request_to_capture();
  request.body.assign(std::vector<std::byte>(ApiCapture::body_limit + 1));
  static_cast<void>(capture.begin(request, 9));
  const auto oversized = Captures::take(capture);
  EXPECT_NE(oversized.find("exceeds 16 MiB capture limit"), std::string::npos);
  EXPECT_EQ(oversized.find("body_base64"), std::string::npos);
}

TEST_F(ApiCaptureTest, CountsRecordsDroppedWhileFull) {
  capture.enable(true);
  const auto request = request_to_capture();
  for (unsigned i = 0; i < 40; ++i)
    static_cast<void>(capture.begin(request, 9));
  for (unsigned i = 0; i < 32; ++i) static_cast<void>(Captures::take(capture));
  EXPECT_NE(Captures::take(capture).find("\"dropped\":8"), std::string::npos);
  EXPECT_EQ(capture.read({}), 0U);
}

TEST_F(ApiCaptureTest, HandsOverARecordAsWipingText) {
  capture.enable(true);
  static_cast<void>(capture.begin(request_to_capture(), 9));
  const auto taken = capture.take();
  ASSERT_TRUE(taken);
  EXPECT_NE(taken->view().find("\"attempt\":9,"), std::string_view::npos);
  EXPECT_NE(taken->view().find("\"phase\":\"request\""),
            std::string_view::npos);
  EXPECT_FALSE(capture.take());
}

TEST_F(ApiCaptureTest, HandsOverACaptureLossRecordAfterTheLastKept) {
  capture.enable(true);
  const auto request = request_to_capture();
  for (unsigned i = 0; i < 33; ++i)
    static_cast<void>(capture.begin(request, 9));
  for (unsigned i = 0; i < 32; ++i) EXPECT_TRUE(capture.take());
  const auto loss = capture.take();
  ASSERT_TRUE(loss);
  EXPECT_NE(loss->view().find("\"phase\":\"capture_loss\",\"dropped\":1}"),
            std::string_view::npos)
      << loss->view();
  EXPECT_FALSE(capture.take());
  EXPECT_EQ(capture.read({}), 0U);
}

}  // namespace
}  // namespace diagnostics_test
