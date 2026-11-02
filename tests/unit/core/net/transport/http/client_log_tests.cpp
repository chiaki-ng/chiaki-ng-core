#include "tests/unit/core/net/transport/http/client_rig.hpp"

#include "src/core/net/transport/http/request_log.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace http_test {
namespace {

using libchiaking::net::http::Client;
using libchiaking::net::http::Endpoint;
using libchiaking::net::http::Method;
using libchiaking::net::http::Request;
using libchiaking::net::http::Scrubber;
using test_support::psn::Reply;
namespace platform = libchiaking::platform;

class HttpClientLog : public ClientRig {};

TEST_F(HttpClientLog, WritesOnlyWithASink) {
  const Client silent{fake, clock, stop};
  silent.log(platform::LogSeverity::info, "dropped");
  EXPECT_EQ(silent.diagnostics(), nullptr);
  test_support::FakeLog sink;
  const Client logging{fake, clock, stop, &sink};
  logging.log(platform::LogSeverity::warning, "[HTTP] kept");
  EXPECT_TRUE(sink.contains("[HTTP] kept"));
}

TEST_F(HttpClientLog, NamesLabelTargetAndCredentialWithoutSecrets) {
  const std::string target = at("?npsso=QUERY-SECRET");
  fake.on("POST", target,
          {Reply{401, "denied for Bearer TOKEN-SECRET-1\n\x01 retry later"}});
  constexpr Endpoint labelled{.label = "test.label", .method = Method::post};
  Request request{labelled, target};
  request.authorize(TestBearer{"TOKEN-SECRET-1", "commerce bearer"});
  request.body(libchiaking::SecretBytes{std::string_view{"code=BODY-SECRET"}});
  const auto sent = client.send(std::move(request));
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value().status(), 401);
  EXPECT_TRUE(
      log.contains("[HTTP] test.label POST m.np.playstation.com/api/example "
                   "auth=commerce bearer"));
  EXPECT_TRUE(
      log.contains("[HTTP] test.label client_error HTTP 401 attempts=1"));
  EXPECT_TRUE(log.contains("body=\"denied for *** ***"));
  EXPECT_FALSE(log.contains("TOKEN-SECRET-1"));
  EXPECT_FALSE(log.contains("QUERY-SECRET"));
  EXPECT_FALSE(log.contains("BODY-SECRET"));
  EXPECT_FALSE(log.contains("\x01"));
}

TEST_F(HttpClientLog, HidesSecretHeaderValuesInTheTarget) {
  const std::string target =
      "https://cc.prod.gaikai.com/v1/sessions/"
      "id%2FPATH-SECRET/lock?forceLogout=true";
  fake.on("POST", target, {Reply{200, "{}"}});
  constexpr Endpoint session{
      .label = "test.session", .method = Method::post, .log_query = true};
  Request request{session, target};
  request.header("X-Gaikai-SessionId",
                 libchiaking::SecretString{"id/PATH-SECRET"});
  EXPECT_TRUE(client.send(std::move(request)));
  EXPECT_TRUE(
      log.contains("[HTTP] test.session POST "
                   "cc.prod.gaikai.com/v1/sessions/***/lock?forceLogout=true"));
  EXPECT_FALSE(log.contains("PATH-SECRET"));
}

TEST_F(HttpClientLog, NamesTheTransportError) {
  fake.on("GET", std::string{url}, {Reply::transport()});
  constexpr Endpoint down{.label = "test.down"};
  EXPECT_FALSE(client.send(Request{down, url}));
  EXPECT_TRUE(
      log.contains("[HTTP] test.down transport_error disconnected attempts=1"));
}

TEST(HttpScrubber, BoundsTheExcerpt) {
  const std::string body(400, 'x');
  EXPECT_EQ(Scrubber{{}}.excerpt(body).size(),
            libchiaking::net::http::log_excerpt_bytes);
}

class HttpScrubberWithHeaders : public ::testing::Test {
 protected:
  static auto sent_headers() -> std::vector<platform::HttpHeader> {
    std::vector<platform::HttpHeader> sent;
    sent.push_back({"Cookie", libchiaking::SecretString{"npsso=SECRET-VALUE"}});
    sent.push_back({"Accept", libchiaking::SecretString{"text/plain"}});
    return sent;
  }

  const std::vector<platform::HttpHeader> sent{sent_headers()};
  const Scrubber scrubber{sent};
};

TEST_F(HttpScrubberWithHeaders, MasksSentSecretsAndControlsInTheExcerpt) {
  EXPECT_EQ(scrubber.excerpt("a\tSECRET-VALUE\r\ntext/plain"),
            "a.***..text/plain");
}

TEST_F(HttpScrubberWithHeaders, MasksSentSecretsInTheTarget) {
  EXPECT_EQ(scrubber.target("https://host.example/SECRET-VALUE?x=1", true),
            "host.example/***?x=1");
}

TEST(HttpScrubber, LogsTheQueryOnlyWhenAsked) {
  EXPECT_EQ(Scrubber::log_target("https://host.example/a/b?x=1#frag", false),
            "host.example/a/b");
  EXPECT_EQ(Scrubber::log_target("https://host.example/a/b?x=1#frag", true),
            "host.example/a/b?x=1");
}

}  // namespace
}  // namespace http_test
