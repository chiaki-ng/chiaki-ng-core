#include "src/core/psn/gk/launch_spec.hpp"
#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;

class GkLockRequest : public ::testing::Test {
 protected:
  static constexpr std::string_view prefix = "{\"requestGameSpecification\":";

  void SetUp() override {
    const auto preauth = gk::LaunchSpec::cloud_stream(
        "synthetic-entitlement", gk::Language{"en-US"},
        gk::TimeZone::parse("UTC+00:00").value(), 1080);
    ASSERT_TRUE(preauth);
    auto signed_spec = preauth.value().authorized(
        GkRig::codes("synthetic-client-code", "synthetic-server-code"));
    ASSERT_TRUE(signed_spec);
    authorized.emplace(std::move(signed_spec.value()));
  }

  auto send(std::string_view spec, std::string_view session_id,
            std::string_view key)
      -> libchiaking::Expected<gk::LockAttempt, gk::Error> {
    Gk client{http};
    return client.lock(GkRig::session_id(session_id), GkRig::config_key(key),
                       spec);
  }

  auto send(std::string_view session_id, std::string_view key)
      -> libchiaking::Expected<gk::LockAttempt, gk::Error> {
    return send(authorized->view(), session_id, key);
  }

  RecordingHttp http;
  std::optional<libchiaking::SecretString> authorized;
};

TEST_F(GkLockRequest, PostsTheSignedSpecToForceALogout) {
  ASSERT_TRUE(send("synthetic-session", "synthetic-key"));
  const auto& request = http.last;
  EXPECT_EQ(request.label, "gk.lock");
  EXPECT_EQ(request.method, platform::HttpMethod::post);
  EXPECT_EQ(request.url.view(),
            "https://cc.prod.gaikai.com/v1/sessions/synthetic-session/"
            "lock?forceLogout=true");
  ASSERT_EQ(request.headers.size(), std::size_t{5});
  EXPECT_EQ(request.headers[0].name, "Content-Type");
  EXPECT_EQ(request.headers[0].value.view(), "application/json");
  EXPECT_EQ(request.headers[1].name, "Accept");
  EXPECT_EQ(request.headers[1].value.view(), "*/*");
  EXPECT_EQ(request.headers[2].name, "User-Agent");
  EXPECT_EQ(request.headers[2].value.view(),
            "PlayStation Portal/6.0.0-rel.444+6a9cea6f5");
  EXPECT_EQ(request.headers[3].name, "X-Gaikai-SessionId");
  EXPECT_EQ(request.headers[3].value.view(), "synthetic-session");
  EXPECT_EQ(request.headers[4].name, "X-Gaikai-Session");
  EXPECT_EQ(request.headers[4].value.view(), "synthetic-key");
  ASSERT_EQ(request.body.size(), prefix.size() + authorized->size() + 1);
  const auto body = request.body.view();
  EXPECT_EQ(body.substr(0, prefix.size()), prefix);
  EXPECT_EQ(body.substr(prefix.size(), authorized->size()), authorized->view());
  EXPECT_EQ(body.back(), '}');
  EXPECT_EQ(request.connect_timeout, std::chrono::seconds{10});
  EXPECT_EQ(request.total_timeout, std::chrono::seconds{30});
  EXPECT_EQ(request.redirect_policy, platform::HttpRedirectPolicy::deny);
  EXPECT_EQ(request.maximum_response_bytes, gk::Client::body_limit);
  EXPECT_GE(request.maximum_response_bytes, std::size_t{1024 * 1024});
}

TEST_F(GkLockRequest, SendsALargePaddedSpec) {
  std::string padded{authorized->view()};
  padded.pop_back();
  padded += ",\"pad\":\"" + std::string(32 * 1024, 'p') + "\"}";
  EXPECT_TRUE(send(padded, "synthetic-session", "synthetic-key"));
  EXPECT_GT(http.last.body.size(), std::size_t{32 * 1024});
}

TEST_F(GkLockRequest, RefusesAnEmptySessionIdOrOneThatBreaksAPath) {
  EXPECT_TRUE(GkRig::fails_for(send("bad\nid", "synthetic-key"),
                               Reason::invalid_session_id));
  EXPECT_TRUE(
      GkRig::fails_for(send("", "synthetic-key"), Reason::invalid_session_id));
}

