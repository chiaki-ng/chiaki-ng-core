#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/internet_link_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
namespace {

using internet_links::ScriptedRemote;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace script = remote_start_script;
using StreamState = remote_play::StreamState;

class InternetLinkStop : public internet_links::ScriptedInternetLinkTest {
 protected:
  static constexpr std::string_view unscoped_members_deleted =
      R"({"dataType":"psn:sessionManager:sys:rps:members:deleted","body":{"data":{"sessionId":"another-session"}}})";

  void SetUp() override {
    ScriptedInternetLinkTest::SetUp();
    if (IsSkipped()) return;
    rig_.http().start_status = 200;
    auto& counts = rig_.counts();
    for (int index = 0; index < 70; ++index)
      counts.after_release.push_back(script::ready_custom_body);
    counts.after_release.push_back(unscoped_members_deleted);
    counts.on_close = [this] { order_.emplace_back("close"); };
    rig_.http().on_request = [this](const platform::HttpRequest& request) {
      if (request.method == platform::HttpMethod::delete_)
        order_.emplace_back("release");
    };
    rig_.crypto().allow_registration_request = true;
    rig_.answer_control_route();
  }

  void start() {
    link_ = rig_.open(ScriptedRemote<>::configuration());
    ASSERT_NE(link_, nullptr);
    ASSERT_TRUE(link_->start());
    EXPECT_EQ(link_->state(), StreamState::establishing_route);
    for (const std::string_view operation :
         {"psn_open", "registration", "session_init", "ctrl_startup"}) {
      SCOPED_TRACE(operation);
      const auto prefix = "operation=" + std::string{operation};
      EXPECT_EQ(rig_.diagnostics().count(prefix + " outcome=begin"), 1);
      EXPECT_EQ(rig_.diagnostics().count(prefix + " outcome=succeeded"), 1);
    }
  }

  void lose_the_push_channel() {
    rig_.counts().fail_live = true;
    for (int index = 0; index < 5000 && rig_.counts().live_failures.load() == 0;
         ++index)
      LiveProviders::nap(std::chrono::milliseconds{1});
    EXPECT_EQ(rig_.counts().live_failures.load(), 1U);
  }

  void expect_stopped(bool stopped) {
    EXPECT_TRUE(stopped);
    EXPECT_EQ(link_->state(), StreamState::stopped);
    EXPECT_FALSE(link_->cleanup_failure());
    EXPECT_EQ(rig_.counts().close, 1U);
  }

  void expect_membership_released() {
    EXPECT_EQ(order_, (std::vector<std::string>{"release", "close"}));
    EXPECT_EQ(rig_.counts().after_release_index,
              rig_.counts().after_release.size());
    EXPECT_TRUE(rig_.diagnostics().contains(
        "remote stop: current PSN membership deletion observed"));
  }

  void expect_membership_kept() {
    EXPECT_EQ(order_, (std::vector<std::string>{"close"}));
    EXPECT_EQ(rig_.counts().after_release_index, 0U);
  }

  void expect_stays_stopped() {
    EXPECT_TRUE(link_->stop());
    EXPECT_EQ(rig_.counts().close, 1U);
  }

  auto rig() -> ScriptedRemote<>& { return rig_; }
  auto link() -> play::Link& { return *link_; }

