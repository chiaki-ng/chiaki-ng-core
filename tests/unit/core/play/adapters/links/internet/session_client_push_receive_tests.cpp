#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/session_client_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;
namespace rest = libchiaking::psn::rest;
using psn_remote_play::PushErrorKind;
using session_client_rig::QueuedPushConnection;
using Type = rest::NotificationType;

class PushClientConnect : public ::testing::Test {
 protected:
  static inline const libchiaking::psn::auth::BearerToken token{
      std::string_view{"SYNTHETIC_TEST_TOKEN"}};
  static constexpr std::string_view fqdn =
      "mobile-pushcl.np.communication.playstation.net";

  session_client_rig::ScriptedWebSocket websocket;
  psn_remote_play::PushClient client{websocket};
};

TEST_F(PushClientConnect, OpensTheNotificationSocket) {
  const auto ok = client.connect(token, fqdn, {});
  ASSERT_TRUE(ok);
  EXPECT_NE(ok.value().get(), nullptr);
  EXPECT_EQ(websocket.calls, 1U);
}

TEST_F(PushClientConnect, ReportsAFailedConnection) {
  websocket.error = platform::WebSocketError::timed_out;
  const auto timed_out = client.connect(token, fqdn, {});
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error().kind, PushErrorKind::websocket_failure);
  EXPECT_EQ(timed_out.error().backend, platform::WebSocketError::timed_out);
}

TEST_F(PushClientConnect, ReportsACancelledConnection) {
  websocket.error = platform::WebSocketError::cancelled;
  const auto cancelled = client.connect(token, fqdn, {});
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind, PushErrorKind::cancelled);
}

TEST_F(PushClientConnect, RefusesBadInputOrAStoppedStartWithoutConnecting) {
  EXPECT_EQ(
      client
          .connect(libchiaking::psn::auth::BearerToken{std::string_view{}},
                   fqdn, {})
          .error()
          .kind,
      PushErrorKind::invalid_token);
  EXPECT_EQ(client.connect(token, "push.example.test/evil", {}).error().kind,
            PushErrorKind::invalid_fqdn);
  const auto stopped =
      platform::StopToken{std::make_shared<test_support::Stopped>()};
  EXPECT_EQ(client.connect(token, fqdn, stopped).error().kind,
            PushErrorKind::cancelled);
  EXPECT_EQ(websocket.calls, 0U);
}

class PushClientReceive : public ::testing::Test {
 protected:
  session_client_rig::ScriptedWebSocket websocket;
  test_support::FakeLog log;
  QueuedPushConnection connection;
};

TEST_F(PushClientReceive, TypesEveryMessageAndCallsTheRestUnknown) {
  psn_remote_play::PushClient client(websocket);
  connection.add(
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","sessionId":"01234567-89ab-cdef-0123-456789abcdef","accountId":123456789})");
  connection.add(
      platform::WebSocketMessageType::binary,
      R"({"dataType":"psn:sessionManager:sys:rps:members:created","sessionId":"01234567-89ab-cdef-0123-456789abcdef","accountId":123456789})");
  connection.add(
      R"({"dataType":"psn:sessionManager:sys:unknown:thing","sessionId":"x"})");
  connection.add("{");
  connection.add(platform::WebSocketMessageType::binary, "{}");
  connection.add(std::string(16 * 1024 + 1, ' '));
  for (const auto expected :
       {Type::remote_play_session_created, Type::members_created, Type::unknown,
        Type::unknown, Type::unknown, Type::unknown}) {
    const auto received = client.receive_notification(connection, {});
    ASSERT_TRUE(received);
    EXPECT_EQ(received.value().type(), expected);
  }
  EXPECT_EQ(connection.receive_calls, 6U);
}

TEST_F(PushClientReceive, ReportsAFailedOrCancelledReceive) {
  psn_remote_play::PushClient client(websocket);
  connection.error = platform::WebSocketError::timed_out;
  const auto timed_out = client.receive_notification(connection, {});
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error().kind, PushErrorKind::websocket_failure);
  EXPECT_EQ(timed_out.error().backend, platform::WebSocketError::timed_out);
  connection.error = platform::WebSocketError::cancelled;
  const auto cancelled = client.receive_notification(connection, {});
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind, PushErrorKind::cancelled);
  EXPECT_EQ(connection.receive_calls, 2U);
}

TEST_F(PushClientReceive, RefusesAStoppedStartWithoutReceiving) {
  psn_remote_play::PushClient client(websocket);
  const auto stopped =
      platform::StopToken{std::make_shared<test_support::Stopped>()};
  const auto prestop = client.receive_notification(connection, stopped);
  ASSERT_FALSE(prestop);
  EXPECT_EQ(prestop.error().kind, PushErrorKind::cancelled);
  EXPECT_EQ(connection.receive_calls, 0U);
}

TEST_F(PushClientReceive, QueuesANotificationForTheStageThatWantsIt) {
  psn_remote_play::PushClient client(websocket, &log);
  connection.add(
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","secret":"SYNTHETIC_SECRET_DO_NOT_LOG"})");
  connection.add(
      R"({"dataType":"psn:sessionManager:sys:rps:members:created","secret":"SYNTHETIC_SECRET_DO_NOT_LOG"})");
  const std::array accepted{Type::members_created};
  auto member = client.receive_notification(connection, {}, accepted);
  ASSERT_TRUE(member);
  EXPECT_EQ(member.value().type(), accepted[0]);
  ASSERT_EQ(log.messages.size(), 2U);
  EXPECT_NE(log.messages[0].find("queued_for_another_stage"),
            std::string::npos);
  auto queued = client.receive_notification(connection, {});
  EXPECT_TRUE(queued);
  EXPECT_EQ(connection.receive_calls, 2U);
  EXPECT_NE(log.messages.back().find("delivered_from_queue"),
            std::string::npos);
  for (const bool membership : {false, true})
    for (const bool data : {false, true})
      client.log_console_readiness(membership, data);
  EXPECT_NE(log.messages.back().find(
                "ready: console joined and session data received"),
            std::string::npos);
  for (const auto& record : log.messages)
    EXPECT_EQ(record.find("SYNTHETIC_SECRET_DO_NOT_LOG"), std::string::npos)
        << record;
}

TEST_F(PushClientReceive, DropsUnknownNotificationsInsteadOfQueueingThem) {
  psn_remote_play::PushClient client(websocket, &log);
  for (std::size_t index = 0; index < 100; ++index)
    connection.add(
        index % 2 == 0
            ? std::string_view{"{"}
            : std::string_view{
                  R"({"dataType":"psn:sessionManager:sys:unknown:thing"})"});
  connection.add(
      R"({"dataType":"psn:sessionManager:sys:rps:members:created"})");
  const std::array accepted{Type::members_created};
  auto member = client.receive_notification(connection, {}, accepted);
  ASSERT_TRUE(member);
  EXPECT_EQ(member.value().type(), accepted[0]);
  ASSERT_FALSE(log.messages.empty());
  EXPECT_NE(log.messages.front().find("ignored"), std::string::npos);
  QueuedPushConnection next;
  next.add(R"({"dataType":"psn:sessionManager:sys:rps:members:created"})");
  auto again = client.receive_notification(next, {}, accepted);
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value().type(), accepted[0]);
}

}  // namespace
}  // namespace play_test