TEST_F(GkLockRequest, AcceptsALongSessionId) {
  EXPECT_TRUE(send(std::string(127, 'a'), "synthetic-key"));
  EXPECT_TRUE(send(std::string(128, 'a'), "synthetic-key"));
}

TEST_F(GkLockRequest, EscapesTheSessionIdInThePathOnly) {
  ASSERT_TRUE(send("a/b c\xC3\xA9", "key with space"));
  EXPECT_NE(http.last.url.view().find("/sessions/a%2Fb%20c%C3%A9/"),
            std::string::npos);
  ASSERT_EQ(http.last.headers.size(), std::size_t{5});
  EXPECT_EQ(http.last.headers[3].value.view(), "a/b c\xC3\xA9");
  EXPECT_EQ(http.last.headers[4].value.view(), "key with space");
}

TEST_F(GkLockRequest, RefusesAConfigKeyThatBreaksAHeader) {
  EXPECT_TRUE(GkRig::fails_for(send("synthetic-session", "bad\r\nkey"),
                               Reason::invalid_config_key));
}

TEST_F(GkLockRequest, RefusesASpecThatIsNotAnObject) {
  EXPECT_TRUE(
      GkRig::fails_for(send("bad-json", "synthetic-session", "synthetic-key"),
                       Reason::invalid_spec));
  EXPECT_TRUE(GkRig::fails_for(send("[]", "synthetic-session", "synthetic-key"),
                               Reason::invalid_spec));
}

TEST_F(GkLockRequest, RefusesASpecOverTheBodyLimit) {
  EXPECT_TRUE(GkRig::fails_for(send(std::string(gk::Client::body_limit, 'x'),
                                    "synthetic-session", "synthetic-key"),
                               Reason::body_too_large));
}

class GkLockResponse : public ::testing::Test {
 protected:
  static auto decode(std::string_view body) -> std::optional<gk::LockReply> {
    RecordingHttp http;
    http.reply_body = std::string{body};
    Gk client{http};
    auto attempt = client.lock(GkRig::session_id("synthetic-session"),
                               GkRig::config_key("synthetic-key"), "{}");
    if (!attempt) return std::nullopt;
    return attempt.value().response;
  }
};

TEST_F(GkLockResponse, ReadsTheLockAndItsPollFrequency) {
  const auto acquired = decode(R"({"lockAcquired":true,"pollFrequency":5})");
  ASSERT_TRUE(acquired);
  EXPECT_TRUE(acquired->acquired);
  EXPECT_EQ(acquired->poll_seconds, 5);
  const auto pending = decode(R"({"lockAcquired":false,"pollFrequency":30})");
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->acquired);
  EXPECT_EQ(pending->poll_seconds, 30);
}

TEST_F(GkLockResponse, WaitsTenSecondsWithoutAReadableReply) {
  for (const auto body : {"{}", "{bad json", "[1,2]"}) {
    SCOPED_TRACE(body);
    const auto result = decode(body);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result->acquired);
    EXPECT_EQ(result->poll_seconds, 10);
  }
}

TEST_F(GkLockResponse, ReadsAPollFrequencySentAsText) {
  const auto result = decode(R"({"lockAcquired":true,"pollFrequency":"20"})");
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->acquired);
  EXPECT_EQ(result->poll_seconds, 20);
}

TEST_F(GkLockResponse, RefusesAReplyWithFieldsOfTheWrongType) {
  const auto text = decode(R"({"lockAcquired":"false","pollFrequency":20})");
  ASSERT_TRUE(text);
  EXPECT_FALSE(text->acquired);
  EXPECT_EQ(text->poll_seconds, 10);
  const auto numbers = decode(R"({"lockAcquired":1,"pollFrequency":true})");
  ASSERT_TRUE(numbers);
  EXPECT_FALSE(numbers->acquired);
  EXPECT_EQ(numbers->poll_seconds, 10);
  const auto fraction =
      decode(R"({"lockAcquired":true,"pollFrequency":"2.9"})");
  ASSERT_TRUE(fraction);
  EXPECT_FALSE(fraction->acquired);
  EXPECT_EQ(fraction->poll_seconds, 10);
}

TEST_F(GkLockResponse, ReplacesANonPositivePollFrequency) {
  const auto zero = decode(R"({"lockAcquired":false,"pollFrequency":0})");
  ASSERT_TRUE(zero);
  EXPECT_FALSE(zero->acquired);
  EXPECT_EQ(zero->poll_seconds, 10);
  const auto negative = decode(R"({"lockAcquired":true,"pollFrequency":-3})");
  ASSERT_TRUE(negative);
  EXPECT_TRUE(negative->acquired);
  EXPECT_EQ(negative->poll_seconds, 10);
}

