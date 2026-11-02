#include "tests/support/enum_printing.hpp"
#include "tests/support/websocket.hpp"
#include "tests/unit/core/play/adapters/links/internet/internet_link_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::SignedInPlayer;
namespace {

using internet_links::NullHttp;
using internet_links::RecordingSink;
using internet_links::Target;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using StreamError = remote_play::StreamError;
using StreamState = remote_play::StreamState;

static_assert(std::is_move_constructible_v<play::InternetLinkConfiguration>);

TEST(InternetLinkDefaults, LeavesIpv6DualSenseCtrlAndKeyboardOff) {
  const play::InternetLinkConfiguration config;
  EXPECT_FALSE(config.enable_ipv6);
  EXPECT_FALSE(config.enable_dualsense_ctrl);
  EXPECT_FALSE(config.enable_keyboard);
}

TEST(InternetLinkDefaults, NeedsItsProvidersToBuildALink) {
  platform::DefaultPlatformFactory missing;
  play::InternetLinkConfiguration config;
  config.user = SignedInPlayer::fresh_for_remote_play();
  config.host = "127.0.0.1";
  const auto link =
      play::InternetLinks::playing(std::move(config), missing, nullptr);
  ASSERT_FALSE(link);
  EXPECT_EQ(link.error(), StreamError::provider_unavailable);
}

class InternetLinkOffline : public ::testing::Test {
 protected:
  static auto configured(std::optional<Target> target = Target::ps5_1)
      -> play::InternetLinkConfiguration {
    play::InternetLinkConfiguration config;
    config.user = SignedInPlayer::fresh_for_remote_play();
    config.host = "127.0.0.1";
    if (target) config.console.target = *target;
    return config;
  }

  auto open(play::InternetLinkConfiguration config)
      -> std::unique_ptr<play::Link> {
    auto link =
        play::InternetLinks::playing(std::move(config), factory_, nullptr);
    if (!link) return nullptr;
    return std::move(link.value());
  }

  void lose_the_network() noexcept { http_.disconnect(); }

 private:
  internet_links::script::RejectCrypto crypto_;
  NullHttp http_;
  test_support::RefusingWebSocket websocket_{
      platform::WebSocketError::backend_failure};
  platform::DefaultPlatformFactory factory_{platform::PlatformProviders{
      .crypto = &crypto_, .http = &http_, .websocket = &websocket_}};
};

TEST_F(InternetLinkOffline, DoesNotStartWithoutATarget) {
  RecordingSink sink;
  const auto link = open(configured(std::nullopt));
  ASSERT_NE(link, nullptr);
  EXPECT_FALSE(link->start());
  EXPECT_EQ(link->state(), StreamState::idle);
  EXPECT_FALSE(link->pump_once());
  EXPECT_FALSE(link->submit_pin({}));
  link->set_event_sink(sink);
  EXPECT_TRUE(link->stop());
  EXPECT_TRUE(link->stop());
  EXPECT_EQ(link->state(), StreamState::stopped);
  EXPECT_EQ(sink.heard().events, 1U);
}

TEST_F(InternetLinkOffline, RefusesSevenAudioChannelsBeforeStarting) {
  RecordingSink sink;
  auto config = configured();
  config.tuning.requested_audio_channels = 7;
  const auto link = open(std::move(config));
  ASSERT_NE(link, nullptr);
  link->set_event_sink(sink);
  const auto started = link->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::invalid_configuration);
  EXPECT_EQ(link->state(), StreamState::idle);
  ASSERT_TRUE(link->failure());
  EXPECT_TRUE(link->failure()->as<play::LinkFailure::AudioRequestRefused>());
}

TEST_F(InternetLinkOffline, TurnsSurroundAudioAwayFromAPs4) {
  for (const auto channels : std::initializer_list<std::uint8_t>{6, 8, 12}) {
    SCOPED_TRACE(static_cast<int>(channels));
    auto source = LiveProviders::threading().create_stop_source();
    ASSERT_TRUE(source);
    static_cast<void>(source.value()->request_stop());
    RecordingSink sink;
    auto config = configured(Target::ps4_10);
    config.tuning.requested_audio_channels = channels;
    config.stop = source.value()->token();
    const auto link = open(std::move(config));
    ASSERT_NE(link, nullptr);
    link->set_event_sink(sink);
    const auto started = link->start();
    ASSERT_FALSE(started);
    const auto& failure = link->failure();
    ASSERT_TRUE(failure);
    EXPECT_EQ(started.error(), StreamError::invalid_configuration);
    EXPECT_EQ(link->state(), StreamState::idle);
    EXPECT_EQ(sink.heard().failed_events, 0U);
    EXPECT_EQ(failure->route(), play::Venue::internet());
    EXPECT_TRUE(failure->as<play::LinkFailure::AudioRequestRefused>());
    EXPECT_EQ(failure->sentence(),
              "Remote console: Session — surround audio is not supported for "
              "this target.");
  }
}

