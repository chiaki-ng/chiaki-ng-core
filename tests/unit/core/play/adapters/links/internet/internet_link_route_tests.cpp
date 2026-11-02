#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/internet_link_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_test {
namespace {

using internet_links::FailNetwork;
using internet_links::ScriptedRemote;
using internet_links::Target;
namespace holepunch = libchiaking::net::nat::holepunch;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace script = remote_start_script;
using StreamError = remote_play::StreamError;
using StreamState = remote_play::StreamState;

class SocketFailureTest : public internet_links::ScriptedInternetLinkTest {
 protected:
  static auto script_until_ready() -> std::vector<std::string_view> {
    return {script::created_body, script::members_body,
            script::ready_members_body, script::ready_custom_body};
  }

  static auto requests_until_release() -> std::vector<std::string> {
    return {"fqdn", "create", "view", "view", "release"};
  }
};

class InternetLinkSocketFailure : public SocketFailureTest,
                                  public ::testing::WithParamInterface<bool> {};

class InternetLinkPs4SocketFailure : public SocketFailureTest {};

TEST_P(InternetLinkSocketFailure, FailsTheControlHolepunchAndReleases) {
  const bool fail_cleanup = GetParam();
  ScriptedRemote<FailNetwork> rig{script_until_ready()};
  rig.http().start_status = 200;
  rig.http().release_status = fail_cleanup ? 500 : 204;
  auto* link = rig.open(ScriptedRemote<FailNetwork>::configuration());
  ASSERT_NE(link, nullptr);
  const auto result = link->start();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), StreamError::route_failed);
  EXPECT_EQ(link->state(), StreamState::failed);
  EXPECT_EQ(rig.http().order, requests_until_release());
  EXPECT_EQ(rig.counts().connect, 1U);
  EXPECT_GE(rig.counts().receive, 2U);
  EXPECT_EQ(rig.counts().close, 1U);
  EXPECT_EQ(rig.network().udp_calls(), 1U);
  ASSERT_TRUE(link->failure());
  const auto retained = *link->failure();
  EXPECT_EQ(retained.route(), play::Venue::internet());
  EXPECT_EQ(retained.stage(), remote_play::FailureStage::control_holepunch);
  const auto* route = retained.as<play::RouteFailure>();
  ASSERT_TRUE(route);
  const auto* sockets = route->as<holepunch::BoundSocketsError>();
  ASSERT_TRUE(sockets);
  EXPECT_EQ(*sockets, holepunch::BoundSocketsError::ipv4_create);
  EXPECT_FALSE(link->cleanup_failure());
  EXPECT_EQ(
      rig.diagnostics().contains("remote stop: PSN membership DELETE failed"),
      fail_cleanup);
  EXPECT_TRUE(link->stop());
  ASSERT_TRUE(link->failure());
  EXPECT_EQ(link->failure()->describe(), retained.describe());
  EXPECT_EQ(link->failure()->stage(), retained.stage());
  EXPECT_GE(rig.heard().events, 1U);
  bool found = false;
  for (const auto& message : rig.diagnostics().messages()) {
    if (message ==
        "remote start: control route hole punch failed: the local UDP "
        "socket could not be created")
      found = true;
    EXPECT_EQ(message.find("SYNTHETIC_TEST_TOKEN"), std::string::npos);
  }
  EXPECT_TRUE(found);
}

INSTANTIATE_TEST_SUITE_P(ReleasedOrNot, InternetLinkSocketFailure,
                         ::testing::Bool());

TEST_F(InternetLinkPs4SocketFailure, FailsTheControlHolepunchAndReleases) {
  ScriptedRemote<FailNetwork> rig{script_until_ready()};
  auto* link =
      rig.open(ScriptedRemote<FailNetwork>::configuration(Target::ps4_10));
  ASSERT_NE(link, nullptr);
  const auto result = link->start();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), StreamError::route_failed);
  EXPECT_EQ(link->state(), StreamState::failed);
  EXPECT_EQ(rig.http().order, requests_until_release());
  EXPECT_EQ(rig.counts().connect, 1U);
  EXPECT_GE(rig.counts().receive, 2U);
  EXPECT_EQ(rig.counts().close, 1U);
  EXPECT_EQ(rig.network().udp_calls(), 1U);
  EXPECT_EQ(rig.heard().events, 1U);
  EXPECT_TRUE(
      std::holds_alternative<remote_play::event::Failed>(rig.heard().last));
}

