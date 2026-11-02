#include "tests/unit/platform/http/provider/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <type_traits>

namespace platform_http_test {
namespace {

using namespace libchiaking::platform;

static_assert(!std::is_copy_constructible_v<HttpResponse>);
static_assert(!std::is_copy_assignable_v<HttpResponse>);
static_assert(std::is_nothrow_move_constructible_v<HttpResponse>);

class HttpResponseHandoff : public HttpValuesTest {
 protected:
  void SetUp() override {
    HttpResponse response;
    response.status_code = 500;
    response.reason = "Internal Server Error";
    response.headers.push_back({"X-Order", libchiaking::SecretString{"one"}});
    response.headers.push_back({"X-Order", libchiaking::SecretString{"two"}});
    response.final_url =
        libchiaking::SecretString{"https://example.test/final"};
    const auto body = bytes({0, 0x00, 0x21});
    response.body = libchiaking::SecretBytes{std::span<const std::byte>{body}};

    provider.emplace(response.clone(), std::nullopt);
    auto performed = provider->perform(HttpRequest{}, StopToken{});
    response.body.clear();
    response.headers.clear();
    ASSERT_TRUE(performed.has_value());
    returned.emplace(std::move(performed.value()));
  }

  std::optional<ScriptedHttpProvider> provider;
  std::optional<HttpResponse> returned;
};

TEST_F(HttpResponseHandoff, CallerGetsTheStatusReasonUrlAndBody) {
  EXPECT_EQ(returned->status_code, 500);
  EXPECT_EQ(returned->reason, "Internal Server Error");
  EXPECT_EQ(returned->final_url.view(), "https://example.test/final");
  EXPECT_TRUE(same_bytes(returned->body.bytes(), bytes({0, 0x00, 0x21})));
}

TEST_F(HttpResponseHandoff, CallerGetsDuplicateHeadersInOrder) {
  ASSERT_EQ(returned->headers.size(), 2U);
  EXPECT_EQ(returned->headers[0].value.view(), "one");
  EXPECT_EQ(returned->headers[1].value.view(), "two");
}

TEST(HttpProviderErrors, ReachTheCallerUnchanged) {
  const std::array errors = {
      HttpError::cancelled,          HttpError::timed_out,
      HttpError::invalid_url,        HttpError::tls_trust_failed,
      HttpError::proxy_failed,       HttpError::disconnected,
      HttpError::response_too_large, HttpError::unsupported,
      HttpError::backend_failure,
  };
  for (const auto error : errors) {
    ScriptedHttpProvider provider(HttpResponse{}, error);
    const auto result = provider.perform(HttpRequest{}, StopToken{});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), error);
  }
}

}  // namespace
}  // namespace platform_http_test
