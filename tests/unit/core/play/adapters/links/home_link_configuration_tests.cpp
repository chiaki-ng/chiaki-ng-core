#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/home_link_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>

namespace play_test {
namespace {

using namespace home_links;

static_assert(std::is_abstract_v<play::Link>);
static_assert(std::is_move_constructible_v<play::HomeLinkConfiguration>);
static_assert(!std::is_copy_constructible_v<play::HomeLinkConfiguration>);

class HomeLinkSetup : public HomeLinkTest {
 protected:
  auto providers() -> platform::DefaultPlatformFactory& { return providers_; }

 private:
  ZeroCrypto crypto_;
  platform::DefaultPlatformFactory providers_{&crypto_};
};

TEST_F(HomeLinkSetup, KeepsItsSettingsWhenMoved) {
  play::HomeLinkConfiguration config;
  config.console.target = Target::ps5_1;
  config.host = "127.0.0.1";
  config.ctrl_endpoint.port = 9295;
  config.codec = remote_play::VideoCodec::h265_hdr;
  auto moved = std::move(config);
  EXPECT_EQ(moved.console.target, Target::ps5_1);
  EXPECT_EQ(moved.ctrl_endpoint.port, 9295);
  EXPECT_EQ(moved.feedback, remote_play::FeedbackMode::standard);
  moved.feedback = remote_play::FeedbackMode::switch_compatibility;
  EXPECT_EQ(moved.feedback, remote_play::FeedbackMode::switch_compatibility);
}

TEST_F(HomeLinkSetup, CannotLinkWithoutCrypto) {
  platform::DefaultPlatformFactory without_crypto;
  play::HomeLinkConfiguration unavailable;
  EXPECT_FALSE(play::HomeLinkConfiguration::link(std::move(unavailable),
                                                 without_crypto));
}

TEST_F(HomeLinkSetup, RefusesToStartWithoutASink) {
  auto listener = Loopback::listener(providers().network().sockets());
  ASSERT_NE(listener, nullptr);
  const auto endpoint = listener->local_endpoint();
  ASSERT_TRUE(endpoint);
  play::HomeLinkConfiguration connected;
  connected.console.target = Target::ps5_1;
  connected.host = "127.0.0.1";
  connected.ctrl_endpoint = endpoint.value();
  auto session =
      play::HomeLinkConfiguration::link(std::move(connected), providers());
  ASSERT_TRUE(session);
  EXPECT_FALSE(session.value()->start());
  EXPECT_EQ(session.value()->state(), StreamState::idle);
  EXPECT_TRUE(listener->close());
}

TEST_F(HomeLinkSetup, StopsWhileEstablishingTheRoute) {
  auto listener = Loopback::listener(providers().network().sockets());
  ASSERT_NE(listener, nullptr);
  const auto endpoint = listener->local_endpoint();
  ASSERT_TRUE(endpoint);
  play::HomeLinkConfiguration connected;
  connected.console.target = Target::ps5_1;
  connected.host = "127.0.0.1";
  connected.ctrl_endpoint = endpoint.value();
  auto session =
      play::HomeLinkConfiguration::link(std::move(connected), providers());
  ASSERT_TRUE(session);
  EventCounter sink;
  session.value()->set_event_sink(sink);
  EXPECT_TRUE(session.value()->start());
  EXPECT_EQ(session.value()->state(), StreamState::establishing_route);
  EXPECT_TRUE(session.value()->stop());
  EXPECT_TRUE(session.value()->stop());
  EXPECT_EQ(session.value()->state(), StreamState::stopped);
  const auto* changed = sink.last_as<remote_play::event::StateChanged>();
  ASSERT_NE(changed, nullptr);
  EXPECT_EQ(changed->state, StreamState::stopped);
  EXPECT_TRUE(listener->close());
}

TEST_F(HomeLinkSetup, CountsTheEncodedMediaItsSinkReceives) {
  EventCounter sink;
  sink.on_event(remote_play::event::EncodedMedia{
      remote_play::EncodedMediaKind::audio, {std::byte{0x42}}});
  EXPECT_EQ(sink.events(), 1U);
  const auto* media = sink.last_as<remote_play::event::EncodedMedia>();
  ASSERT_NE(media, nullptr);
  ASSERT_FALSE(media->bytes.empty());
  EXPECT_EQ(media->bytes[0], std::byte{0x42});
}

class HomeLinkAudio : public HomeLinkTest {
 protected:
  auto link(Target target, std::uint8_t channels,
            std::shared_ptr<test_support::AtomicStop> stop = nullptr)
      -> std::unique_ptr<play::Link> {
    play::HomeLinkConfiguration config;
    config.console.target = target;
    config.host = "127.0.0.1";
    config.ctrl_endpoint = {platform::IpAddress::loopback_ipv4(), 9295};
    config.active_stream_port = 9296;
    config.tuning.requested_audio_channels = channels;
    if (stop) config.stop = platform::StopToken{std::move(stop)};
    auto session =
        play::HomeLinkConfiguration::link(std::move(config), providers_);
    EXPECT_TRUE(session);
    if (!session) return nullptr;
    return std::move(session.value());
  }