class InternetLinkControlRoute
    : public internet_links::ScriptedInternetLinkTest {
 protected:
  static auto control_script(std::string_view offer = script::route_offer_body)
      -> std::vector<std::string_view> {
    return {script::created_body,
            script::members_body,
            script::ready_members_body,
            script::ready_custom_body,
            offer,
            script::route_result_body,
            script::route_accept_body};
  }

  static auto control_requests() -> std::vector<std::string> {
    return {"fqdn",      "create", "view",    "view",    "stun",
            "stun_ipv6", "start",  "status",  "status",  "message",
            "message",   "view",   "message", "message", "release"};
  }

  static void expect_one_failure(ScriptedRemote<>& rig) {
    EXPECT_GE(rig.counts().receive, 8U);
    EXPECT_EQ(rig.counts().connect, 1U);
    EXPECT_EQ(rig.counts().close, 1U);
    EXPECT_EQ(rig.http().order, control_requests());
    EXPECT_EQ(rig.heard().events, 1U);
    EXPECT_TRUE(
        std::holds_alternative<remote_play::event::Failed>(rig.heard().last));
  }

  static void expect_rudp_inits_advance(
      const public_route_fixture::RouteSockets& sockets) {
    const auto& inits = sockets.rudp_init_requests;
    ASSERT_EQ(inits.size(), 3U);
    for (const auto& init : inits) {
      ASSERT_GE(init.size(), 12U);
      EXPECT_TRUE(std::equal(init.begin() + 8, init.begin() + 12,
                             inits.front().begin() + 8));
    }
    const auto counter = [](const std::vector<std::byte>& payload) {
      return static_cast<unsigned>(
          (std::to_integer<unsigned>(payload[0]) << 8U) |
          std::to_integer<unsigned>(payload[1]));
    };
    EXPECT_EQ(counter(inits[0]), 0x1FFU);
    EXPECT_GT(counter(inits[1]), counter(inits[0]) + 2);
    EXPECT_GT(counter(inits[2]), counter(inits[1]) + 2);
  }

  static void expect_route_held_until_stop(ScriptedRemote<>& rig,
                                           play::Link& link) {
    auto& sockets = rig.sockets();
    EXPECT_GE(sockets.ack_sends, 2U);
    EXPECT_EQ(rig.heard().events, 1U);
    EXPECT_TRUE(std::holds_alternative<remote_play::event::StateChanged>(
        rig.heard().last));
    EXPECT_EQ(sockets.closes, 0U);
    EXPECT_EQ(rig.counts().close, 0U);
    EXPECT_TRUE(link.stop());
    EXPECT_EQ(link.state(), StreamState::stopped);
    EXPECT_EQ(rig.counts().close, 1U);
    EXPECT_EQ(sockets.closes, 1U);
    EXPECT_EQ(rig.http().order, control_requests());
  }

  static auto failed_start(ScriptedRemote<>& rig, StreamError error)
      -> play::Link* {
    rig.http().start_status = 200;
    auto config = ScriptedRemote<>::configuration();
    config.enable_ipv6 = false;
    auto* link = rig.open(std::move(config));
    EXPECT_NE(link, nullptr);
    if (!link) return nullptr;
    const auto result = link->start();
    EXPECT_FALSE(result);
    if (!result) {
      EXPECT_EQ(result.error(), error);
    }
    EXPECT_EQ(link->state(), StreamState::failed);
    return link;
  }
};

