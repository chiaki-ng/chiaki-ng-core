#include "tests/unit/platform/websocket/curl/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace platform_curl_test {
namespace {

class CurlWebSocketMessages : public CurlWebSocketRig {};

TEST_F(CurlWebSocketMessages, ReassembleAFragmentedMessageAroundAPing) {
  LoopbackWebSocketServer server;
  ASSERT_TRUE(server.ready());
  auto connected = provider.connect(request_for(server, 1024), {});
  ASSERT_TRUE(connected);
  auto message = connected.value()->receive({});
  ASSERT_TRUE(message);
  EXPECT_EQ(message.value().type, WebSocketMessageType::binary);
  EXPECT_EQ(message.value().payload.view(), std::string_view{"hello world"});
  EXPECT_TRUE(connected.value()->close());
  EXPECT_TRUE(connected.value()->close());
  server.join();
  EXPECT_TRUE(server.succeeded());
  EXPECT_NE(server.request().find("GET /chiaki?x=1 HTTP/1.1"),
            std::string::npos);
  EXPECT_NE(server.request().find("X-Ordered: first\r\nX-Ordered: second"),
            std::string::npos);
  EXPECT_NE(server.request().find("Sec-WebSocket-Protocol: chiaki"),
            std::string::npos);
}

TEST_F(CurlWebSocketMessages, AreRefusedPastTheirSizeLimit) {
  LoopbackWebSocketServer server;
  auto connected = provider.connect(request_for(server, 4), {});
  ASSERT_TRUE(connected);
  const auto message = connected.value()->receive({});
  ASSERT_FALSE(message);
  EXPECT_EQ(message.error(), WebSocketError::message_too_large);
  (void)connected.value()->close();
  server.join();
}

}  // namespace
}  // namespace platform_curl_test
