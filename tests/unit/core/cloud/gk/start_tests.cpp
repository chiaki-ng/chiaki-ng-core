#include "src/core/psn/gk/launch_spec.hpp"
#include "tests/support/json/tree.hpp"
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

class GkStartStatus : public ::testing::Test {
 protected:
  static auto reason(std::uint16_t status, std::string_view body)
      -> std::optional<Reason> {
    RecordingHttp http;
    http.status = status;
    http.reply_body = std::string{body};
    Gk client{http};
    const auto result =
        client.start(R"({"spec":true})", GkRig::config_key("synthetic-key"));
    if (result) return std::nullopt;
    return result.error().reason();
  }
};

TEST_F(GkStartStatus, CallsASuccessWithoutASessionMalformed) {
  EXPECT_EQ(reason(200, ""), Reason::malformed);
  EXPECT_EQ(reason(200, "noGameForEntitlement"), Reason::malformed);
  EXPECT_EQ(reason(200, std::string_view{"\0junk", sizeof("\0junk") - 1}),
            Reason::malformed);
}

TEST_F(GkStartStatus, AsksForAnEntitlementRetryWithoutAGame) {
  EXPECT_EQ(reason(401, "noGameForEntitlement"), Reason::entitlement_retry);
  EXPECT_EQ(reason(401, "prefix noGameForEntitlement suffix"),
            Reason::entitlement_retry);
  EXPECT_EQ(
      reason(401, std::string_view{"noGameForEntitlement\0tail",
                                   sizeof("noGameForEntitlement\0tail") - 1}),
      Reason::entitlement_retry);
}

TEST_F(GkStartStatus, IsRejectedByAnyOtherErrorStatus) {
  EXPECT_EQ(reason(500, "Internal Server Error"), Reason::rejected);
  EXPECT_EQ(reason(403, ""), Reason::rejected);
}

TEST_F(GkStartStatus, FindsAMissingGamePastANul) {
  EXPECT_EQ(
      reason(500, std::string_view{"err\0noGameForEntitlement",
                                   sizeof("err\0noGameForEntitlement") - 1}),
      Reason::entitlement_retry);
}

class GkStartResponse : public ::testing::Test {
 protected:
  static auto decode(std::string_view body)
      -> libchiaking::Expected<gk::Started, gk::Error> {
    RecordingHttp http;
    http.reply_body = std::string{body};
    Gk client{http};
    return client.start(R"({"spec":true})", GkRig::config_key("synthetic-key"));
  }

  static auto malformed(std::string_view body) -> testing::AssertionResult {
    return GkRig::fails_for(decode(body), Reason::malformed);
  }
};

TEST_F(GkStartResponse, ReadsTheSessionId) {
  const auto result = decode(R"({"sessionId":"abc123xyz"})");
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().session_id.view(), "abc123xyz");
}

TEST_F(GkStartResponse, UnescapesTheSessionId) {
  const auto result = decode(R"({"sessionId":"abc/def"})");
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().session_id.view(), "abc/def");
}

TEST_F(GkStartResponse, RefusesAMissingEmptyOrNonTextSessionId) {
  EXPECT_TRUE(malformed("{}"));
  EXPECT_TRUE(malformed(R"({"sessionId":""})"));
  EXPECT_TRUE(malformed(R"({"sessionId":123})"));
  EXPECT_TRUE(malformed(R"({"sessionId":null})"));
  EXPECT_TRUE(malformed(R"({"sessionId":[]})"));
}

TEST_F(GkStartResponse, RefusesAReplyThatIsNotAnObject) {
  EXPECT_TRUE(malformed("{"));
  EXPECT_TRUE(malformed(""));
  EXPECT_TRUE(malformed("[]"));
}

TEST_F(GkStartResponse, TakesTheLastOfRepeatedSessionIds) {
  const auto result = decode(R"({"sessionId":"a","sessionId":"b"})");
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().session_id.view(), "b");
}

TEST_F(GkStartResponse, ReadsTheSessionIdBesideLongPadding) {
  const auto result =
      decode(std::string{"{\"sessionId\":\"a\",\"padding\":\""} +
             std::string(16 * 1024, 'x') + "\"}");
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().session_id.view(), "a");
}

class GkStartRequest : public ::testing::Test {
 protected:
  static constexpr std::string_view spec =
      R"({"titleId":"synthetic-title","settings":{}})";
  static constexpr std::size_t prefix_size =
      sizeof("{\"requestGameSpecification\":") - 1;

  auto start(std::string_view sent, const gk::ConfigKey& key)
      -> libchiaking::Expected<gk::Started, gk::Error> {
    http.reply_body = R"({"sessionId":"synthetic-session"})";
    Gk client{http};
    return client.start(sent, key);
  }

