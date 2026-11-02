#include "tests/support/stop_states.hpp"
#include "tests/unit/platform/websocket/curl/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

namespace platform_curl_test {
namespace {

using Mode = LoopbackWebSocketServer::Mode;

class CurlWebSocketKeepalive : public CurlWebSocketRig {};

TEST_F(CurlWebSocketKeepalive, PingsOnceRightAwayWhenAskedTo) {
  LoopbackWebSocketServer server(Mode::keepalive);
  auto request = request_for(server, 1024);
  request.ping_interval = std::chrono::seconds(2);
  request.pong_timeout = std::chrono::seconds(1);
  request.ping_immediately = true;
  request.close_mode = WebSocketCloseMode::transport_only;
  auto connected = provider.connect(request, {});
  ASSERT_TRUE(connected);
  auto first = connected.value()->receive(
      deadline_after(std::chrono::milliseconds(250)));
  const bool closed = static_cast<bool>(connected.value()->close());
  server.join();
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value().payload.size(), 6U);
  EXPECT_TRUE(closed);
  EXPECT_EQ(server.pings(), 1U);
  EXPECT_EQ(server.closes(), 0U);
  EXPECT_TRUE(server.succeeded());
}

TEST_F(CurlWebSocketKeepalive, KeepsAPongArrivingAfterTheDeadlineBuffered) {
  LoopbackWebSocketServer server(Mode::late_pong);
  auto request = request_for(server, 1024);
  request.ping_interval = std::chrono::seconds(10);
  request.pong_timeout = std::chrono::milliseconds(10);
  request.ping_immediately = true;
  auto connected = provider.connect(request, {});
  ASSERT_TRUE(connected);
  auto result = connected.value()->receive(
      deadline_after(std::chrono::milliseconds(300)));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), WebSocketError::cancelled);
  EXPECT_EQ(server.pings(), 1U);
  (void)connected.value()->close();
}

TEST_F(CurlWebSocketKeepalive, TimesOutWhenAPongNeverArrives) {
  LoopbackWebSocketServer server(Mode::missing_pong);
  auto request = request_for(server, 1024);
  request.ping_interval = std::chrono::milliseconds(50);
  request.pong_timeout = std::chrono::milliseconds(150);
  auto connected = provider.connect(request, {});
  ASSERT_TRUE(connected);
  const auto before = now();
  auto result = connected.value()->receive(
      deadline_after(std::chrono::milliseconds(2000)));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), WebSocketError::timed_out);
  EXPECT_LE(now() - before, std::chrono::seconds(2));
  (void)connected.value()->close();
}

class CurlWebSocketKeepaliveBetweenReceives
    : public CurlWebSocketRig,
      public testing::WithParamInterface<WebSocketCloseMode> {};

TEST_P(CurlWebSocketKeepaliveBetweenReceives, KeepsTheConnectionAnswering) {
  const auto close_mode = GetParam();
  LoopbackWebSocketServer server(Mode::keepalive);
  auto request = request_for(server, 1024);
  request.ping_interval = std::chrono::milliseconds(50);
  request.pong_timeout = std::chrono::milliseconds(250);
  request.close_mode = close_mode;
  auto connected = provider.connect(request, {});
  ASSERT_TRUE(connected);
  auto& connection = *connected.value();
  auto first = connection.receive({});
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value().payload.size(), 6U);
  pause(std::chrono::milliseconds(700));
  auto second = connection.receive({});
  ASSERT_TRUE(second);
  EXPECT_EQ(second.value().payload.size(), 5U);
  EXPECT_GE(server.pings(), 3U);

  const StopToken cancelled(std::make_shared<const test_support::Stopped>());
  auto stopped = connection.receive(cancelled);
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error(), WebSocketError::cancelled);
  auto interrupted =
      connection.receive(deadline_after(std::chrono::milliseconds(100)));
  ASSERT_FALSE(interrupted);
  EXPECT_EQ(interrupted.error(), WebSocketError::cancelled);
  EXPECT_GE(server.pings(), 4U);

  const auto before = now();
  EXPECT_TRUE(connection.close());
  EXPECT_TRUE(connection.close());
  EXPECT_LE(now() - before, std::chrono::seconds(2));
  auto closed = connection.receive({});
  ASSERT_FALSE(closed);
  EXPECT_EQ(closed.error(), WebSocketError::disconnected);
  server.join();
  EXPECT_EQ(server.closes(),
            close_mode == WebSocketCloseMode::protocol_close ? 1U : 0U);
  EXPECT_TRUE(server.succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    EachCloseMode, CurlWebSocketKeepaliveBetweenReceives,
    testing::Values(WebSocketCloseMode::protocol_close,
                    WebSocketCloseMode::transport_only),
    [](const testing::TestParamInfo<WebSocketCloseMode>& info) -> std::string {
      return info.param == WebSocketCloseMode::protocol_close ? "ProtocolClose"
                                                              : "TransportOnly";
    });

}  // namespace
}  // namespace platform_curl_test
