#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/default_platform/adapters/links/link_driver_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace command = remote_play::command;
using Rig = LinkDriverRig;
using RealStream = Rig::RealStream;
using CloudService = Rig::CloudService;
using StreamError = Rig::StreamError;
using StreamState = Rig::StreamState;
using remote_play::QuitReason;
using namespace std::chrono_literals;

class LinkDriverIdle : public ::testing::Test {
 protected:
  static constexpr std::array pin{std::byte{'1'}, std::byte{'2'},
                                  std::byte{'3'}, std::byte{'4'}};

  static void diagnostics_are_empty(RealStream& opened) {
    EXPECT_EQ(opened->state(), StreamState::idle);
    EXPECT_FALSE(opened->failure());
    EXPECT_FALSE(opened->cleanup_failure());
    EXPECT_EQ(opened->quit().reason, QuitReason::none);
    EXPECT_TRUE(opened->quit().text.empty());
    EXPECT_TRUE(remote_play::quit_reason_is_error(opened->quit().reason));
    const auto diagnostics = opened->diagnostics();
    EXPECT_FALSE(diagnostics.available);
    EXPECT_EQ(diagnostics.video_units, 0U);
    EXPECT_EQ(diagnostics.rejected_packets, 0U);
    EXPECT_EQ(diagnostics.crypto_rejections, 0U);
    EXPECT_EQ(diagnostics.assembled_video, 0U);
    EXPECT_EQ(diagnostics.dropped_posts, 0U);
    EXPECT_EQ(diagnostics.send_dropped, 0U);
    EXPECT_EQ(diagnostics.reorder_dropped, 0U);
    EXPECT_EQ(diagnostics.video_stale, 0U);
    EXPECT_EQ(diagnostics.takion_features, 0U);
    EXPECT_FALSE(diagnostics.round_trip.present);
    EXPECT_FALSE(diagnostics.extension.present);
    EXPECT_FALSE(diagnostics.video_metrics.present);
    EXPECT_FALSE(diagnostics.video_metrics.reports_present);
    EXPECT_FALSE(diagnostics.tos.present);
    EXPECT_EQ(diagnostics.tos.packets, 0U);
    EXPECT_FALSE(opened.platform.log().read());
  }

  static void inputs_are_refused(RealStream& opened, bool keeps_an_early_pin) {
    const std::array<std::byte, play::LinkDriver::microphone_frame_size>
        frame{};
    const auto flags = remote_play::audio_state_flags(3, 0x3ff, 1);
    EXPECT_EQ(opened->state(), StreamState::idle);
    const auto submitted = opened->submit_pin(pin);
    if (keeps_an_early_pin)
      EXPECT_TRUE(submitted.has_value());
    else
      EXPECT_TRUE(Rig::refused(submitted, StreamError::pin_rejected));
    EXPECT_TRUE(Rig::refused(opened->report_video_loss(1, 2),
                             StreamError::invalid_state));
    EXPECT_TRUE(Rig::refused(opened->send_microphone(frame),
                             StreamError::invalid_state));
    EXPECT_TRUE(
        Rig::refused(opened->request_idr(), StreamError::invalid_state));
    EXPECT_TRUE(
        Rig::refused(opened->connect_microphone(), StreamError::invalid_state));
    EXPECT_TRUE(Rig::refused(opened->set_microphone_muted(true),
                             StreamError::invalid_state));
    EXPECT_TRUE(Rig::refused(
        opened->send_audio_state(remote_play::AudioStateKind::flags, flags),
        StreamError::invalid_state));
    EXPECT_TRUE(Rig::refused(
        opened->send_audio_state(remote_play::AudioStateKind::hrtf, {}),
        StreamError::invalid_state));
    EXPECT_TRUE(Rig::refused(opened->select_datacenter("dc1"),
                             StreamError::invalid_state));
    EXPECT_EQ(opened->state(), StreamState::idle);
    EXPECT_EQ(opened.events.state_events, 0U);
  }