TEST_F(InternetLinkOffline, LetsStereoAndEveryPs5AudioRequestThrough) {
  for (const auto target : {Target::ps4_10, Target::ps5_1}) {
    for (const auto channels :
         std::initializer_list<std::uint8_t>{0, 2, 6, 8, 12}) {
      if (target == Target::ps4_10 && channels > 2) continue;
      SCOPED_TRACE(static_cast<int>(channels));
      SCOPED_TRACE(static_cast<int>(target));
      auto source = LiveProviders::threading().create_stop_source();
      ASSERT_TRUE(source);
      static_cast<void>(source.value()->request_stop());
      RecordingSink sink;
      auto config = configured(target);
      config.tuning.requested_audio_channels = channels;
      config.stop = source.value()->token();
      const auto link = open(std::move(config));
      ASSERT_NE(link, nullptr);
      link->set_event_sink(sink);
      const auto started = link->start();
      ASSERT_FALSE(started);
      const auto& failure = link->failure();
      ASSERT_TRUE(failure);
      EXPECT_EQ(started.error(), StreamError::route_failed);
      EXPECT_FALSE(failure->as<play::LinkFailure::AudioRequestRefused>());
    }
  }
}

TEST_F(InternetLinkOffline, FailsAtPsnOpenWhenPsnIsUnreachable) {
  RecordingSink sink;
  const auto link = open(configured());
  ASSERT_NE(link, nullptr);
  link->set_event_sink(sink);
  const auto started = link->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::route_failed);
  EXPECT_EQ(link->state(), StreamState::failed);
  const auto& heard = sink.heard();
  EXPECT_EQ(heard.events, 1U);
  ASSERT_EQ(heard.setup_progress.size(), 1U);
  EXPECT_EQ(heard.setup_progress.front().stage,
            remote_play::FailureStage::psn_open);
  EXPECT_EQ(heard.setup_progress.front().elapsed_us, 0U);
  EXPECT_TRUE(std::holds_alternative<remote_play::event::Failed>(heard.last));
  ASSERT_TRUE(link->failure());
  EXPECT_FALSE(link->failure()->offline());
  EXPECT_TRUE(link->stop());
  EXPECT_TRUE(link->stop());
}

TEST_F(InternetLinkOffline, FailsAtPsnOpenAsOfflineWithoutANetwork) {
  lose_the_network();
  RecordingSink sink;
  const auto link = open(configured());
  ASSERT_NE(link, nullptr);
  link->set_event_sink(sink);
  const auto started = link->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::route_failed);
  ASSERT_TRUE(link->failure());
  EXPECT_EQ(link->failure()->stage(), remote_play::FailureStage::psn_open);
  EXPECT_TRUE(link->failure()->offline());
  EXPECT_TRUE(link->stop());
}

TEST_F(InternetLinkOffline, FailsAPs4AtPsnOpenWhenPsnIsUnreachable) {
  RecordingSink sink;
  const auto link = open(configured(Target::ps4_10));
  ASSERT_NE(link, nullptr);
  link->set_event_sink(sink);
  const auto started = link->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::route_failed);
  EXPECT_EQ(link->state(), StreamState::failed);
  EXPECT_EQ(sink.heard().events, 1U);
}

TEST(InternetLinkIdentity, FailsTheCtrlStartupWhenTheDeviceIdCannotBeMade) {
  internet_links::script::RejectCrypto crypto;
  NullHttp http;
  test_support::RefusingWebSocket websocket{
      platform::WebSocketError::backend_failure};
  internet_links::script::ScriptRandom random;
  random.fail = true;
  platform::DefaultPlatformFactory factory{
      platform::PlatformProviders{.random = &random,
                                  .crypto = &crypto,
                                  .http = &http,
                                  .websocket = &websocket}};
  play::InternetLinkConfiguration config;
  config.user = SignedInPlayer::fresh_for_remote_play();
  config.console.target = Target::ps5_1;
  RecordingSink sink;
  const auto link =
      play::InternetLinks::playing(std::move(config), factory, nullptr);
  ASSERT_TRUE(link);
  link.value()->set_event_sink(sink);
  const auto started = link.value()->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::ctrl_failed);
  EXPECT_EQ(link.value()->state(), StreamState::failed);
  const auto& last = sink.heard().last;
  ASSERT_TRUE(std::holds_alternative<remote_play::event::Failed>(last));
  EXPECT_EQ(std::get<remote_play::event::Failed>(last).reason,
            remote_play::StreamFailure::negotiation_failed);
  const auto& detail = link.value()->failure();
  ASSERT_TRUE(detail);
  EXPECT_EQ(detail->route(), play::Venue::internet());
  EXPECT_EQ(detail->stage(), remote_play::FailureStage::ctrl_startup);
  EXPECT_EQ(detail->result(), StreamError::ctrl_failed);
}

}  // namespace
}  // namespace play_test
