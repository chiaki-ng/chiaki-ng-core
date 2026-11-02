#include "tests/support/stop_states.hpp"
#include "tests/unit/platform/websocket/provider/rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <type_traits>

namespace platform_websocket_test {
namespace {

using namespace libchiaking::platform;

static_assert(std::has_virtual_destructor_v<WebSocketConnection>);
static_assert(!std::is_copy_constructible_v<WebSocketConnection>);
static_assert(!std::is_copy_assignable_v<WebSocketConnection>);

class WebSocketLifecycle : public ::testing::Test {
 protected:
  void SetUp() override { stop_state->stopped = true; }

  std::shared_ptr<test_support::StopState> stop_state =
      std::make_shared<test_support::StopState>();
  const StopToken stopped{stop_state};
  ScriptedConnection connection{WebSocketMessage{}, std::nullopt, std::nullopt};
};

TEST_F(WebSocketLifecycle, ConnectIsCancelledByARaisedStop) {
  ScriptedProvider provider(WebSocketMessage{}, std::nullopt);
  const auto result = provider.connect(WebSocketRequest{}, stopped);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), WebSocketError::cancelled);
  EXPECT_TRUE(provider.captured_stop_requested());
}

TEST_F(WebSocketLifecycle, ReceiveIsCancelledByARaisedStop) {
  const auto result = connection.receive(stopped);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), WebSocketError::cancelled);
}

TEST_F(WebSocketLifecycle, ClosingTwiceSucceedsBothTimes) {
  const auto first_close = connection.close();
  const auto second_close = connection.close();
  EXPECT_TRUE(first_close.has_value());
  EXPECT_TRUE(second_close.has_value());
  EXPECT_EQ(connection.close_count(), 2);
}

TEST_F(WebSocketLifecycle, CloseFailureReachesTheCaller) {
  ScriptedConnection failing(WebSocketMessage{}, std::nullopt,
                             WebSocketError::backend_failure);
  const auto result = failing.close();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), WebSocketError::backend_failure);
}

}  // namespace
}  // namespace platform_websocket_test