  template <class Configuration>
  static void console_commands_are_refused_before_ctrl(
      Configuration configuration) {
    configuration.enable_keyboard = true;
    RealStream opened{std::move(configuration)};
    const auto account = libchiaking::AccountId::from_base64("AQIDBAUGBwg=");
    ASSERT_TRUE(account);
    const auto refused = [&](auto command, StreamError error) {
      return Rig::refused(opened->execute(std::move(command)), error);
    };
    EXPECT_TRUE(refused(command::GotoBed{}, StreamError::invalid_state));
    EXPECT_TRUE(refused(command::GoHome{}, StreamError::invalid_state));
    EXPECT_TRUE(
        refused(command::KeyboardSetText{libchiaking::SecretString{"hi"}},
                StreamError::invalid_state));
    EXPECT_TRUE(refused(command::KeyboardAccept{}, StreamError::invalid_state));
    EXPECT_TRUE(refused(command::KeyboardReject{}, StreamError::invalid_state));
    EXPECT_TRUE(
        refused(command::SetPadCount{5}, StreamError::invalid_argument));
    EXPECT_TRUE(opened->execute(command::SetPadCount{2}));
    EXPECT_TRUE(opened->execute(command::CouchSetAccountId{1, *account}));
    EXPECT_TRUE(refused(command::CouchSetAccountId{0, *account},
                        StreamError::invalid_argument));
    EXPECT_TRUE(refused(command::CouchUserJoin{1}, StreamError::invalid_state));
    EXPECT_TRUE(
        refused(command::CouchPadJoin{1, 0}, StreamError::invalid_state));
    EXPECT_TRUE(refused(command::CouchLeave{1}, StreamError::invalid_state));
    auto passcode = play::AccessCode::parse("1234");
    ASSERT_TRUE(passcode);
    EXPECT_TRUE(refused(command::CouchPasscode{1, std::move(*passcode)},
                        StreamError::invalid_state));
    EXPECT_TRUE(
        refused(command::CouchPresence{1, true}, StreamError::invalid_state));
    EXPECT_TRUE(refused(command::CouchPushPresence{1, true},
                        StreamError::invalid_state));
    EXPECT_TRUE(
        refused(command::CouchAnnouncePads{}, StreamError::invalid_state));
    EXPECT_TRUE(
        refused(command::CouchOpenPad{1, 0}, StreamError::invalid_state));
    EXPECT_TRUE(
        refused(command::CouchAcceptPad{1}, StreamError::invalid_state));
    EXPECT_TRUE(refused(command::CouchClosePad{1}, StreamError::invalid_state));
    EXPECT_TRUE(
        refused(command::CouchJoinPs4{1, 0}, StreamError::invalid_state));
    EXPECT_TRUE(refused(command::CouchLeavePs4{1}, StreamError::invalid_state));
    EXPECT_TRUE(refused(command::CouchJoin{1, 0}, StreamError::invalid_state));
    EXPECT_TRUE(refused(command::CouchRemove{1}, StreamError::invalid_state));
    EXPECT_EQ(opened->state(), StreamState::idle);
  }

  static void stops_once(RealStream& opened) {
    EXPECT_EQ(opened->quit().reason, QuitReason::none);
    EXPECT_TRUE(opened->stop());
    EXPECT_EQ(opened.events.state_events, 1U);
    EXPECT_EQ(opened.events.failed_events, 0U);
    ASSERT_FALSE(opened.events.states.empty());
    EXPECT_EQ(opened.events.states.back().state, StreamState::stopped);
    EXPECT_EQ(opened.events.states.back().quit_reason, QuitReason::stopped);
    EXPECT_EQ(opened->state(), StreamState::stopped);
    EXPECT_EQ(opened->quit().reason, QuitReason::stopped);
    EXPECT_TRUE(opened->stop());
    EXPECT_TRUE(opened->stop_for_sleep());
    EXPECT_EQ(opened.events.state_events, 1U);
    EXPECT_TRUE(Rig::refused(opened->start(), StreamError::invalid_state));
  }
};

