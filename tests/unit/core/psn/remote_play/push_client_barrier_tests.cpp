#include "tests/unit/core/psn/remote_play/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::net::RemoteWaitDeadline;
using libchiaking::platform::StopToken;
using libchiaking::psn::remote_play::ConsoleReady;
using libchiaking::psn::remote_play::ConsoleReadyBarrier;
using libchiaking::psn::remote_play::CreationBarrier;
using libchiaking::psn::remote_play::CreationWaitError;
using libchiaking::psn::remote_play::PushClient;
using libchiaking::psn::remote_play::PushErrorKind;
using Ending = ScriptedConnection::Ending;
using Script = std::vector<std::string_view>;

class PushClientBarrier : public ::testing::Test {
 protected:
  static constexpr std::string_view created =
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","to":{"onlineId":"userA"}})";
  static constexpr std::string_view member =
      R"({"dataType":"psn:sessionManager:sys:rps:members:created"})";
  static constexpr std::string_view members_created =
      R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{"data":{"members":[{"deviceUniqueId":"0000000000000000000000000000000000000000000000000000000000000000"}]}}})";
  static constexpr std::string_view custom_updated =
      R"({"dataType":"psn:sessionManager:sys:rps:customData1:updated","body":{"data":{"customData1":"QUFBQUFBQUFBQUFBQUFBQUFBQUFBQT09"}}})";

  PushClientBarrier() {
    connection.clock = &clock;
    connection.deadline = 200;
  }

  void ends_with(Ending ending) {
    connection.ending = ending;
    connection.parent = stop_state.get();
  }

  auto awaits_creation() {
    CreationBarrier barrier{7};
    return client.await_creation(barrier, 7, connection, StopToken{stop_state},
                                 clock, 200);
  }

  auto awaits_console(std::optional<std::array<std::byte, 32>> uid,
                      RemoteWaitDeadline deadline = RemoteWaitDeadline{200}) {
    ConsoleReadyBarrier barrier{7, uid};
    return client.await_console_ready(barrier, 7, connection,
                                      StopToken{stop_state}, clock, deadline);
  }

  void console_ready_after(Script script) {
    connection.script = std::move(script);
    const ConsoleReady zero{};
    const auto ready = awaits_console(std::array<std::byte, 32>{});
    ASSERT_TRUE(ready);
    EXPECT_EQ(ready.value().device_uid, zero.device_uid);
    EXPECT_EQ(ready.value().custom_data1, zero.custom_data1);
    EXPECT_EQ(connection.calls, 2);
  }

  test_support::RefusingWebSocket provider{
      libchiaking::platform::WebSocketError::unsupported};
  PushClient client{provider};
  test_support::FakeClock clock{100};
  ScriptedConnection connection;
  std::shared_ptr<test_support::StopState> stop_state =
      std::make_shared<test_support::StopState>();
};

TEST_F(PushClientBarrier, CreationSkipsUnreadableAndUntypedNotifications) {
  connection.script = {"{", created, R"({"noType":1})", member};
  const auto ready = awaits_creation();
  ASSERT_TRUE(ready);
  EXPECT_EQ(ready.value(), "userA");
  EXPECT_EQ(connection.calls, 4);
}

TEST_F(PushClientBarrier, CreationAcceptsTheMemberBeforeTheSession) {
  connection.script = {member, "{", created};
  const auto ready = awaits_creation();
  ASSERT_TRUE(ready);
  EXPECT_EQ(ready.value(), "userA");
  EXPECT_EQ(connection.calls, 3);
}

TEST_F(PushClientBarrier, CreationTimesOutAtTheDeadline) {
  ends_with(Ending::expire_cancelled);
  const auto outcome = awaits_creation();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().kind, CreationWaitError::timed_out);
  EXPECT_EQ(connection.calls, 1);
}

TEST_F(PushClientBarrier, CreationIsCancelledByTheParent) {
  ends_with(Ending::cancelled);
  const auto outcome = awaits_creation();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().kind, CreationWaitError::cancelled);
  EXPECT_EQ(connection.calls, 1);
}

TEST_F(PushClientBarrier, CreationReportsAReceiveFailure) {
  ends_with(Ending::invalid);
  const auto outcome = awaits_creation();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().kind, CreationWaitError::receive_failed);
  ASSERT_TRUE(outcome.error().push);
  EXPECT_EQ(outcome.error().push->kind, PushErrorKind::websocket_failure);
  EXPECT_EQ(connection.calls, 1);
}

TEST_F(PushClientBarrier, ConsoleIsReadyAfterTheMemberThenItsData) {
  console_ready_after({members_created, custom_updated});
}

TEST_F(PushClientBarrier, ConsoleIsReadyAfterItsDataThenTheMember) {
  console_ready_after({custom_updated, members_created});
}

TEST_F(PushClientBarrier, ConsoleReadinessSharesOneDeadline) {
  connection.advance = 120;
  connection.script = {members_created, custom_updated};
  const auto ready = awaits_console(std::array<std::byte, 32>{});
  ASSERT_FALSE(ready);
  EXPECT_EQ(ready.error().kind, CreationWaitError::timed_out);
}

TEST_F(PushClientBarrier, ConsoleReadinessCanGiveEachWaitItsOwnDeadline) {
  connection.advance = 120;
  connection.script = {members_created, custom_updated};
  const auto ready = awaits_console(std::array<std::byte, 32>{},
                                    RemoteWaitDeadline::per_wait(100));
  EXPECT_TRUE(ready);
  EXPECT_EQ(connection.calls, 2);
}

TEST_F(PushClientBarrier, ConsoleReadinessTimesOutAtTheDeadline) {
  ends_with(Ending::expire_cancelled);
  const auto outcome = awaits_console(std::nullopt);
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().kind, CreationWaitError::timed_out);
  EXPECT_EQ(connection.calls, 1);
}

TEST_F(PushClientBarrier, ConsoleReadinessIsCancelledByTheParent) {
  ends_with(Ending::cancelled);
  const auto outcome = awaits_console(std::nullopt);
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().kind, CreationWaitError::cancelled);
  EXPECT_EQ(connection.calls, 1);
}

TEST_F(PushClientBarrier, ConsoleReadinessReportsAReceiveFailure) {
  ends_with(Ending::invalid);
  const auto outcome = awaits_console(std::nullopt);
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().kind, CreationWaitError::receive_failed);
  EXPECT_EQ(connection.calls, 1);
}

}  // namespace
}  // namespace psn_test
