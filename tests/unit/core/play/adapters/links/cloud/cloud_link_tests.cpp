#include "libchiaking/detail/diagnostics/log_queue.hpp"
#include "libchiaking/platform/default_factory.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "src/core/play/adapters/links/cloud/server_connect.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/unit/core/play/adapters/links/cloud/cloud_link_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace play_test {

using test_support::LiveProviders;
namespace {

namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using cloud_links::RejectCrypto;
using cloud_links::StateSink;
using cloud_links::StopOnRoute;
using cloud_links::UnauthorizedHttp;
using StreamError = remote_play::StreamError;
using StreamState = remote_play::StreamState;

class CloudLink : public cloud_links::CloudLinkTest {
 protected:
  auto link(play::CloudLinkConfiguration config)
      -> libchiaking::Expected<std::unique_ptr<play::Link>, StreamError> {
    return play::CloudLinkConfiguration::link(std::move(config), providers,
                                              cleared().seams());
  }

  RejectCrypto crypto;
  UnauthorizedHttp http;
  libchiaking::diagnostics::LogQueue logs{LiveProviders::steady()};
  platform::DefaultPlatformFactory providers{platform::PlatformProviders{
      .crypto = &crypto, .http = &http, .diagnostics = &logs}};
};

TEST_F(CloudLink, RefusesToLinkWithoutCryptoAndHttp) {
  platform::DefaultPlatformFactory no_providers;
  auto unavailable = play::CloudLinkConfiguration::link(
      make_config(), no_providers, cleared().seams());
  ASSERT_FALSE(unavailable);
  EXPECT_EQ(unavailable.error(), StreamError::provider_unavailable);
  platform::DefaultPlatformFactory crypto_only{&crypto};
  unavailable = play::CloudLinkConfiguration::link(make_config(), crypto_only,
                                                   cleared().seams());
  ASSERT_FALSE(unavailable);
  EXPECT_EQ(unavailable.error(), StreamError::provider_unavailable);
  platform::DefaultPlatformFactory http_only{
      platform::PlatformProviders{.http = &http}};
  unavailable = play::CloudLinkConfiguration::link(make_config(), http_only,
                                                   cleared().seams());
  ASSERT_FALSE(unavailable);
  EXPECT_EQ(unavailable.error(), StreamError::provider_unavailable);
}

TEST_F(CloudLink, RefusesToStartWithoutAnEventSink) {
  auto missing_sink = link(make_config());
  ASSERT_TRUE(missing_sink);
  const auto started = missing_sink.value()->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::invalid_configuration);
  EXPECT_EQ(missing_sink.value()->state(), StreamState::idle);
  EXPECT_EQ(http.calls(), 0U);
}

TEST_F(CloudLink, RefusesAnUnsupportedAudioChannelCountBeforeAnyRequest) {
  auto unsupported_audio = make_config();
  unsupported_audio.tuning.requested_audio_channels = 7;
  auto unsupported = link(std::move(unsupported_audio));
  ASSERT_TRUE(unsupported);
  StateSink sink;
  unsupported.value()->set_event_sink(sink);
  const auto started = unsupported.value()->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::invalid_configuration);
  EXPECT_EQ(unsupported.value()->state(), StreamState::idle);
  EXPECT_EQ(http.calls(), 0U);
  ASSERT_TRUE(unsupported.value()->failure());
  EXPECT_TRUE(unsupported.value()
                  ->failure()
                  ->as<play::LinkFailure::AudioRequestRefused>());
}

TEST_F(CloudLink, RefusesATimeZoneWithANulBeforeAnyRequest) {
  auto invalid_config = make_config();
  invalid_config.time_zone.assign("UTC\0+00:00", 10);
  auto invalid = link(std::move(invalid_config));
  ASSERT_TRUE(invalid);
  StateSink sink;
  invalid.value()->set_event_sink(sink);
  const auto started = invalid.value()->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::invalid_configuration);
  EXPECT_EQ(invalid.value()->state(), StreamState::idle);
  EXPECT_EQ(http.calls(), 0U);
  EXPECT_EQ(sink.states, 0U);
  EXPECT_EQ(sink.failures, 0U);
}