 private:
  std::vector<std::string> order_;
  ScriptedRemote<> rig_{{script::created_body, script::members_body,
                         script::ready_members_body, script::ready_custom_body,
                         script::route_offer_body, script::route_result_body,
                         script::route_accept_body}};
  play::Link* link_{};
};

TEST_F(InternetLinkStop, ReleasesTheMembershipBeforeClosingThePush) {
  ASSERT_NO_FATAL_FAILURE(start());
  expect_stopped(static_cast<bool>(link().stop()));
  expect_membership_released();
  expect_stays_stopped();
}

TEST_F(InternetLinkStop, KeepsTheMembershipWhenStoppingForSleep) {
  ASSERT_NO_FATAL_FAILURE(start());
  expect_stopped(static_cast<bool>(link().stop_for_sleep()));
  expect_membership_kept();
  expect_stays_stopped();
}

TEST_F(InternetLinkStop, StopsCleanlyWhenTheMembershipDeleteFails) {
  rig().http().release_status = 500;
  ASSERT_NO_FATAL_FAILURE(start());
  expect_stopped(static_cast<bool>(link().stop()));
  expect_membership_released();
  expect_stays_stopped();
}

TEST_F(InternetLinkStop, KeepsTheMembershipOnceThePushChannelIsLost) {
  ASSERT_NO_FATAL_FAILURE(start());
  lose_the_push_channel();
  expect_stopped(static_cast<bool>(link().stop()));
  expect_membership_kept();
  expect_stays_stopped();
}

class InternetLinkDataRouteStart
    : public internet_links::ScriptedInternetLinkTest {
 protected:
  static auto full_script() -> std::vector<std::string_view> {
    return {script::created_body,        script::members_body,
            script::ready_members_body,  script::ready_custom_body,
            script::route_offer_body,    script::route_result_body,
            script::route_accept_body,   script::data_offer_body,
            script::data_result_body,    script::data_accept_body,
            script::members_deleted_body};
  }

  static auto reach_the_data_route(ScriptedRemote<>& rig) -> play::Link* {
    rig.http().start_status = 200;
    rig.crypto().allow_registration_request = true;
    rig.crypto().allow_stream_prepare = true;
    rig.answer_control_route();
    rig.sockets().allow_idle_poll = true;
    auto* link = rig.open(ScriptedRemote<>::configuration());
    EXPECT_NE(link, nullptr);
    if (!link) return nullptr;
    EXPECT_TRUE(link->start());
    rig.clock().now = 10'000'100;
    EXPECT_TRUE(link->pump_once());
    EXPECT_EQ(rig.sockets().creates, 2U);
    return link;
  }
};

TEST_F(InternetLinkDataRouteStart, ToleratesLosingThePushAfterTheDataAccept) {
  ScriptedRemote<> rig{full_script()};
  auto* link = reach_the_data_route(rig);
  ASSERT_NE(link, nullptr);
  rig.counts().fail_live = true;
  for (int index = 0; index < 5000 && rig.counts().live_failures.load() == 0;
       ++index)
    LiveProviders::nap(std::chrono::milliseconds{1});
  EXPECT_EQ(rig.counts().live_failures.load(), 1U);
  LiveProviders::nap(std::chrono::milliseconds{20});
  EXPECT_TRUE(link->pump_once());
  EXPECT_EQ(link->state(), StreamState::establishing_route);
  EXPECT_FALSE(link->failure());
  EXPECT_TRUE(rig.diagnostics().contains("PSN push lost"));
  EXPECT_TRUE(link->stop());
  EXPECT_EQ(rig.requests("release"), 0);
}

TEST_F(InternetLinkDataRouteStart, DrainsLateDataDatagramsBeforeTakion) {
  ScriptedRemote<> rig{full_script()};
  auto& sockets = rig.sockets();
  sockets.respond_to_takion_init = true;
  sockets.respond_to_takion_cookie = true;
  sockets.respond_to_takion_data = true;
  sockets.respond_to_takion_big = true;
  sockets.respond_to_switch_request = true;
  auto* link = reach_the_data_route(rig);
  ASSERT_NE(link, nullptr);
  ASSERT_NE(sockets.last, nullptr);
  auto& data_socket = *sockets.last;
  std::vector<std::byte> late_request(88, std::byte{});
  late_request[0] = std::byte{0x06};
  data_socket.queue_raw(std::move(late_request));
  const auto sends_before = data_socket.send_calls;
  rig.clock().now = 15'000'100;
  EXPECT_TRUE(link->pump_once());
  EXPECT_GE(sockets.takion_init_sends, 1U);
  ASSERT_TRUE(data_socket.raw_consumed_at);
  EXPECT_EQ(*data_socket.raw_consumed_at, sends_before);
  EXPECT_TRUE(link->stop());
}

}  // namespace
}  // namespace play_test
