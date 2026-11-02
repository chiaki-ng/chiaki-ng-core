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

class GkAuthorizeRequest : public ::testing::Test {
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
      -> libchiaking::Expected<gk::Authorized, gk::Error> {
    Gk client{http};
    return client.authorize(GkRig::session_id(session_id),
                            GkRig::config_key(key), spec);
  }

  auto send(std::string_view session_id, std::string_view key)
      -> libchiaking::Expected<gk::Authorized, gk::Error> {
    return send(authorized->view(), session_id, key);
  }

  RecordingHttp http;
  std::optional<libchiaking::SecretString> authorized;
};

TEST_F(GkAuthorizeRequest, PostsTheSignedSpecWithTheSessionHeaders) {
  ASSERT_TRUE(send("synthetic-session", "synthetic-key"));
  const auto& request = http.last;
  EXPECT_EQ(request.label, "gk.authorize");
  EXPECT_EQ(request.method, platform::HttpMethod::post);
  EXPECT_EQ(
      request.url.view(),
      "https://cc.prod.gaikai.com/v1/sessions/synthetic-session/authorize");
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
}

TEST_F(GkAuthorizeRequest, RefusesAnEmptySessionIdOrOneThatBreaksAPath) {
  EXPECT_TRUE(GkRig::fails_for(send("bad\nid", "synthetic-key"),
                               Reason::invalid_session_id));
  EXPECT_TRUE(
      GkRig::fails_for(send("", "synthetic-key"), Reason::invalid_session_id));
}

TEST_F(GkAuthorizeRequest, AcceptsALongSessionId) {
  EXPECT_TRUE(send(std::string(127, 'a'), "synthetic-key"));
  EXPECT_TRUE(send(std::string(128, 'a'), "synthetic-key"));
}

TEST_F(GkAuthorizeRequest, EscapesTheSessionIdInThePathOnly) {
  ASSERT_TRUE(send("a/b c\xC3\xA9", "key with space"));
  EXPECT_NE(http.last.url.view().find("/sessions/a%2Fb%20c%C3%A9/"),
            std::string::npos);
  ASSERT_EQ(http.last.headers.size(), std::size_t{5});
  EXPECT_EQ(http.last.headers[3].value.view(), "a/b c\xC3\xA9");
  EXPECT_EQ(http.last.headers[4].value.view(), "key with space");
}

TEST_F(GkAuthorizeRequest, RefusesAConfigKeyThatBreaksAHeader) {
  EXPECT_TRUE(GkRig::fails_for(send("synthetic-session", "bad\r\nkey"),
                               Reason::invalid_config_key));
}

TEST_F(GkAuthorizeRequest, RefusesASpecThatIsNotAnObject) {
  EXPECT_TRUE(
      GkRig::fails_for(send("bad-json", "synthetic-session", "synthetic-key"),
                       Reason::invalid_spec));
  EXPECT_TRUE(GkRig::fails_for(send("[]", "synthetic-session", "synthetic-key"),
                               Reason::invalid_spec));
}

TEST_F(GkAuthorizeRequest, RefusesASpecOverTheBodyLimit) {
  EXPECT_TRUE(GkRig::fails_for(send(std::string(gk::Client::body_limit, 'x'),
                                    "synthetic-session", "synthetic-key"),
                               Reason::body_too_large));
}

class AuthorizeProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    EXPECT_EQ(request.method, platform::HttpMethod::post);
    EXPECT_EQ(request.headers.size(), std::size_t{5});
    if (request.headers.size() > 4) {
      EXPECT_EQ(request.headers[4].value.view(), "synthetic-key");
    }
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
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  unsigned calls{};
};

class GkAuthorizeClient : public ::testing::Test {
 protected:
  auto authorize(const platform::StopToken& stop = {})
      -> libchiaking::Expected<gk::Authorized, gk::Error> {
    Gk client{provider, stop};
    return client.authorize(GkRig::session_id("synthetic-session"),
                            GkRig::config_key("synthetic-key"), "{}");
  }

  void rejects(std::vector<std::pair<std::string, std::string>> headers,
               std::string body) {
    provider.status = 403;
    provider.headers = std::move(headers);
    provider.body = std::move(body);
  }

  AuthorizeProvider provider;
};

TEST_F(GkAuthorizeClient, RotatesTheTrimmedKeyFromTheReply) {
  provider.headers = {{"X-Gaikai-Session", " rotated-key "}};
  const auto result = authorize();
  ASSERT_TRUE(result);
  ASSERT_TRUE(result.value().rotated_key);
  EXPECT_EQ(result.value().rotated_key->view(), "rotated-key");
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAuthorizeClient, KeepsTheKeyWithoutAHeader) {
  const auto result = authorize();
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().rotated_key);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAuthorizeClient, KeepsTheKeyForABlankHeader) {
  provider.headers = {{"X-Gaikai-Session", " \t "}};
  const auto result = authorize();
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().rotated_key);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAuthorizeClient, RefusesARotatedKeyThatBreaksAHeader) {
  provider.headers = {{"X-Gaikai-Session", "bad\r\nkey"}};
  EXPECT_TRUE(GkRig::fails_for(authorize(), Reason::invalid_session_key));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAuthorizeClient, ReadsAPlusRefusalFromTheEventHeaderInAnyCase) {
  rejects({{"x-GAIKAI-EVENT", "error 002.2001"}}, "");
  EXPECT_TRUE(GkRig::fails_for(authorize(), Reason::psplus_rejected));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAuthorizeClient, ReadsOnlyTheFirstEventHeader) {
  rejects({{"x-gaikai-event", "other"}, {"x-gaikai-event", "002.2001"}}, "");
  EXPECT_TRUE(GkRig::fails_for(authorize(), Reason::rejected));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAuthorizeClient, ReadsAPlusRefusalFromTheBody) {
  rejects({}, "error 002.2001");
  EXPECT_TRUE(GkRig::fails_for(authorize(), Reason::psplus_rejected));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAuthorizeClient, ReadsAPlusRefusalPastANulInTheBody) {
  rejects({}, std::string{"x\0", 2} + "002.2001");
  EXPECT_TRUE(GkRig::fails_for(authorize(), Reason::psplus_rejected));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAuthorizeClient, AsksForAnEntitlementRetryWithoutAGame) {
  rejects({}, R"({"name":"noGameForEntitlementId"})");
  EXPECT_TRUE(GkRig::fails_for(authorize(), Reason::entitlement_retry));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAuthorizeClient, ReadsAPlusRefusalPastANulInTheHeader) {
  rejects({{"x-gaikai-event", std::string{"x\0", 2} + "002.2001"}}, "other");
  EXPECT_TRUE(GkRig::fails_for(authorize(), Reason::psplus_rejected));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAuthorizeClient, ReportsATransportFailure) {
  provider.error = platform::HttpError::backend_failure;
  const auto result = authorize();
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_transport());
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAuthorizeClient, ReportsACancelledRequest) {
  provider.error = platform::HttpError::cancelled;
  const auto result = authorize();
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(GkAuthorizeClient, SendsNothingOnceStopped) {
  const auto result =
      authorize(platform::StopToken{std::make_shared<test_support::Stopped>()});
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(provider.calls, 0U);
}

}  // namespace
}  // namespace cloud_test