TEST_F(CloudLink, RefusesStreamCommandsBeforeItStarts) {
  auto session = link(make_config());
  ASSERT_TRUE(session);
  EXPECT_TRUE(session.value()->send_controller_state({}));
  remote_play::ControllerState out_of_range{};
  out_of_range.controller_id = 4;
  const auto rejected_controller =
      session.value()->send_controller_state(out_of_range);
  ASSERT_FALSE(rejected_controller);
  EXPECT_EQ(rejected_controller.error(), StreamError::controller_rejected);
  const std::array<std::byte, 40> opus{};
  const auto idle_microphone = session.value()->send_microphone(opus);
  ASSERT_FALSE(idle_microphone);
  EXPECT_EQ(idle_microphone.error(), StreamError::invalid_state);
  const auto idle_idr = session.value()->request_idr();
  ASSERT_FALSE(idle_idr);
  EXPECT_EQ(idle_idr.error(), StreamError::invalid_state);
  const auto cloud_connect = session.value()->connect_microphone();
  ASSERT_FALSE(cloud_connect);
  EXPECT_EQ(cloud_connect.error(), StreamError::invalid_state);
  const auto cloud_mute = session.value()->set_microphone_muted(true);
  ASSERT_FALSE(cloud_mute);
  EXPECT_EQ(cloud_mute.error(), StreamError::invalid_state);
}

TEST_F(CloudLink, FailsTheRouteWhenTheIdentityCheckIsRefused) {
  auto session = link(make_config());
  ASSERT_TRUE(session);
  StateSink sink;
  session.value()->set_event_sink(sink);
  const auto started = session.value()->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::route_failed);
  EXPECT_EQ(session.value()->state(), StreamState::failed);
  EXPECT_EQ(http.calls(), 1U);
  EXPECT_EQ(sink.states, 1U);
  EXPECT_EQ(sink.failures, 1U);
  EXPECT_EQ(sink.last_state, StreamState::establishing_route);
  EXPECT_EQ(sink.last_failure,
            remote_play::StreamFailure::route_establishment_failed);
}

TEST_F(CloudLink, LogsAFailedStartWithoutItsSecrets) {
  auto session = link(make_config());
  ASSERT_TRUE(session);
  StateSink sink;
  session.value()->set_event_sink(sink);
  ASSERT_FALSE(session.value()->start());
  bool identity_failed = false;
  bool primary = false;
  bool cleanup = false;
  while (const auto log = logs.read()) {
    const std::string_view text{log->c_str()};
    SCOPED_TRACE(text);
    EXPECT_EQ(text.find("SYNTHETIC_TOKEN"), std::string_view::npos);
    EXPECT_EQ(text.find("synthetic-entitlement"), std::string_view::npos);
    identity_failed = identity_failed ||
                      text.find("operation=cloud_identity outcome=failed") !=
                          std::string_view::npos;
    primary = primary || text.find("operation=session outcome=failed") !=
                             std::string_view::npos;
    cleanup = cleanup || text.find("operation=shutdown outcome=succeeded") !=
                             std::string_view::npos;
  }
  EXPECT_TRUE(identity_failed);
  EXPECT_TRUE(primary);
  EXPECT_TRUE(cleanup);
}

