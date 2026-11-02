#include "tests/support/stop_states.hpp"
#include "tests/unit/platform/websocket/curl/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

namespace platform_curl_test {
namespace {

using Mode = LoopbackWebSocketServer::Mode;

class CurlWebSocketInterrupt : public CurlWebSocketRig {};

TEST_F(CurlWebSocketInterrupt, WakesReceiveAndClosePromptly) {
  std::chrono::microseconds receiving{};
  std::chrono::microseconds closing{};
  for (int attempt = 0; attempt < 3; ++attempt) {
    LoopbackWebSocketServer server(Mode::keepalive);
    CurlWebSocketProvider attempt_provider{threading(), clock()};
    auto request = request_for(server, 1024);
    request.ping_interval = std::chrono::seconds(10);
    request.pong_timeout = std::chrono::seconds(1);
    request.close_mode = WebSocketCloseMode::transport_only;
    auto connected = attempt_provider.connect(request, {});
    ASSERT_TRUE(connected);
    auto& connection = *connected.value();
    for (int round = 0; round < 2; ++round) {
      auto state = std::make_shared<test_support::AtomicStop>();
      auto stopper =
          Thread::start(threading(), "stopper", ThreadRole::unspecified, [&] {
            pause(std::chrono::milliseconds(5));
            state->stopped = true;
            connection.interrupt_receive();
          });
      ASSERT_TRUE(stopper);
      const auto before = now();
      auto result = connection.receive(StopToken{state});
      receiving += now() - before;
      static_cast<void>(stopper.value().join());
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error(), WebSocketError::cancelled);
    }
    pause(std::chrono::milliseconds(5));
    const auto before = now();
    EXPECT_TRUE(connection.close());
    closing += now() - before;
    server.join();
    EXPECT_TRUE(server.succeeded());
  }
  EXPECT_LE(receiving, std::chrono::milliseconds(120));
  EXPECT_LE(closing, std::chrono::milliseconds(60));
}

}  // namespace
}  // namespace platform_curl_test