TEST_F(InternetLinkControlRoute, ProbesBothAddressFamiliesWithIpv6Enabled) {
  ScriptedRemote<> rig{control_script()};
  rig.http().start_status = 200;
  auto config = ScriptedRemote<>::configuration();
  config.enable_ipv6 = true;
  auto* link = rig.open(std::move(config));
  ASSERT_NE(link, nullptr);
  const auto hmac_before = rig.crypto().hmac_calls;
  const auto result = link->start();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), StreamError::route_failed);
  EXPECT_EQ(link->state(), StreamState::failed);
  const auto& sockets = rig.sockets();
  EXPECT_EQ(sockets.ipv6_creates, 1U);
  EXPECT_EQ(sockets.creates, 2U);
  EXPECT_EQ(sockets.closes, 2U);
  EXPECT_GE(sockets.probe_sends, 1U);
  EXPECT_EQ(rig.crypto().hmac_calls, hmac_before + 1);
  expect_one_failure(rig);
}

TEST_F(InternetLinkControlRoute, FailsWhenTheConsoleNeverAnswersRudpInit) {
  ScriptedRemote<> rig{control_script()};
  rig.crypto().allow_registration_request = true;
  ASSERT_NE(failed_start(rig, StreamError::route_failed), nullptr);
  const auto& sockets = rig.sockets();
  EXPECT_EQ(sockets.creates, 1U);
  EXPECT_EQ(sockets.closes, 1U);
  EXPECT_GE(sockets.probe_sends, 1U);
  EXPECT_GE(sockets.rudp_sends, 1U);
  expect_one_failure(rig);
}

TEST_F(InternetLinkControlRoute, FailsWhenTheConsoleNeverAnswersTheCookie) {
  ScriptedRemote<> rig{control_script()};
  rig.crypto().allow_registration_request = true;
  rig.sockets().respond_to_init = true;
  ASSERT_NE(failed_start(rig, StreamError::route_failed), nullptr);
  const auto& sockets = rig.sockets();
  EXPECT_EQ(sockets.creates, 1U);
  EXPECT_EQ(sockets.closes, 1U);
  EXPECT_GE(sockets.probe_sends, 1U);
  EXPECT_GE(sockets.rudp_sends, 1U);
  EXPECT_GE(sockets.cookie_sends, 1U);
  expect_one_failure(rig);
}

TEST_F(InternetLinkControlRoute, FailsWhenTheConsoleNeverAnswersRegistration) {
  ScriptedRemote<> rig{control_script()};
  rig.crypto().allow_registration_request = true;
  rig.sockets().respond_to_init = true;
  rig.sockets().respond_to_cookie = true;
  ASSERT_NE(failed_start(rig, StreamError::route_failed), nullptr);
  const auto& sockets = rig.sockets();
  EXPECT_EQ(sockets.creates, 1U);
  EXPECT_EQ(sockets.closes, 1U);
  EXPECT_GE(sockets.rudp_sends, 1U);
  EXPECT_GE(sockets.cookie_sends, 1U);
  EXPECT_GE(sockets.registration_http_sends, 1U);
  expect_one_failure(rig);
}

TEST_F(InternetLinkControlRoute, FailsCtrlStartupWhenTheConsoleNeverAnswers) {
  ScriptedRemote<> rig{control_script()};
  rig.crypto().allow_registration_request = true;
  rig.sockets().respond_to_init = true;
  rig.sockets().respond_to_cookie = true;
  rig.sockets().respond_to_registration = true;
  auto* link = failed_start(rig, StreamError::ctrl_network_failure);
  ASSERT_NE(link, nullptr);
  ASSERT_TRUE(link->failure());
  EXPECT_EQ(link->failure()->stage(), remote_play::FailureStage::ctrl_startup);
  const auto* connect =
      link->failure()
          ->as<libchiaking::play::internet::remote_ctrl::ConnectFailure>();
  ASSERT_TRUE(connect);
  ASSERT_TRUE(connect->startup);
  EXPECT_EQ(connect->startup->cause,
            static_cast<std::uint32_t>(
                remote_play::RudpCtrlHttpExchangeError::receive_failed));
  const auto& sockets = rig.sockets();
  EXPECT_EQ(sockets.creates, 1U);
  EXPECT_EQ(sockets.closes, 1U);
  EXPECT_GE(sockets.registration_http_sends, 1U);
  EXPECT_GE(sockets.ack_sends, 1U);
  expect_one_failure(rig);
}

