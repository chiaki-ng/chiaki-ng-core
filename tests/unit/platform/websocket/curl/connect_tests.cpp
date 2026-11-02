#include "tests/support/stop_states.hpp"
#include "tests/unit/platform/websocket/curl/rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string_view>

namespace platform_curl_test {
namespace {

class CurlWebSocketConnect : public CurlWebSocketRig {};

TEST_F(CurlWebSocketConnect, RefusesAUrlWithAnEmbeddedNul) {
  WebSocketRequest request;
  request.url =
      libchiaking::SecretString{std::string_view("ws://example.test/a\0b", 21)};
  const auto invalid = provider.connect(request, {});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), WebSocketError::invalid_url);
}

TEST_F(CurlWebSocketConnect, IsCancelledWhenItsStopIsAlreadyRaised) {
  WebSocketRequest request;
  request.url = libchiaking::SecretString{"ws://127.0.0.1:1/unused"};
  const StopToken cancelled(std::make_shared<const test_support::Stopped>());
  const auto stopped = provider.connect(request, cancelled);
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error(), WebSocketError::cancelled);
}

}  // namespace
}  // namespace platform_curl_test
