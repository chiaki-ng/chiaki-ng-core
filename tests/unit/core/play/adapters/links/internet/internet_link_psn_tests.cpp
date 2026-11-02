#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/internet_link_rig.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::SignedInPlayer;
namespace {

using internet_links::ScriptedRemote;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace script = remote_start_script;
using Stage = remote_play::FailureStage;

class InternetLinkPsn : public internet_links::ScriptedInternetLinkTest {};

TEST_F(InternetLinkPsn, ServicesThePushChannelFromPsnOpen) {
  bool serviced_during_stun = false;
  ScriptedRemote<> rig{{script::created_body, script::members_body,
                        script::ready_members_body, script::ready_custom_body}};
  rig.network().dns_.on_stun = [&] {
    const auto limit = LiveProviders::steady_us() + 2'000'000;
    while (rig.counts().receive.load() < 4 &&
           LiveProviders::steady_us() < limit)
      LiveProviders::nap(std::chrono::milliseconds{1});
    serviced_during_stun = rig.counts().receive.load() >= 4;
  };
  auto* link = rig.open(ScriptedRemote<>::configuration());
  ASSERT_NE(link, nullptr);
  EXPECT_FALSE(link->start());
  EXPECT_TRUE(serviced_during_stun);
  ASSERT_TRUE(link->failure());
  EXPECT_EQ(link->failure()->stage(), Stage::psn_start);
  EXPECT_EQ(link->failure()->http_status(), 401);
}

class InternetLinkPsnDeadline
    : public internet_links::ScriptedInternetLinkTest,
      public ::testing::WithParamInterface<std::tuple<bool, bool>> {};

TEST_P(InternetLinkPsnDeadline, GivesUpOnAMissingNotificationAndReleases) {
  const auto [start_phase, cancel] = GetParam();
  auto source = LiveProviders::threading().create_stop_source();
  ASSERT_TRUE(source);
  std::atomic<bool> waiting_stage{!start_phase};
  ScriptedRemote<> rig{
      start_phase ? std::vector<std::string_view>{script::created_body,
                                                  script::members_body,
                                                  script::ready_members_body}
                  : std::vector<std::string_view>{script::created_body}};
  rig.http().start_status = 200;
  rig.counts().progress.reset();
  rig.http().on_request = [&](const platform::HttpRequest& request) {
    if (request.url.view().find("/commands") != std::string::npos)
      waiting_stage = true;
  };
  rig.counts().on_exhaustion = [&] {
    if (!waiting_stage) return;
    rig.clock().now += 1100;
    if (cancel) static_cast<void>(source.value()->request_stop());
  };
  play::InternetLinkConfiguration config;
  config.user = SignedInPlayer::fresh_for_remote_play();
  config.console.target = remote_play::RegistrationTarget::ps5_1;
  config.start_timeout_us = 1000;
  config.stop = source.value()->token();
  auto* link = rig.open(std::move(config));
  ASSERT_NE(link, nullptr);
  EXPECT_FALSE(link->start());
  ASSERT_TRUE(link->failure());
  const auto& failure = *link->failure();
  EXPECT_EQ(failure.stage(), start_phase ? Stage::psn_start : Stage::psn_open);
  EXPECT_EQ(failure.cancelled(), cancel);
  if (!cancel) {
    EXPECT_NE(failure.sentence().find("expected PSN notification did not "
                                      "arrive before the deadline"),
              std::string::npos);
  }
  EXPECT_EQ(rig.requests("release"), 1);
  EXPECT_EQ(rig.counts().close, 1U);
}

INSTANTIATE_TEST_SUITE_P(OpenAndStartWaits, InternetLinkPsnDeadline,
                         ::testing::Combine(::testing::Bool(),
                                            ::testing::Bool()));

class InternetLinkPsnRefusal : public internet_links::ScriptedInternetLinkTest,
                               public ::testing::WithParamInterface<bool> {};

TEST_P(InternetLinkPsnRefusal, FailsAtPsnStartAndReleasesTheSession) {
  const bool fail_release = GetParam();
  ScriptedRemote<> rig{{script::created_body, script::members_body}};
  rig.http().release_status = fail_release ? 500 : 204;
  auto* link = rig.open(ScriptedRemote<>::configuration());
  ASSERT_NE(link, nullptr);
  const auto result = link->start();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), remote_play::StreamError::route_failed);
  EXPECT_EQ(link->state(), remote_play::StreamState::failed);
  EXPECT_EQ(rig.http().order,
            (std::vector<std::string>{"fqdn", "create", "view", "view", "stun",
                                      "stun_ipv6", "start", "release"}));
  EXPECT_EQ(rig.counts().connect, 1U);
  EXPECT_EQ(rig.counts().close, 1U);
  EXPECT_EQ(rig.heard().events, 1U);
  EXPECT_TRUE(
      std::holds_alternative<remote_play::event::Failed>(rig.heard().last));
  ASSERT_TRUE(link->failure());
  const auto& failure = *link->failure();
  EXPECT_EQ(failure.stage(), Stage::psn_start);
  EXPECT_TRUE(failure.as<play::WakeFailure>());
  EXPECT_EQ(failure.http_status(), 401);
  const auto description = failure.sentence();
  EXPECT_NE(
      description.find("PSN console readiness — PSN rejected the request."),
      std::string::npos);
  EXPECT_NE(description.find("HTTP 401."), std::string::npos);
  EXPECT_EQ(description.find("SYNTHETIC_TEST_TOKEN"), std::string::npos);
  EXPECT_FALSE(link->cleanup_failure());
  EXPECT_EQ(
      rig.diagnostics().contains("remote stop: PSN membership DELETE failed"),
      fail_release);
  bool found = false;
  for (const auto& message : rig.diagnostics().messages()) {
    if (message.starts_with("remote start: PSN start failed: PSN "
                            "rejected the request (http=401)"))
      found = true;
    EXPECT_EQ(message.find("SYNTHETIC_TEST_TOKEN"), std::string::npos);
  }
  EXPECT_TRUE(found);
}

INSTANTIATE_TEST_SUITE_P(ReleasedOrNot, InternetLinkPsnRefusal,
                         ::testing::Bool());

}  // namespace
}  // namespace play_test