TEST_F(LinkDriverIdle, ReportsNoFailureAndEmptyDiagnosticsOnEveryKind) {
  {
    SCOPED_TRACE("local");
    RealStream local{RealStream::local()};
    diagnostics_are_empty(local);
  }
  {
    SCOPED_TRACE("remote");
    RealStream remote{RealStream::remote()};
    diagnostics_are_empty(remote);
  }
  {
    SCOPED_TRACE("cloud");
    RealStream cloud{RealStream::cloud()};
    diagnostics_are_empty(cloud);
  }
  {
    SCOPED_TRACE("legacy cloud stream");
    RealStream legacy_cloud_stream{
        RealStream::cloud(CloudService::legacy_cloud_stream)};
    diagnostics_are_empty(legacy_cloud_stream);
  }
}

class LinkDriverRefusedCloudStart : public LinkDriverIdle {
 protected:
  void SetUp() override {
    ASSERT_EQ(opened_->state(), StreamState::idle);
    ASSERT_TRUE(
        Rig::refused(opened_->start(), StreamError::invalid_configuration));
  }

  static auto surround() -> libchiaking::play::CloudLinkConfiguration {
    auto configuration = Rig::cloud_for(CloudService::legacy_cloud_stream);
    configuration.tuning.requested_audio_channels = 6;
    return configuration;
  }

  RealStream opened_{surround()};
};

TEST_F(LinkDriverRefusedCloudStart, StaysIdleWithoutEvents) {
  EXPECT_EQ(opened_->state(), StreamState::idle);
  const auto& failure = opened_->failure();
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->result(), StreamError::invalid_configuration);
  EXPECT_TRUE(
      failure->as<libchiaking::play::LinkFailure::AudioRequestRefused>());
  EXPECT_NE(failure->sentence().find(
                "surround audio is not supported for this target"),
            std::string::npos);
  EXPECT_TRUE(failure->action_url().empty());
  EXPECT_EQ(opened_.events.state_events, 0U);
  EXPECT_EQ(opened_.events.failed_events, 0U);
  EXPECT_EQ(opened_.events.other_events, 0U);
}

TEST_F(LinkDriverRefusedCloudStart, RefusesWorkButStillWaits) {
  EXPECT_TRUE(Rig::refused(opened_->pump_once(), StreamError::invalid_state));
  const auto before = LiveProviders::steady_us();
  EXPECT_TRUE(opened_->wait(30ms));
  EXPECT_GE(Rig::since(before), 25ms);
  EXPECT_TRUE(Rig::refused(opened_->report_video_loss(2, 1),
                           StreamError::invalid_state));
  EXPECT_TRUE(Rig::refused(opened_->report_video_loss(65535, 0),
                           StreamError::invalid_state));
  EXPECT_TRUE(opened_->send_controller_state(remote_play::ControllerState{}));
  EXPECT_TRUE(
      Rig::refused(opened_->submit_pin(pin), StreamError::pin_rejected));
  EXPECT_EQ(opened_->quit().reason, QuitReason::none);
  EXPECT_TRUE(opened_->quit().text.empty());
}

TEST_F(LinkDriverRefusedCloudStart, PublishesOneStopWhenStopped) {
  EXPECT_TRUE(opened_->stop());
  EXPECT_EQ(opened_.events.state_events, 1U);
  ASSERT_FALSE(opened_.events.states.empty());
  EXPECT_EQ(opened_.events.states.back().state, StreamState::stopped);
  EXPECT_EQ(opened_.events.states.back().quit_reason, QuitReason::stopped);
  EXPECT_EQ(opened_->quit().reason, QuitReason::stopped);
  EXPECT_FALSE(remote_play::quit_reason_is_error(opened_->quit().reason));
  EXPECT_TRUE(opened_->stop());
  EXPECT_EQ(opened_.events.state_events, 1U);
  EXPECT_EQ(opened_.events.failed_events, 0U);
}

