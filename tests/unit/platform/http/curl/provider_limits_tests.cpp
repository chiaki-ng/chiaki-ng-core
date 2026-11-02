#include "tests/support/stop_states.hpp"
#include "tests/unit/platform/http/curl/loopback_http_server.hpp"
#include "tests/unit/platform/http/curl/rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string_view>

namespace platform_curl_test {
namespace {

class CurlHttpLimits : public CurlHttpProviders {};

TEST_P(CurlHttpLimits, RefusesAResponseLargerThanItsLimit) {
  LoopbackHttpServer server(
      "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n"
      "Connection: close\r\n\r\n12345678");
  HttpRequest request;
  request.url = libchiaking::SecretString{server.url("/large")};
  request.maximum_response_bytes = 4;
  const auto limited = provider().perform(request, {});
  server.join();
  ASSERT_FALSE(limited);
  EXPECT_EQ(limited.error(), HttpError::response_too_large);
}

TEST_P(CurlHttpLimits, RefusesAUrlWithAnEmbeddedNul) {
  HttpRequest request;
  request.url = libchiaking::SecretString{
      std::string_view("http://example.test/a\0b", 23)};
  const auto invalid = provider().perform(request, {});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), HttpError::invalid_url);
}

TEST_P(CurlHttpLimits, CancelsARequestWhoseStopIsAlreadyRaised) {
  HttpRequest request;
  request.url = libchiaking::SecretString{"http://127.0.0.1:1/unused"};
  const StopToken cancelled(std::make_shared<const test_support::Stopped>());
  const auto stopped = provider().perform(request, cancelled);
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error(), HttpError::cancelled);
}

INSTANTIATE_TEST_SUITE_P(EveryTransport, CurlHttpLimits,
                         testing::Values(CurlHttpKind::pooled,
                                         CurlHttpKind::multi,
                                         CurlHttpKind::multi_without_extras),
                         CurlHttpRig::kind_name);

}  // namespace
}  // namespace platform_curl_test
