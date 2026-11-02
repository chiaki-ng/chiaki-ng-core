#include "src/core/net/deadline_stop_state.hpp"
#include "tests/unit/core/psn/remote_play/rig.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace psn_test {
namespace {

using libchiaking::platform::StopToken;
using libchiaking::psn::remote_play::BoundedPushReceiveErrorKind;
using libchiaking::psn::remote_play::PushClient;
using libchiaking::psn::remote_play::PushErrorKind;
using libchiaking::psn::rest::NotificationType;
using Ending = ScriptedConnection::Ending;

class PushClientBoundedReceive : public ::testing::Test {
 protected:
  PushClientBoundedReceive() {
    connection.clock = &clock;
    connection.deadline = 200;
    connection.parent = parent_state.get();
  }

  auto receives() {
    return client.receive_bounded(connection, parent, clock, 200);
  }

  test_support::RefusingWebSocket provider{
      libchiaking::platform::WebSocketError::unsupported};
  PushClient client{provider};
  test_support::FakeClock clock{100};
  ScriptedConnection connection;
  std::shared_ptr<test_support::StopState> parent_state =
      std::make_shared<test_support::StopState>();
  StopToken parent{parent_state};
};

TEST_F(PushClientBoundedReceive, IsCancelledUnreadOnceTheParentStopped) {
  parent_state->stopped = true;
  const auto result = receives();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, BoundedPushReceiveErrorKind::cancelled);
  EXPECT_EQ(connection.calls, 0);
}

TEST_F(PushClientBoundedReceive, TimesOutUnreadAtTheDeadline) {
  clock.now_us = 200;
  const auto result = receives();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, BoundedPushReceiveErrorKind::timed_out);
  EXPECT_EQ(connection.calls, 0);
}

TEST_F(PushClientBoundedReceive, IsCancelledWhenTheParentStopsDuringReceive) {
  connection.ending = Ending::cancelled;
  const auto result = receives();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, BoundedPushReceiveErrorKind::cancelled);
  EXPECT_EQ(connection.calls, 1);
}

TEST_F(PushClientBoundedReceive, TimesOutWhenTheDeadlineCancelsTheReceive) {
  connection.ending = Ending::expire_cancelled;
  const auto result = receives();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, BoundedPushReceiveErrorKind::timed_out);
  EXPECT_EQ(connection.calls, 1);
}

TEST_F(PushClientBoundedReceive, ReportsAWebSocketFailure) {
  connection.ending = Ending::invalid;
  const auto result = receives();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, BoundedPushReceiveErrorKind::push_failure);
  EXPECT_EQ(result.error().push.kind, PushErrorKind::websocket_failure);
  EXPECT_EQ(connection.calls, 1);
}

TEST_F(PushClientBoundedReceive, DeliversAMessageArrivingAtTheDeadline) {
  connection.ending = Ending::expire_valid;
  const auto result = receives();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().type(),
            NotificationType::remote_play_session_created);
  EXPECT_EQ(connection.calls, 1);
}

TEST_F(PushClientBoundedReceive, TimesOutOnAnEarlierOuterDeadline) {
  connection.ending = Ending::expire_cancelled;
  connection.deadline = 150;
  auto outer =
      std::make_shared<libchiaking::net::DeadlineStopState>(parent, clock, 150);
  auto nested =
      client.receive_bounded(connection, StopToken{outer}, clock, 300);
  ASSERT_FALSE(nested);
  EXPECT_EQ(nested.error().kind, BoundedPushReceiveErrorKind::timed_out);
  const auto calls = connection.calls;
  nested = client.receive_bounded(connection, StopToken{outer}, clock, 300);
  ASSERT_FALSE(nested);
  EXPECT_EQ(nested.error().kind, BoundedPushReceiveErrorKind::timed_out);
  EXPECT_EQ(connection.calls, calls);
  parent_state->stopped = true;
  nested = client.receive_bounded(connection, StopToken{outer}, clock, 300);
  ASSERT_FALSE(nested);
  EXPECT_EQ(nested.error().kind, BoundedPushReceiveErrorKind::cancelled);
}

}  // namespace
}  // namespace psn_test
