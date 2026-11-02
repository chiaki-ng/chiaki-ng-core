#include "tests/unit/platform/http/provider/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string_view>
#include <type_traits>

namespace platform_http_test {
namespace {

using namespace libchiaking::platform;

static_assert(std::has_virtual_destructor_v<HttpProvider>);
static_assert(!std::is_copy_constructible_v<HttpProvider>);
static_assert(!std::is_copy_assignable_v<HttpProvider>);
static_assert(!std::is_copy_constructible_v<HttpRequest>);
static_assert(!std::is_copy_assignable_v<HttpRequest>);
static_assert(std::is_nothrow_move_constructible_v<HttpRequest>);

class HttpRequestHandoff : public HttpValuesTest {
 protected:
  void SetUp() override {
    request.method = HttpMethod::delete_;
    request.url = libchiaking::SecretString{
        std::string_view("https://example.test/a\0b", 24)};
    request.headers.push_back(
        {"X-Duplicate", libchiaking::SecretString{"first"}});
    request.headers.push_back(
        {"Content-Type",
         libchiaking::SecretString{"application/octet-stream"}});
    request.headers.push_back(
        {"X-Duplicate", libchiaking::SecretString{"second"}});
    const auto body = bytes({0, 0x00, 0xff, 0x7f});
    request.body = libchiaking::SecretBytes{std::span<const std::byte>{body}};
    request.connect_timeout = std::chrono::milliseconds(150);
    request.total_timeout = std::chrono::milliseconds(900);
    request.redirect_policy = HttpRedirectPolicy::follow_https;
    request.maximum_response_bytes = 4096;
    request.tls_configuration.trust_mode =
        TlsTrustMode::system_plus_custom_ca_pem;
    request.tls_configuration.custom_ca_pem = bytes({0, 0x00, 0x0a});
    request.tls_configuration.proxy_policy = HttpProxyPolicy::explicit_proxy;
    request.tls_configuration.explicit_proxy_url = "https://proxy.test";

    original = request.clone();
    result.emplace(provider.perform(request, StopToken{}));
    request.url = libchiaking::SecretString{"changed"};
    request.body.clear();
    request.tls_configuration.custom_ca_pem.clear();
  }

  HttpRequest request;
  HttpRequest original;
  ScriptedHttpProvider provider{HttpResponse{}, HttpError::cancelled};
  std::optional<libchiaking::Expected<HttpResponse, HttpError>> result;
};

TEST_F(HttpRequestHandoff, ProviderErrorReachesTheCaller) {
  ASSERT_FALSE(result->has_value());
  EXPECT_EQ(result->error(), HttpError::cancelled);
}

TEST_F(HttpRequestHandoff, ProviderSeesACloneUnaffectedByLaterChanges) {
  const auto& captured = provider.captured_request();
  EXPECT_EQ(captured.method, HttpMethod::delete_);
  EXPECT_EQ(captured.url.view(), original.url.view());
  EXPECT_TRUE(same_headers(captured.headers, original.headers));
  EXPECT_TRUE(same_bytes(captured.body.bytes(), original.body.bytes()));
  EXPECT_EQ(captured.connect_timeout, std::chrono::milliseconds(150));
  EXPECT_EQ(captured.total_timeout, std::chrono::milliseconds(900));
  EXPECT_EQ(captured.redirect_policy, HttpRedirectPolicy::follow_https);
  EXPECT_EQ(captured.maximum_response_bytes, 4096U);
  EXPECT_EQ(captured.tls_configuration.trust_mode,
            TlsTrustMode::system_plus_custom_ca_pem);
  EXPECT_EQ(captured.tls_configuration.custom_ca_pem,
            original.tls_configuration.custom_ca_pem);
  EXPECT_EQ(captured.tls_configuration.proxy_policy,
            HttpProxyPolicy::explicit_proxy);
  EXPECT_EQ(captured.tls_configuration.explicit_proxy_url,
            "https://proxy.test");
}

TEST_F(HttpRequestHandoff, KeepsBinaryBodyAndCertificateBytes) {
  const auto& captured = provider.captured_request();
  EXPECT_TRUE(same_bytes(captured.body.bytes(), bytes({0, 0x00, 0xff, 0x7f})));
  EXPECT_TRUE(same_bytes(captured.tls_configuration.custom_ca_pem,
                         bytes({0, 0x00, 0x0a})));
}

TEST_F(HttpRequestHandoff, KeepsDuplicateHeadersInOrder) {
  const auto& headers = provider.captured_request().headers;
  ASSERT_EQ(headers.size(), 3U);
  EXPECT_EQ(headers[0].value.view(), "first");
  EXPECT_EQ(headers[2].value.view(), "second");
}

TEST(HttpRequestDefaults, AreAPlainBoundedGetWithSystemTrust) {
  const HttpRequest request;
  EXPECT_EQ(request.method, HttpMethod::get);
  EXPECT_EQ(request.redirect_policy, HttpRedirectPolicy::deny);
  EXPECT_EQ(request.connect_timeout, std::chrono::milliseconds(0));
  EXPECT_EQ(request.total_timeout, std::chrono::seconds(30));
  EXPECT_EQ(request.maximum_response_bytes, 4U * 1024U * 1024U);
  EXPECT_EQ(request.tls_configuration.trust_mode, TlsTrustMode::system);
  EXPECT_TRUE(request.tls_configuration.custom_ca_pem.empty());
  EXPECT_EQ(request.tls_configuration.proxy_policy, HttpProxyPolicy::system);
  EXPECT_TRUE(request.tls_configuration.explicit_proxy_url.empty());
}

}  // namespace
}  // namespace platform_http_test
