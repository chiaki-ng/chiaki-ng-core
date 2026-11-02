#include "tests/unit/core/psn/remote_play/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::platform::StopToken;
using libchiaking::psn::remote_play::ConsoleReadyBarrier;
using libchiaking::psn::remote_play::PushClient;
using libchiaking::psn::remote_play::PushErrorKind;
using libchiaking::psn::rest::Notification;
using libchiaking::psn::rest::NotificationType;
using Script = std::vector<std::string_view>;

class PushClientQueue : public ::testing::Test {
 protected:
  static constexpr std::string_view members_created =
      R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{"data":{"members":[{"deviceUniqueId":"0000000000000000000000000000000000000000000000000000000000000000"}]}}})";
  static constexpr std::string_view custom_updated =
      R"({"dataType":"psn:sessionManager:sys:rps:customData1:updated","body":{"data":{"customData1":"QUFBQUFBQUFBQUFBQUFBQUFBQUFBQT09"}}})";
  static constexpr std::string_view offer =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"offer":"synthetic"}})";
  static constexpr std::string_view unrelated = R"({"dataType":"unrelated"})";
  static constexpr std::array session_messages{
      NotificationType::session_message_created};

  void keeps_offers_skipped_while_waiting(Script script) {
    connection.clock = &clock;
    connection.deadline = 200;
    connection.script = std::move(script);
    ConsoleReadyBarrier barrier{7, std::array<std::byte, 32>{}};
    const auto ready =
        client.await_console_ready(barrier, 7, connection, {}, clock, 200);
    EXPECT_TRUE(ready);
    EXPECT_EQ(connection.calls, 5);
    auto stopped = std::make_shared<test_support::StopState>();
    stopped->stopped = true;
    const auto cancelled = client.receive_notification(
        connection, StopToken{stopped}, session_messages);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().kind, PushErrorKind::cancelled);
    for (int duplicate = 0; duplicate < 2; ++duplicate) {
      const auto retained =
          client.receive_notification(connection, {}, session_messages);
      ASSERT_TRUE(retained) << duplicate;
      EXPECT_EQ(retained.value().body(), offer);
      EXPECT_EQ(connection.calls, 5);
    }
    const auto exhausted = client.receive_notification(connection, {});
    EXPECT_FALSE(exhausted);
    EXPECT_EQ(connection.calls, 6);
  }

  test_support::RefusingWebSocket provider{
      libchiaking::platform::WebSocketError::unsupported};
  PushClient client{provider};
  test_support::FakeClock clock{100};
  ScriptedConnection connection;
};

TEST_F(PushClientQueue, KeepsOffersSkippedAfterTheMember) {
  keeps_offers_skipped_while_waiting(
      {members_created, offer, unrelated, offer, custom_updated});
}

TEST_F(PushClientQueue, KeepsOffersSkippedAfterTheData) {
  keeps_offers_skipped_while_waiting(
      {custom_updated, offer, unrelated, offer, members_created});
}

TEST_F(PushClientQueue, FillsAtSixtyFourHeldNotifications) {
  connection.script.assign(65, offer);
  constexpr std::array accepted{NotificationType::members_created};
  const auto overflow = client.receive_notification(connection, {}, accepted);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().kind, PushErrorKind::notification_queue_full);
  EXPECT_EQ(connection.calls, 65);
  ScriptedConnection foreign;
  const auto wrong = client.receive_notification(foreign, {});
  ASSERT_FALSE(wrong);
  EXPECT_EQ(wrong.error().kind, PushErrorKind::connection_mismatch);
  EXPECT_EQ(foreign.calls, 0);
  for (int index = 0; index < 64; ++index) {
    const auto retained = client.receive_notification(connection, {});
    ASSERT_TRUE(retained) << index;
    EXPECT_EQ(retained.value().body(), offer);
  }
}

TEST(PushClientSession, TakesAnIdlessNotificationUnlessAnIdIsRequired) {
  const auto missing = Notification::parse(
      R"({"dataType":"psn:sessionManager:sys:rps:members:deleted"})");
  EXPECT_TRUE(
      PushClient::notification_in_session(missing, ServiceRig<>::session_id));
  EXPECT_FALSE(PushClient::notification_in_session(
      missing, ServiceRig<>::session_id, true));
}

}  // namespace
}  // namespace psn_test