TEST_F(CloudLink, RefusesEverythingButStopOnceFailed) {
  auto session = link(make_config());
  ASSERT_TRUE(session);
  StateSink sink;
  session.value()->set_event_sink(sink);
  ASSERT_FALSE(session.value()->start());
  const auto started = session.value()->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::invalid_state);
  const auto failed_controller = session.value()->send_controller_state({});
  ASSERT_FALSE(failed_controller);
  EXPECT_EQ(failed_controller.error(), StreamError::invalid_state);
  const std::byte pin[4]{};
  const auto pin_result = session.value()->submit_pin(pin);
  ASSERT_FALSE(pin_result);
  EXPECT_EQ(pin_result.error(), StreamError::pin_rejected);

  EXPECT_TRUE(session.value()->stop());
  EXPECT_EQ(session.value()->state(), StreamState::failed);
  EXPECT_EQ(sink.states, 1U);
  EXPECT_EQ(sink.failures, 1U);
  EXPECT_TRUE(session.value()->stop());
  EXPECT_EQ(sink.states, 1U);
  EXPECT_EQ(sink.failures, 1U);
}

TEST_F(CloudLink, EndsTheStartWhenStoppedInsideTheStartCallback) {
  auto session = link(make_config());
  ASSERT_TRUE(session);
  StopOnRoute sink{*session.value()};
  session.value()->set_event_sink(sink);
  EXPECT_TRUE(session.value()->start());
  EXPECT_EQ(session.value()->state(), StreamState::stopped);
  EXPECT_EQ(http.calls(), 0U);
  EXPECT_EQ(sink.stopped(), 1U);
  EXPECT_EQ(sink.failures(), 0U);
  EXPECT_TRUE(session.value()->stop());
  EXPECT_EQ(sink.stopped(), 1U);
  EXPECT_EQ(sink.failures(), 0U);
}

TEST_F(CloudLink, EndsAnAllocatedStartWhenStoppedInsideTheStartCallback) {
  auto allocated_config = make_config();
  play::CloudAllocation allocation;
  allocation.service = libchiaking::cloud::CloudService::cloud_stream;
  allocation.server_ip = "127.0.0.1";
  allocation.server_port = 9;
  allocation.launch_spec.assign("e30=");
  allocation.handshake_key.assign("AAAAAAAAAAAAAAAAAAAAAA==");
  allocated_config.allocation = std::move(allocation);
  auto allocated = link(std::move(allocated_config));
  ASSERT_TRUE(allocated);
  StopOnRoute sink{*allocated.value()};
  allocated.value()->set_event_sink(sink);
  EXPECT_TRUE(allocated.value()->start());
  EXPECT_EQ(allocated.value()->state(), StreamState::stopped);
  EXPECT_EQ(sink.stopped(), 1U);
  EXPECT_EQ(sink.failures(), 0U);
}

TEST_F(CloudLink, TakesCouchCommandsAndAPadCountBeforeItStarts) {
  auto session = link(make_config());
  ASSERT_TRUE(session);
  namespace command = remote_play::command;
  EXPECT_TRUE(session.value()->execute(command::SetPadCount{3}));
  const auto zero = session.value()->execute(command::SetPadCount{0});
  ASSERT_FALSE(zero);
  EXPECT_EQ(zero.error(), StreamError::invalid_argument);
  EXPECT_TRUE(session.value()->execute(command::CouchSetAccountId{
      1, libchiaking::AccountId::from_base64("AQIDBAUGBwg=").value()}));
  const auto home = session.value()->execute(command::GoHome{});
  ASSERT_FALSE(home);
  EXPECT_EQ(home.error(), StreamError::invalid_state);
  const auto idle_open = session.value()->execute(command::CouchOpenPad{1, 0});
  ASSERT_FALSE(idle_open);
  EXPECT_EQ(idle_open.error(), StreamError::invalid_state);
}

TEST(CloudServerConnect, CarriesThePadCountIntoTheControlProfile) {
  play::ServerConnect::Options options;
  options.pad_count = 3;
  const auto profile = play::ServerConnect::control_profile(
      libchiaking::cloud::CloudService::cloud_stream, options);
  EXPECT_EQ(profile.pad_count, std::uint8_t{3});
}

}  // namespace
}  // namespace play_test
