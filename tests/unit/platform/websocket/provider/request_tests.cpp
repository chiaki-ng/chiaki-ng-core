#include "tests/unit/platform/websocket/provider/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace platform_websocket_test {
namespace {

using namespace libchiaking::platform;

static_assert(std::has_virtual_destructor_v<WebSocketProvider>);
static_assert(!std::is_copy_constructible_v<WebSocketProvider>);
static_assert(!std::is_copy_assignable_v<WebSocketProvider>);
static_assert(!std::is_copy_constructible_v<WebSocketRequest>);
static_assert(!std::is_copy_assignable_v<WebSocketRequest>);
static_assert(std::is_nothrow_move_constructible_v<WebSocketRequest>);

class WebSocketRequestHandoff : public WebSocketValuesTest {
 protected:
  void SetUp() override {
    request.url = libchiaking::SecretString{
        std::string_view("wss://example.test/a\0b", 22)};
    request.headers.push_back(
        {"X-Duplicate", libchiaking::SecretString{"first"}});
    request.headers.push_back(
        {"Sec-WebSocket-Protocol", libchiaking::SecretString{"chiaki"}});
    request.headers.push_back(
        {"X-Duplicate", libchiaking::SecretString{"second"}});
    request.subprotocol = std::string("chiaki\0v1", 9);
    request.connect_timeout = std::chrono::milliseconds(150);
    request.ping_interval = std::chrono::milliseconds(30);
    request.pong_timeout = std::chrono::milliseconds(10);
    request.maximum_message_bytes = 4096;
    request.tls_configuration.trust_mode =
        TlsTrustMode::system_plus_custom_ca_pem;
    request.tls_configuration.custom_ca_pem = bytes({0, 0x00, 0x0a});
    request.tls_configuration.proxy_policy = HttpProxyPolicy::explicit_proxy;
    request.tls_configuration.explicit_proxy_url = "https://proxy.test";

    original = request.clone();
    auto connected = provider.connect(request, StopToken{});
    request.url = libchiaking::SecretString{"changed"};
    request.headers.clear();
    request.subprotocol = "changed";
    request.tls_configuration.custom_ca_pem.clear();
    ASSERT_FALSE(connected.has_value());
    error = connected.error();
  }

  WebSocketRequest request;
  WebSocketRequest original;
  ScriptedProvider provider{WebSocketMessage{}, WebSocketError::cancelled};
  std::optional<WebSocketError> error;
};

TEST_F(WebSocketRequestHandoff, ProviderErrorReachesTheCaller) {
  EXPECT_EQ(error, WebSocketError::cancelled);
}

TEST_F(WebSocketRequestHandoff, ProviderSeesACloneUnaffectedByLaterChanges) {
  const auto& captured = provider.captured_request();
  EXPECT_EQ(captured.url.view(), original.url.view());
  EXPECT_TRUE(same_headers(captured.headers, original.headers));
  EXPECT_EQ(captured.subprotocol, original.subprotocol);
  EXPECT_EQ(captured.connect_timeout, std::chrono::milliseconds(150));
  EXPECT_EQ(captured.ping_interval, std::chrono::milliseconds(30));
  EXPECT_EQ(captured.pong_timeout, std::chrono::milliseconds(10));
  EXPECT_EQ(captured.maximum_message_bytes, 4096U);
  EXPECT_EQ(captured.tls_configuration.trust_mode,
            TlsTrustMode::system_plus_custom_ca_pem);
  EXPECT_EQ(captured.tls_configuration.custom_ca_pem,
            original.tls_configuration.custom_ca_pem);
  EXPECT_EQ(captured.tls_configuration.proxy_policy,
            HttpProxyPolicy::explicit_proxy);
  EXPECT_EQ(captured.tls_configuration.explicit_proxy_url,
            "https://proxy.test");
}

TEST_F(WebSocketRequestHandoff, KeepsDuplicateHeadersInOrder) {
  const auto& headers = provider.captured_request().headers;
  ASSERT_EQ(headers.size(), 3U);
  EXPECT_EQ(headers[0].value.view(), "first");
  EXPECT_EQ(headers[2].value.view(), "second");
}

TEST_F(WebSocketRequestHandoff, PassesEveryConnectErrorThrough) {
  for (const auto expected : every_error) {
    ScriptedProvider failing(WebSocketMessage{}, expected);
    const auto result = failing.connect(WebSocketRequest{}, StopToken{});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), expected);
  }
}

TEST(WebSocketRequestDefaults, AreEmptyUnboundedAndUseSystemTrust) {
  const WebSocketRequest request;
  EXPECT_TRUE(request.url.empty());
  EXPECT_TRUE(request.headers.empty());
  EXPECT_TRUE(request.subprotocol.empty());
  EXPECT_EQ(request.connect_timeout, std::chrono::milliseconds(0));
  EXPECT_EQ(request.ping_interval, std::chrono::milliseconds(0));
  EXPECT_EQ(request.pong_timeout, std::chrono::milliseconds(0));
  EXPECT_EQ(request.maximum_message_bytes, 0U);
  EXPECT_EQ(request.tls_configuration.trust_mode, TlsTrustMode::system);
  EXPECT_TRUE(request.tls_configuration.custom_ca_pem.empty());
  EXPECT_EQ(request.tls_configuration.proxy_policy, HttpProxyPolicy::system);
  EXPECT_TRUE(request.tls_configuration.explicit_proxy_url.empty());
}

}  // namespace
}  // namespace platform_websocket_test