  static auto stopped() -> std::shared_ptr<test_support::AtomicStop> {
    auto state = std::make_shared<test_support::AtomicStop>();
    state->request_stop();
    return state;
  }

 private:
  ZeroCrypto crypto_;
  platform::DefaultPlatformFactory providers_{
      platform::PlatformProviders{.crypto = &crypto_}};
};

TEST_F(HomeLinkAudio, RefusesUnsupportedChannelCounts) {
  for (const auto channels :
       {std::uint8_t{1}, std::uint8_t{7}, std::uint8_t{13}}) {
    SCOPED_TRACE(static_cast<int>(channels));
    auto session = link(Target::ps5_1, channels);
    ASSERT_NE(session, nullptr);
    EventCounter sink;
    session->set_event_sink(sink);
    const auto started = session->start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error(), StreamError::invalid_configuration);
    EXPECT_EQ(session->state(), StreamState::idle);
    const auto& failure = session->failure();
    ASSERT_TRUE(failure);
    EXPECT_TRUE(failure->as<play::LinkFailure::AudioRequestRefused>());
    EXPECT_EQ(failure->describe(),
              "the requested audio channel count is not supported");
  }
}

TEST_F(HomeLinkAudio, AsksForStereoOnEveryTargetAndSurroundOnlyOnPs5) {
  for (const auto target :
       {Target::ps4_8, Target::ps4_9, Target::ps4_10, Target::ps5_1}) {
    for (const auto channels :
         std::initializer_list<std::uint8_t>{0, 2, 6, 8, 12}) {
      if (channels > 2 && target != Target::ps5_1) continue;
      SCOPED_TRACE(static_cast<int>(channels));
      SCOPED_TRACE(static_cast<int>(target));
      auto session = link(target, channels, stopped());
      ASSERT_NE(session, nullptr);
      EventCounter sink;
      session->set_event_sink(sink);
      const auto started = session->start();
      ASSERT_FALSE(started);
      const auto& failure = session->failure();
      ASSERT_TRUE(failure);
      EXPECT_EQ(started.error(), StreamError::route_failed);
      EXPECT_EQ(session->state(), StreamState::failed);
      EXPECT_FALSE(failure->as<play::LinkFailure::AudioRequestRefused>());
    }
  }
}

TEST_F(HomeLinkAudio, RefusesSurroundBeforePs5) {
  for (const auto target : {Target::ps4_8, Target::ps4_9, Target::ps4_10}) {
    for (const auto channels : std::initializer_list<std::uint8_t>{6, 8, 12}) {
      SCOPED_TRACE(static_cast<int>(channels));
      SCOPED_TRACE(static_cast<int>(target));
      auto session = link(target, channels, stopped());
      ASSERT_NE(session, nullptr);
      EventCounter sink;
      session->set_event_sink(sink);
      const auto started = session->start();
      ASSERT_FALSE(started);
      const auto& failure = session->failure();
      ASSERT_TRUE(failure);
      EXPECT_EQ(started.error(), StreamError::invalid_configuration);
      EXPECT_EQ(session->state(), StreamState::idle);
      EXPECT_EQ(sink.events(), 0U);
      EXPECT_EQ(sink.failed_events(), 0U);
      EXPECT_EQ(failure->route(), play::Venue::home());
      EXPECT_EQ(failure->result(), StreamError::invalid_configuration);
      EXPECT_TRUE(failure->as<play::LinkFailure::AudioRequestRefused>());
      EXPECT_EQ(failure->sentence(),
                "Local console: Session — surround audio is not supported for "
                "this target.");
    }
  }
}

}  // namespace
}  // namespace play_test
