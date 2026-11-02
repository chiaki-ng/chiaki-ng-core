#include "tests/unit/platform/http/curl/keep_alive_http_server.hpp"
#include "tests/unit/platform/http/curl/loopback_http_server.hpp"
#include "tests/unit/platform/http/curl/rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace platform_curl_test {
namespace {

class CurlHttpExchange : public CurlHttpProviders {};

TEST_P(CurlHttpExchange, SendsABinaryPostAndKeepsRepeatedHeadersBothWays) {
  const std::string response =
      "HTTP/1.1 200 OK\r\n"
      "X-Duplicate: first\r\nX-Duplicate: second\r\n"
      "Content-Length: 5\r\nConnection: close\r\n\r\n" +
      std::string("\x01\x00\xff\x7f\x0a", 5);
  LoopbackHttpServer server(response);
  ASSERT_TRUE(server.ready());

  HttpRequest request;
  request.method = HttpMethod::post;
  request.url = libchiaking::SecretString{server.url("/binary")};
  request.headers.push_back({"X-Request", libchiaking::SecretString{"one"}});
  request.headers.push_back({"X-Request", libchiaking::SecretString{"two"}});
  request.body =
      libchiaking::SecretBytes{std::string_view("\x10\x00\xff\x20", 4)};
  const auto result = provider().perform(request, {});
  server.join();
  ASSERT_TRUE(result);
  EXPECT_TRUE(server.succeeded());
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_EQ(result.value().reason, "OK");
  EXPECT_EQ(result.value().body.view(),
            std::string_view("\x01\x00\xff\x7f\x0a", 5));

  std::vector<std::string> duplicate_values;
  for (const auto& header : result.value().headers) {
    if (header.name == "X-Duplicate")
      duplicate_values.emplace_back(header.value.view());
  }
  EXPECT_EQ(duplicate_values, (std::vector<std::string>{"first", "second"}));

  const std::string& sent = server.request();
  const std::size_t separator = sent.find("\r\n\r\n");
  EXPECT_NE(sent.find("POST /binary HTTP/1.1"), std::string::npos);
  EXPECT_NE(sent.find("X-Request: one\r\nX-Request: two"), std::string::npos);
  ASSERT_NE(separator, std::string::npos);
  EXPECT_EQ(sent.substr(separator + 4), std::string("\x10\x00\xff\x20", 4));
}

TEST_P(CurlHttpExchange, KeepsHeaderNamesAndValueEndsAsTheServerSentThem) {
  LoopbackHttpServer server(
      "HTTP/1.1 200 OK\r\n"
      "X-Padded:  \tvalue \t\r\n"
      "X-Spaced : spaced\r\n"
      "Content-Length: 0\r\nConnection: close\r\n\r\n");
  HttpRequest request;
  request.url = libchiaking::SecretString{server.url("/heads")};
  const auto result = provider().perform(request, {});
  server.join();
  ASSERT_TRUE(result);
  std::vector<std::string> seen;
  for (const auto& header : result.value().headers)
    seen.push_back(header.name + "=" + std::string{header.value.view()});
  EXPECT_NE(std::ranges::find(seen, "X-Padded=value \t"), seen.end());
  EXPECT_NE(std::ranges::find(seen, "X-Spaced =spaced"), seen.end());
}

TEST_P(CurlHttpExchange, ReturnsANonSuccessStatusWithItsReason) {
  LoopbackHttpServer server(
      "HTTP/1.1 500 Internal Server Error\r\n"
      "Content-Length: 4\r\nConnection: close\r\n\r\nnope");
  HttpRequest request;
  request.url = libchiaking::SecretString{server.url("/error")};
  const auto result = provider().perform(request, {});
  server.join();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().status_code, 500);
  EXPECT_EQ(result.value().reason, "Internal Server Error");
}

TEST_P(CurlHttpExchange, ReturnsARedirectUnfollowedWhenRedirectsAreDenied) {
  LoopbackHttpServer server(
      "HTTP/1.1 302 Found\r\n"
      "Location: /not-followed\r\nContent-Length: 0\r\n"
      "Connection: close\r\n\r\n");
  HttpRequest request;
  request.url = libchiaking::SecretString{server.url("/redirect")};
  request.redirect_policy = HttpRedirectPolicy::deny;
  const auto result = provider().perform(request, {});
  server.join();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().status_code, 302);
  EXPECT_NE(server.request().find("GET /redirect HTTP/1.1"), std::string::npos);
}

TEST_P(CurlHttpExchange, NeverDoublesOrCarriesOverAUserAgent) {
  KeepAliveHttpServer server(2);
  ASSERT_TRUE(server.ready());
  HttpRequest with_agent;
  with_agent.url = libchiaking::SecretString{server.url("/agent")};
  with_agent.headers.push_back(
      {"User-Agent", libchiaking::SecretString{"RpNetHttpUtilImpl"}});
  EXPECT_TRUE(provider().perform(with_agent, {}));
  HttpRequest without_agent;
  without_agent.url = libchiaking::SecretString{server.url("/plain")};
  EXPECT_TRUE(provider().perform(without_agent, {}));
  server.join();
  ASSERT_EQ(server.requests().size(), 2U);
  EXPECT_EQ(count_occurrences(server.requests()[0], "User-Agent:"), 1U);
  EXPECT_NE(server.requests()[0].find("User-Agent: RpNetHttpUtilImpl\r\n"),
            std::string::npos);
  EXPECT_EQ(count_occurrences(server.requests()[1], "User-Agent:"), 0U);
}

INSTANTIATE_TEST_SUITE_P(EveryTransport, CurlHttpExchange,
                         testing::Values(CurlHttpKind::pooled,
                                         CurlHttpKind::multi,
                                         CurlHttpKind::multi_without_extras),
                         CurlHttpRig::kind_name);

}  // namespace
}  // namespace platform_curl_test
