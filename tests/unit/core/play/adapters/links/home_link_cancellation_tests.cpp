#include "src/core/play/adapters/links/internet/internet_link.hpp"
#include "src/core/play/adapters/links/internet/internet_link_configuration.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/websocket.hpp"
#include "tests/unit/core/play/adapters/links/home_link_rig.hpp"
#include "tests/support/play/signed_in_player.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <utility>

namespace play_test {
namespace {

using namespace home_links;

class HomeLinkCancellation : public HomeLinkTest {
 protected:
  auto providers() -> platform::DefaultPlatformFactory& { return providers_; }

 private:
  ZeroCrypto crypto_;
  platform::DefaultPlatformFactory providers_{&crypto_};
};

TEST_F(HomeLinkCancellation, FailsAsCancelledWhenStoppedBeforeStarting) {
  auto state = std::make_shared<test_support::AtomicStop>();
  play::HomeLinkConfiguration cancelled;
  cancelled.host = "127.0.0.1";
  cancelled.ctrl_endpoint = {platform::IpAddress::loopback_ipv4(), 9295};
  cancelled.stop = platform::StopToken{state};
  auto pre =
      play::HomeLinkConfiguration::link(std::move(cancelled), providers());
  ASSERT_TRUE(pre);
  EventCounter sink;
  pre.value()->set_event_sink(sink);
  state->request_stop();
  const auto pre_start = pre.value()->start();
  ASSERT_FALSE(pre_start);
  EXPECT_EQ(pre_start.error(), StreamError::route_failed);
  EXPECT_EQ(pre.value()->state(), StreamState::failed);
  EXPECT_EQ(sink.failed_events(), 1U);
  const auto* failed = sink.last_as<remote_play::event::Failed>();
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason, remote_play::StreamFailure::cancelled);
  const auto pre_events = sink.events();
  EXPECT_TRUE(pre.value()->stop());
  EXPECT_TRUE(pre.value()->stop());
  EXPECT_EQ(pre.value()->state(), StreamState::failed);
  EXPECT_EQ(sink.events(), pre_events);
  EXPECT_EQ(sink.failed_events(), 1U);
}

TEST_F(HomeLinkCancellation, StopsOnTheNextPumpWhenCancelledWhileRunning) {
  auto listener = Loopback::listener(providers().network().sockets());
  ASSERT_NE(listener, nullptr);
  const auto endpoint = listener->local_endpoint();
  ASSERT_TRUE(endpoint);
  auto running_state = std::make_shared<test_support::AtomicStop>();
  play::HomeLinkConfiguration running;
  running.host = "127.0.0.1";
  running.ctrl_endpoint = endpoint.value();
  running.stop = platform::StopToken{running_state};
  auto session =
      play::HomeLinkConfiguration::link(std::move(running), providers());
  ASSERT_TRUE(session);
  EventCounter sink;
  session.value()->set_event_sink(sink);
  ASSERT_TRUE(session.value()->start());
  EXPECT_EQ(session.value()->state(), StreamState::establishing_route);
  running_state->request_stop();
  EXPECT_TRUE(session.value()->pump_once());
  EXPECT_EQ(session.value()->state(), StreamState::stopped);
  const auto* changed = sink.last_as<remote_play::event::StateChanged>();
  ASSERT_NE(changed, nullptr);
  EXPECT_EQ(changed->state, StreamState::stopped);
  EXPECT_TRUE(listener->close());
}

class HomeLinkInternetCancellation : public HomeLinkTest {
 protected:
  auto link(std::shared_ptr<test_support::AtomicStop> state)
      -> std::unique_ptr<play::Link> {
    play::InternetLinkConfiguration config;
    config.user =
        test_support::play::SignedInPlayer::fresh_for_remote_play("synthetic-token");
    config.host = "127.0.0.1";
    config.console.target = Target::ps5_1;
    config.stop = platform::StopToken{std::move(state)};
    auto session =
        play::InternetLinks::playing(std::move(config), providers_, nullptr);
    EXPECT_TRUE(session);
    if (!session) return nullptr;
    return std::move(session.value());
  }

  auto http() -> RejectHttp& { return http_; }
  auto websocket() -> test_support::RefusingWebSocket& { return websocket_; }

 private:
  ZeroCrypto crypto_;
  RejectHttp http_;
  test_support::RefusingWebSocket websocket_{
      platform::WebSocketError::unsupported};
  platform::DefaultPlatformFactory providers_{platform::PlatformProviders{
      .crypto = &crypto_, .http = &http_, .websocket = &websocket_}};
};

TEST_F(HomeLinkInternetCancellation, AsksNothingWhenStoppedBeforeStarting) {
  auto state = std::make_shared<test_support::AtomicStop>();
  auto session = link(state);
  ASSERT_NE(session, nullptr);
  EventCounter sink;
  session->set_event_sink(sink);
  state->request_stop();
  const auto started = session->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::route_failed);
  EXPECT_EQ(session->state(), StreamState::failed);
  EXPECT_EQ(sink.failed_events(), 1U);
  const auto* failed = sink.last_as<remote_play::event::Failed>();
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason, remote_play::StreamFailure::cancelled);
  EXPECT_EQ(http().calls(), 0U);
  EXPECT_EQ(websocket().connects.load(), 0U);
  const auto failed_events = sink.events();
  EXPECT_TRUE(session->stop());
  EXPECT_TRUE(session->stop_for_sleep());
  EXPECT_EQ(session->state(), StreamState::failed);
  EXPECT_EQ(sink.events(), failed_events);
  EXPECT_EQ(sink.failed_events(), 1U);
}

TEST_F(HomeLinkInternetCancellation, FailsAsCancelledWhenStoppedMidRequest) {
  auto during_state = std::make_shared<test_support::AtomicStop>();
  auto in_flight = link(during_state);
  ASSERT_NE(in_flight, nullptr);
  EventCounter sink;
  in_flight->set_event_sink(sink);
  http().cancel_on_call(*during_state);
  const auto failed = in_flight->start();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), StreamError::route_failed);
  EXPECT_EQ(http().calls(), 1U);
  EXPECT_EQ(websocket().connects.load(), 0U);
  EXPECT_EQ(sink.failed_events(), 1U);
  const auto* failure = sink.last_as<remote_play::event::Failed>();
  ASSERT_NE(failure, nullptr);
  EXPECT_EQ(failure->reason, remote_play::StreamFailure::cancelled);
}

}  // namespace
}  // namespace play_test
