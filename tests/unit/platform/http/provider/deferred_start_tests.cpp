#include "tests/unit/platform/http/provider/rig.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <utility>

namespace platform_http_test {
namespace {

using namespace libchiaking::platform;

class HttpDefaultStart : public ::testing::Test {
 protected:
  [[nodiscard]] static auto no_content() -> HttpResponse {
    HttpResponse response;
    response.status_code = 204;
    return response;
  }

  void start_deferred() {
    HttpRequest request;
    request.url = libchiaking::SecretString{"https://example.test/deferred"};
    pending.emplace(port.start(request, {}));
  }

  ScriptedHttpProvider provider{no_content(), std::nullopt};
  HttpProvider& port = provider;
  std::optional<PendingHttpResponse> pending;
};

TEST_F(HttpDefaultStart, SendsNothingUntilWaitedOn) {
  start_deferred();
  EXPECT_EQ(provider.performed(), 0);
}

TEST_F(HttpDefaultStart, SendsTheCopiedRequestOnTheFirstWait) {
  start_deferred();
  auto response = std::move(*pending).wait();
  ASSERT_TRUE(response);
  EXPECT_EQ(response.value().status_code, 204);
  EXPECT_EQ(provider.performed(), 1);
  EXPECT_EQ(provider.captured_request().url.view(),
            "https://example.test/deferred");
}

TEST_F(HttpDefaultStart, RefusesASecondWaitWithoutSendingAgain) {
  start_deferred();
  ASSERT_TRUE(std::move(*pending).wait());
  const auto again = std::move(*pending).wait();
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), HttpError::backend_failure);
  EXPECT_EQ(provider.performed(), 1);
}

TEST_F(HttpDefaultStart, NeverSendsAnAbandonedRequest) {
  {
    HttpRequest dropped;
    const auto abandoned = port.start(dropped, {});
  }
  EXPECT_EQ(provider.performed(), 0);
}

}  // namespace
}  // namespace platform_http_test
