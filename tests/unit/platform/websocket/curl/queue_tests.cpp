#include "tests/unit/platform/websocket/curl/rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace platform_curl_test {
namespace {

using Mode = LoopbackWebSocketServer::Mode;

class CurlWebSocketQueue : public CurlWebSocketRig,
                           public testing::WithParamInterface<Mode> {};

TEST_P(CurlWebSocketQueue, HoldsAtMostItsLimitOfUnreadMessages) {
  LoopbackWebSocketServer server(GetParam());
  auto connected = provider.connect(request_for(server, 16384), {});
  ASSERT_TRUE(connected);
  server.join();
  EXPECT_TRUE(server.succeeded());
  unsigned count = 0;
  for (;;) {
    auto result = connected.value()->receive({});
    if (!result) {
      EXPECT_EQ(result.error(), WebSocketError::message_too_large);
      break;
    }
    ++count;
  }
  EXPECT_EQ(count, GetParam() == Mode::flood ? 64U : 32U);
  (void)connected.value()->close();
}

INSTANTIATE_TEST_SUITE_P(
    EachFlood, CurlWebSocketQueue,
    testing::Values(Mode::flood, Mode::byte_flood),
    [](const testing::TestParamInfo<Mode>& info) -> std::string {
      return info.param == Mode::flood ? "ManySmallMessages"
                                       : "ManyLargeMessages";
    });

}  // namespace
}  // namespace platform_curl_test