TEST_F(GkLockResponse, KeepsALongPollFrequency) {
  const auto result = decode(R"({"pollFrequency":1200})");
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->acquired);
  EXPECT_EQ(result->poll_seconds, 1200);
}

TEST_F(GkLockResponse, StopsReadingAtARawNul) {
  const std::string with_nul =
      std::string{R"({"lockAcquired":true,"pollFrequency":7})"} + '\0' +
      "garbage";
  const auto whole = decode(with_nul);
  ASSERT_TRUE(whole);
  EXPECT_TRUE(whole->acquired);
  EXPECT_EQ(whole->poll_seconds, 7);
  const std::string cut_json =
      std::string{"{\"lockAcquired\":tru"} + '\0' + "e}";
  const auto cut = decode(cut_json);
  ASSERT_TRUE(cut);
  EXPECT_FALSE(cut->acquired);
  EXPECT_EQ(cut->poll_seconds, 10);
}

class LockProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    EXPECT_EQ(request.method, platform::HttpMethod::post);
    EXPECT_NE(request.url.view().find("/lock?forceLogout=true"),
              std::string::npos);
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response;
    response.status_code = status;
    for (const auto& [name, value] : headers)
      response.headers.push_back({name, libchiaking::SecretString{value}});
    for (const char character : body)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  std::optional<platform::HttpError> error;
  std::uint16_t status{200};
  std::string body{R"({"lockAcquired":true,"pollFrequency":5})"};
  std::vector<std::pair<std::string, std::string>> headers{
      {"X-Gaikai-Session", "new-key"}};
  unsigned calls{};
};

class GkLockAttemptClient : public ::testing::Test {
 protected:
  auto attempt(const platform::StopToken& stop = {})
      -> libchiaking::Expected<gk::LockAttempt, gk::Error> {
    Gk client{provider, stop};
    return client.lock(GkRig::session_id("synthetic-session"),
                       GkRig::config_key("synthetic-key"), "{}");
  }

  LockProvider provider;
};

TEST_F(GkLockAttemptClient, AcquiresTheLockAndRotatesTheKey) {
  const auto result = attempt();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().response.acquired);
  EXPECT_EQ(result.value().response.poll_seconds, 5);
  ASSERT_TRUE(result.value().rotated_key);
  EXPECT_EQ(result.value().rotated_key->view(), "new-key");
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkLockAttemptClient, ReportsAPendingLockWithoutRotating) {
  provider.body = R"({"lockAcquired":false,"pollFrequency":17})";
  provider.headers.clear();
  const auto result = attempt();
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().response.acquired);
  EXPECT_EQ(result.value().response.poll_seconds, 17);
  EXPECT_FALSE(result.value().rotated_key);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkLockAttemptClient, ReportsAnUnreadableReplyAsPending) {
  provider.body = "bad json";
  provider.headers.clear();
  const auto result = attempt();
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().response.acquired);
  EXPECT_EQ(result.value().response.poll_seconds, 10);
  EXPECT_FALSE(result.value().rotated_key);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkLockAttemptClient, RefusesARotatedKeyThatBreaksAHeader) {
  provider.body = "bad json";
  provider.headers = {{"X-Gaikai-Session", "bad\r\nkey"}};
  EXPECT_TRUE(GkRig::fails_for(attempt(), Reason::invalid_session_key));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkLockAttemptClient, IsRejectedByAnErrorStatus) {
  provider.status = 403;
  provider.headers = {{"X-Gaikai-Session", "bad\r\nkey"}};
  EXPECT_TRUE(GkRig::fails_for(attempt(), Reason::rejected));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkLockAttemptClient, ReportsATransportFailure) {
  provider.error = platform::HttpError::backend_failure;
  EXPECT_TRUE(GkRig::fails_for(attempt(), Reason::transport));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkLockAttemptClient, ReportsACancelledRequest) {
  provider.error = platform::HttpError::cancelled;
  const auto result = attempt();
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkLockAttemptClient, SendsNothingOnceStopped) {
  provider.error = platform::HttpError::cancelled;
  const auto result =
      attempt(platform::StopToken{std::make_shared<test_support::Stopped>()});
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 0U);
}

}  // namespace
}  // namespace cloud_test