TEST_F(LinkDriverIdle, RefusesInputOnEveryKind) {
  {
    SCOPED_TRACE("local");
    RealStream local{RealStream::local()};
    inputs_are_refused(local, true);
  }
  {
    SCOPED_TRACE("remote");
    RealStream remote{RealStream::remote()};
    inputs_are_refused(remote, true);
  }
  {
    SCOPED_TRACE("cloud");
    RealStream cloud{RealStream::cloud()};
    inputs_are_refused(cloud, false);
  }
  {
    SCOPED_TRACE("legacy cloud stream");
    RealStream legacy_cloud_stream{
        RealStream::cloud(CloudService::legacy_cloud_stream)};
    inputs_are_refused(legacy_cloud_stream, false);
  }
}

TEST_F(LinkDriverIdle, RefusesConsoleCommandsBeforeALocalCtrl) {
  console_commands_are_refused_before_ctrl(RealStream::local());
}

TEST_F(LinkDriverIdle, RefusesConsoleCommandsBeforeARemoteCtrl) {
  console_commands_are_refused_before_ctrl(RealStream::remote());
}

TEST_F(LinkDriverIdle, PublishesOneStopWhenStoppedForSleep) {
  RealStream opened{RealStream::cloud()};
  EXPECT_TRUE(opened->stop_for_sleep());
  EXPECT_EQ(opened.events.state_events, 1U);
  ASSERT_FALSE(opened.events.states.empty());
  EXPECT_EQ(opened.events.states.back().state, StreamState::stopped);
  EXPECT_EQ(opened->state(), StreamState::stopped);
  EXPECT_TRUE(opened->stop_for_sleep());
  EXPECT_TRUE(opened->stop());
  EXPECT_EQ(opened.events.state_events, 1U);
  EXPECT_EQ(opened.events.failed_events, 0U);
}

TEST_F(LinkDriverIdle, PublishesOneStopOnEveryKind) {
  {
    SCOPED_TRACE("local");
    RealStream local{RealStream::local()};
    stops_once(local);
  }
  {
    SCOPED_TRACE("remote");
    RealStream remote{RealStream::remote()};
    stops_once(remote);
  }
  {
    SCOPED_TRACE("cloud");
    RealStream cloud{RealStream::cloud()};
    stops_once(cloud);
  }
}

TEST_F(LinkDriverIdle, TakesTwoCancelsWithoutHarm) {
  RealStream opened{RealStream::cloud()};
  opened->cancel();
  opened->cancel();
  EXPECT_TRUE(opened.stop.stop_requested());
  EXPECT_EQ(opened->state(), StreamState::idle);
  EXPECT_EQ(opened.events.state_events, 0U);
  EXPECT_EQ(opened.events.failed_events, 0U);
}

TEST_F(LinkDriverIdle, TakesALaunchPickOnlyDuringARealCloudLaunch) {
  auto configuration = RealStream::cloud();
  configuration.datacenter =
      play::DatacenterPreference::ask_at_launch(2000ms).value();
  RealStream opened{std::move(configuration)};
  EXPECT_TRUE(Rig::refused(opened->select_datacenter(""),
                           StreamError::invalid_argument));
  EXPECT_TRUE(
      Rig::refused(opened->select_datacenter(std::string_view{"dc\0", 3}),
                   StreamError::invalid_argument));
  EXPECT_TRUE(Rig::refused(opened->select_datacenter("dc1"),
                           StreamError::invalid_state));
  EXPECT_TRUE(Rig::refused(opened->select_datacenter("Auto"),
                           StreamError::invalid_state));
  opened->cancel();
  EXPECT_TRUE(Rig::refused(opened->start(), StreamError::route_failed));
  EXPECT_TRUE(Rig::refused(opened->select_datacenter("dc1"),
                           StreamError::invalid_state));
  EXPECT_TRUE(opened->stop());

  RealStream local{RealStream::local()};
  EXPECT_TRUE(Rig::refused(local->select_datacenter("dc1"),
                           StreamError::invalid_state));
}

}  // namespace
}  // namespace play_test