TEST_F(InternetLinkControlRoute, HoldsTheEstablishedControlRouteUntilStopped) {
  ScriptedRemote<> rig{control_script()};
  rig.http().start_status = 200;
  rig.crypto().allow_registration_request = true;
  rig.answer_control_route();
  auto config = ScriptedRemote<>::configuration();
  config.enable_ipv6 = false;
  auto* link = rig.open(std::move(config));
  ASSERT_NE(link, nullptr);
  ASSERT_TRUE(link->start());
  EXPECT_EQ(link->state(), StreamState::establishing_route);
  const auto& sockets = rig.sockets();
  EXPECT_GE(sockets.ctrl_http_sends, 1U);
  EXPECT_EQ(rig.heard().registered_events, 0U);
  EXPECT_GE(sockets.rudp_sends, 3U);
  EXPECT_GE(sockets.cookie_sends, 3U);
  expect_rudp_inits_advance(sockets);
  expect_route_held_until_stop(rig, *link);
}

TEST_F(InternetLinkControlRoute, ConnectsToALocalOrStaticPeerThroughRefusals) {
  for (const auto offer :
       {script::lan_route_offer_body, script::static_route_offer_body}) {
    SCOPED_TRACE(offer);
    ScriptedRemote<> rig{control_script(offer)};
    rig.http().start_status = 200;
    rig.crypto().allow_registration_request = true;
    rig.answer_control_route();
    auto& sockets = rig.sockets();
    sockets.connect_peer = true;
    sockets.refuse_rudp_init = 3;
    sockets.refusals_per_init = 3;
    auto config = ScriptedRemote<>::configuration();
    config.enable_ipv6 = false;
    auto* link = rig.open(std::move(config));
    ASSERT_NE(link, nullptr);
    ASSERT_TRUE(link->start());
    EXPECT_EQ(link->state(), StreamState::establishing_route);
    EXPECT_GE(sockets.ctrl_http_sends, 1U);
    ASSERT_NE(sockets.first, nullptr);
    ASSERT_TRUE(sockets.first->connected_peer);
    EXPECT_EQ(
        *sockets.first->connected_peer,
        platform::SocketEndpoint(platform::IpAddress::loopback_ipv4(), 9999));
    EXPECT_GE(sockets.first->connected_sends, 3U);
    EXPECT_EQ(sockets.first->pending_refusals, 0U);
    EXPECT_EQ(rig.heard().registered_events, 0U);
    EXPECT_GE(sockets.rudp_sends, 3U);
    EXPECT_GE(sockets.cookie_sends, 3U);
    expect_rudp_inits_advance(sockets);
    expect_route_held_until_stop(rig, *link);
  }
}

TEST_F(InternetLinkControlRoute, FailsTheDataRouteWhenNoDataOfferArrives) {
  ScriptedRemote<> rig{control_script()};
  rig.http().start_status = 200;
  rig.crypto().allow_registration_request = true;
  rig.answer_control_route();
  rig.sockets().queue_session_id = true;
  auto config = ScriptedRemote<>::configuration();
  config.enable_ipv6 = false;
  auto* link = rig.open(std::move(config));
  ASSERT_NE(link, nullptr);
  ASSERT_TRUE(link->start());
  const auto pumped = link->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), StreamError::route_failed);
  EXPECT_EQ(link->state(), StreamState::failed);
  const auto& sockets = rig.sockets();
  EXPECT_EQ(sockets.creates, 2U);
  EXPECT_EQ(sockets.closes, 2U);
  EXPECT_EQ(sockets.session_id_queued, 1U);
  EXPECT_EQ(rig.counts().connect, 1U);
  EXPECT_GE(rig.counts().receive, 9U);
  EXPECT_EQ(rig.counts().close, 1U);
  EXPECT_EQ(rig.requests("stun"), 1);
  EXPECT_EQ(rig.requests("message"), 4);
  EXPECT_EQ(rig.requests("release"), 0);
  EXPECT_EQ(rig.heard().events, 2U);
  EXPECT_TRUE(
      std::holds_alternative<remote_play::event::Failed>(rig.heard().last));
}

}  // namespace
}  // namespace play_test