  auto start(std::string_view sent)
      -> libchiaking::Expected<gk::Started, gk::Error> {
    return start(sent, config);
  }

  static auto full_spec()
      -> libchiaking::Expected<gk::LaunchSpec, gk::LaunchSpec::Invalid> {
    return gk::LaunchSpec::cloud_stream(
        "synthetic-entitlement", gk::Language{"en-US"},
        gk::TimeZone::parse("UTC+00:00").value(), 1080);
  }

  static auto exact_spec() -> std::string {
    const std::size_t exact_spec_size =
        gk::Client::body_limit - prefix_size - 1;
    const std::string spec_prefix = "{\"k\":\"";
    return spec_prefix +
           std::string(exact_spec_size - spec_prefix.size() - 2, 'x') + "\"}";
  }

  const gk::ConfigKey config = GkRig::config_key("synthetic-key");
  RecordingHttp http;
};

TEST_F(GkStartRequest, PostsTheWrappedSpecWithTheConfigKey) {
  EXPECT_TRUE(start(spec));
  EXPECT_EQ(http.calls, 1U);
  const auto& request = http.last;
  EXPECT_EQ(request.label, "gk.start");
  EXPECT_EQ(request.method, platform::HttpMethod::post);
  EXPECT_EQ(request.url.view(),
            "https://cc.prod.gaikai.com/v1/sessions/start?npEnv=np");
  ASSERT_EQ(request.headers.size(), std::size_t{4});
  EXPECT_EQ(request.headers[0].name, "Content-Type");
  EXPECT_EQ(request.headers[0].value.view(), "application/json");
  EXPECT_EQ(request.headers[1].name, "Accept");
  EXPECT_EQ(request.headers[1].value.view(), "*/*");
  EXPECT_EQ(request.headers[2].name, "User-Agent");
  EXPECT_EQ(request.headers[2].value.view(),
            "PlayStation Portal/6.0.0-rel.444+6a9cea6f5");
  EXPECT_EQ(request.headers[3].name, "X-Gaikai-Session");
  EXPECT_EQ(request.headers[3].value.view(), "synthetic-key");
  EXPECT_EQ(
      std::string{request.body.view()},
      std::string{"{\"requestGameSpecification\":"} + std::string{spec} + "}");
  EXPECT_EQ(request.connect_timeout, std::chrono::seconds{10});
  EXPECT_EQ(request.total_timeout, std::chrono::seconds{30});
  EXPECT_EQ(request.redirect_policy, platform::HttpRedirectPolicy::deny);
  EXPECT_EQ(request.maximum_response_bytes, gk::Client::body_limit);
}

TEST_F(GkStartRequest, WrapsAFullLaunchSpecAsItIs) {
  const auto full = full_spec();
  ASSERT_TRUE(full);
  EXPECT_TRUE(start(full.value().json()));
  EXPECT_EQ(http.last.body.view(), "{\"requestGameSpecification\":" +
                                       std::string{full.value().json()} + "}");
}

TEST_F(GkStartRequest, RewritesTheSpecAsCompactJson) {
  EXPECT_TRUE(start(
      R"({ "a" : "xé\u001F\u0001\/" , "n" : 12, "d" : 1.5, "t" : true, "z" : null,)"
      R"( "arr" : [ 1 , { "k" : "v" } ], "e" : {} , "neg": -7, "big": 18446744073709551615 })"));
  EXPECT_EQ(
      http.last.body.view(),
      "{\"requestGameSpecification\":{\"a\":\"x\xC3\xA9\\u001f\\u0001/"
      "\",\"n\":12,"
      "\"d\":1.5,\"t\":true,\"z\":null,\"arr\":[1,{\"k\":\"v\"}],\"e\":{},"
      "\"neg\":-7,"
      "\"big\":18446744073709551615}}");
}

TEST_F(GkStartRequest, NestsTheSpecAsAnObject) {
  const auto full = full_spec();
  ASSERT_TRUE(full);
  EXPECT_TRUE(start(full.value().json()));
  const auto wrapped = std::string{http.last.body.view()};
  const auto parsed = test_support::JsonTree::parse(wrapped);
  ASSERT_TRUE(parsed);
  const auto* nested = test_support::JsonTree::member(
      parsed.value(), "requestGameSpecification");
  ASSERT_NE(nested, nullptr);
  ASSERT_TRUE(nested->is_object());
  EXPECT_EQ(test_support::JsonTree::text_at(*nested, "entitlementId"),
            "synthetic-entitlement");
}

TEST_F(GkStartRequest, RefusesAnEmptyConfigKeyOrOneThatBreaksAHeader) {
  for (const auto invalid : {"", "bad\r\nkey", "bad\nkey"}) {
    SCOPED_TRACE(invalid);
    EXPECT_TRUE(GkRig::fails_for(start(spec, GkRig::config_key(invalid)),
                                 Reason::invalid_config_key));
  }
}

TEST_F(GkStartRequest, AcceptsALongConfigKeyWithHighBytes) {
  EXPECT_TRUE(start(spec, GkRig::config_key(std::string(4097, 'x') + " \x80")));
}

TEST_F(GkStartRequest, RefusesASpecThatIsNotAnObject) {
  for (const auto invalid : {"{", "[1,2]", "null"}) {
    SCOPED_TRACE(invalid);
    EXPECT_TRUE(GkRig::fails_for(start(invalid), Reason::invalid_spec));
  }
}

TEST_F(GkStartRequest, SendsABodyOfExactlyTheLimit) {
  const auto exact = exact_spec();
  ASSERT_EQ(exact.size(), gk::Client::body_limit - prefix_size - 1);
  EXPECT_TRUE(start(exact));
  EXPECT_EQ(http.last.body.size(), gk::Client::body_limit);
}

TEST_F(GkStartRequest, RefusesABodyOverTheLimitWithoutSending) {
  auto exact = exact_spec();
  exact.insert(exact.size() - 2, 1, 'x');
  EXPECT_TRUE(GkRig::fails_for(start(exact), Reason::body_too_large));
  EXPECT_TRUE(
      GkRig::fails_for(start(std::string(gk::Client::body_limit + 1, ' ')),
                       Reason::body_too_large));
  EXPECT_EQ(http.calls, 0U);
}

class StartProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    EXPECT_EQ(request.method, platform::HttpMethod::post);
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response{};
    response.status_code = status;
    for (const auto character : body)
      response.body.push_back(static_cast<std::byte>(character));
    for (const auto& [name, value] : headers)
      response.headers.push_back({name, libchiaking::SecretString{value}});
    return response;
  }

  std::optional<platform::HttpError> error;
  std::uint16_t status{200};
  std::string body{R"({"sessionId":"synthetic-session"})"};
  std::vector<std::pair<std::string, std::string>> headers;
  unsigned calls{};
};

class GkStartClient : public ::testing::Test {
 protected:
  auto start(const platform::StopToken& stop = {})
      -> libchiaking::Expected<gk::Started, gk::Error> {
    Gk client{provider, stop};
    return client.start(R"({"spec":true})", config);
  }

  const gk::ConfigKey config = GkRig::config_key("synthetic-key");
  StartProvider provider;
};

TEST_F(GkStartClient, StartsASessionAndRotatesTheTrimmedKey) {
  provider.headers = {{"X-Gaikai-Session", " rotated-synthetic-key "}};
  const auto result = start();
  ASSERT_TRUE(result);
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(result.value().session_id.view(), "synthetic-session");
  ASSERT_TRUE(result.value().rotated_key);
  EXPECT_EQ(result.value().rotated_key->view(), "rotated-synthetic-key");
}

TEST_F(GkStartClient, KeepsTheKeyWithoutAHeader) {
  const auto result = start();
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().rotated_key);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkStartClient, RefusesARotatedKeyThatBreaksAHeader) {
  provider.headers = {{"x-gaikai-session", "bad\r\nkey"}};
  EXPECT_TRUE(GkRig::fails_for(start(), Reason::invalid_session_key));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkStartClient, AsksForAnEntitlementRetryWithItsStatus) {
  provider.status = 401;
  provider.body = R"({"message":"noGameForEntitlement"})";
  const auto result = start();
  EXPECT_TRUE(GkRig::fails_for(result, Reason::entitlement_retry));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().http_status(), 401);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkStartClient, IsRejectedWithItsStatus) {
  provider.status = 403;
  provider.body = "forbidden";
  const auto result = start();
  EXPECT_TRUE(GkRig::fails_for(result, Reason::rejected));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().http_status(), 403);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkStartClient, CallsAnEmptyReplyMalformed) {
  provider.body.clear();
  EXPECT_TRUE(GkRig::fails_for(start(), Reason::malformed));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkStartClient, CallsAReplyThatIsNotJsonMalformed) {
  provider.body = "{not json";
  EXPECT_TRUE(GkRig::fails_for(start(), Reason::malformed));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkStartClient, ReportsATransportFailure) {
  provider.error = platform::HttpError::backend_failure;
  const auto result = start();
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_transport());
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkStartClient, ReportsACancelledRequest) {
  provider.error = platform::HttpError::cancelled;
  const auto result = start();
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkStartClient, SendsNothingOnceStopped) {
  provider.error = platform::HttpError::cancelled;
  const auto result =
      start(platform::StopToken{std::make_shared<test_support::Stopped>()});
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 0U);
}

}  // namespace
}  // namespace cloud_test
