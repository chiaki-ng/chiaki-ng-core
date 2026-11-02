#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/home_link_rig.hpp"
#include "tests/unit/core/play/adapters/links/home_link_stream_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace play_test {
namespace {

using namespace home_links;

class HomeLinkStream : public HomeLinkTest,
                       public testing::WithParamInterface<StreamConsole> {
 protected:
  void calibrate() {
    ASSERT_NO_FATAL_FAILURE(exchange_.connect());
    ASSERT_NO_FATAL_FAILURE(exchange_.calibrate());
    ASSERT_NO_FATAL_FAILURE(exchange_.open_active_stream());
  }

  void stream() {
    ASSERT_NO_FATAL_FAILURE(calibrate());
    ASSERT_NO_FATAL_FAILURE(exchange_.deliver_rumble());
    ASSERT_NO_FATAL_FAILURE(exchange_.start_streaming());
  }

  void stream_media() {
    ASSERT_NO_FATAL_FAILURE(stream());
    ASSERT_NO_FATAL_FAILURE(exchange_.press_cross());
    ASSERT_NO_FATAL_FAILURE(exchange_.recover_video());
    ASSERT_NO_FATAL_FAILURE(exchange_.send_microphone());
    ASSERT_NO_FATAL_FAILURE(exchange_.deliver_protected_media());
  }

  void receive_media() {
    ASSERT_NO_FATAL_FAILURE(stream_media());
    ASSERT_NO_FATAL_FAILURE(exchange_.receive_burst());
    ASSERT_EQ(exchange_.sink().media_events(), 4U);
  }

  auto exchange() -> StreamExchange& { return exchange_; }

 private:
  StreamExchange exchange_{GetParam()};
};

TEST_P(HomeLinkStream, CalibratesBeforeOpeningTheActiveStream) {
  ASSERT_NO_FATAL_FAILURE(calibrate());
  EXPECT_EQ(exchange().echoed_probes(), 10U);
  EXPECT_EQ(exchange().link().state(), StreamState::negotiating_stream);
  exchange().close_peers();
}

TEST_P(HomeLinkStream, DeliversRumbleBeforeStreaming) {
  ASSERT_NO_FATAL_FAILURE(calibrate());
  ASSERT_NO_FATAL_FAILURE(exchange().deliver_rumble());
  EXPECT_EQ(exchange().sink().rumble_before_streaming(), 1U);
  EXPECT_EQ(exchange().sink().streaming_events(), 0U);
  exchange().close_peers();
}

TEST_P(HomeLinkStream, ReportsTheStreamInfoBeforeStreaming) {
  ASSERT_NO_FATAL_FAILURE(stream());
  const auto& sink = exchange().sink();
  EXPECT_EQ(sink.stream_info_events(), 1U);
  EXPECT_EQ(sink.streaming_events(), 1U);
  EXPECT_TRUE(sink.info_before_streaming());
  ASSERT_TRUE(sink.last_info());
  EXPECT_EQ(sink.last_info()->audio.channels, 2);
  EXPECT_EQ(sink.last_info()->audio.bits, 16);
  ASSERT_TRUE(sink.last_info()->launch);
  const auto expected_launch =
      exchange().ps5()
          ? remote_play::event::EffectiveLaunch{remote_play::VideoCodec::h264,
                                                1920, 1080, 60, 15000}
          : remote_play::event::EffectiveLaunch{remote_play::VideoCodec::h264,
                                                1280, 720, 60, 10000};
  EXPECT_EQ(*sink.last_info()->launch, expected_launch);
  ASSERT_EQ(sink.last_info()->profiles.size(), 1U);
  EXPECT_EQ(sink.last_info()->profiles[0].width, 1280);
  EXPECT_EQ(sink.last_info()->profiles[0].height, 720);
  EXPECT_EQ(sink.last_info()->profiles[0].video_header,
            (std::vector<std::byte>{std::byte{1}, std::byte{2}}));
  exchange().close_peers();
}

TEST_P(HomeLinkStream, SaysTheConnectionIsWaitingAfterFiveSilentSeconds) {
  ASSERT_NO_FATAL_FAILURE(stream());
  ASSERT_NO_FATAL_FAILURE(
      exchange().fall_silent(std::chrono::milliseconds{4'999}));
  EXPECT_TRUE(exchange().sink().waiting().empty());
  ASSERT_NO_FATAL_FAILURE(exchange().fall_silent(std::chrono::milliseconds{1}));
  EXPECT_EQ(exchange().sink().waiting(), std::vector<bool>{true});
  ASSERT_NO_FATAL_FAILURE(exchange().hear_again());
  EXPECT_EQ(exchange().sink().waiting(), (std::vector<bool>{true, false}));
  EXPECT_EQ(exchange().link().state(), StreamState::streaming);
  exchange().close_peers();
}

TEST_P(HomeLinkStream, SendsControllerStateAsFeedback) {
  ASSERT_NO_FATAL_FAILURE(stream());
  ASSERT_NO_FATAL_FAILURE(exchange().press_cross());
  EXPECT_EQ(exchange().link().state(), StreamState::streaming);
  exchange().close_peers();
}

TEST_P(HomeLinkStream, AsksForRecoveryAfterVideoLoss) {
  ASSERT_NO_FATAL_FAILURE(stream());
  ASSERT_NO_FATAL_FAILURE(exchange().press_cross());
  ASSERT_NO_FATAL_FAILURE(exchange().recover_video());
  EXPECT_EQ(exchange().link().state(), StreamState::streaming);
  exchange().close_peers();
}

TEST_P(HomeLinkStream, SendsMicrophoneAudio) {
  ASSERT_NO_FATAL_FAILURE(stream());
  ASSERT_NO_FATAL_FAILURE(exchange().press_cross());
  ASSERT_NO_FATAL_FAILURE(exchange().recover_video());
  ASSERT_NO_FATAL_FAILURE(exchange().send_microphone());
  EXPECT_EQ(exchange().link().state(), StreamState::streaming);
  exchange().close_peers();
}

TEST_P(HomeLinkStream, DeliversProtectedMedia) {
  ASSERT_NO_FATAL_FAILURE(receive_media());
  EXPECT_EQ(exchange().sink().profile_headers(), 1U);
  EXPECT_EQ(exchange().link().state(), StreamState::streaming);
  exchange().close_peers();
}

TEST_P(HomeLinkStream, StopsWhenTheConsoleDisconnects) {
  ASSERT_NO_FATAL_FAILURE(stream_media());
  ASSERT_NO_FATAL_FAILURE(exchange().disconnect());
  const auto& sink = exchange().sink();
  EXPECT_EQ(sink.media_events(), 4U);
  EXPECT_EQ(exchange().link().state(), StreamState::stopped);
  EXPECT_EQ(sink.failed_events(), 0U);
  EXPECT_EQ(exchange().link().quit().reason,
            remote_play::QuitReason::stream_connection_remote_shutdown);
  EXPECT_EQ(exchange().link().quit().text, "Server shutting down");
  const auto& diagnostics = exchange().diagnostics();
  EXPECT_FALSE(diagnostics.contains("Active handshake FSM failed"));
  EXPECT_FALSE(diagnostics.contains("active pump failed"));
  EXPECT_EQ(diagnostics.severity_of(
                "Remote disconnected from StreamConnection with reason "
                "\"Server shutting down\""),
            platform::LogSeverity::info);
  EXPECT_EQ(diagnostics.severity_of(
                "StreamConnection closing after Remote disconnected"),
            platform::LogSeverity::info);
  EXPECT_EQ(diagnostics.severity_of_exact(
                "Remote disconnected from StreamConnection"),
            platform::LogSeverity::error);
  exchange().close_peers();
}

TEST_P(HomeLinkStream, ReleasesAHeldFrameAndReportsCongestion) {
  ASSERT_NO_FATAL_FAILURE(receive_media());
  ASSERT_NO_FATAL_FAILURE(exchange().hold_a_frame_and_report_congestion());
  EXPECT_EQ(exchange().link().state(), StreamState::streaming);
  exchange().close_peers();
}

TEST_P(HomeLinkStream, KeepsStreamingThroughGapsAndALostCtrlUntilStopped) {
  ASSERT_NO_FATAL_FAILURE(receive_media());
  ASSERT_NO_FATAL_FAILURE(exchange().hold_a_frame_and_report_congestion());
  ASSERT_NO_FATAL_FAILURE(exchange().skip_frames_and_lose_ctrl());
  auto& link = exchange().link();
  const auto& sink = exchange().sink();
  EXPECT_TRUE(link.send_controller_state(exchange().controller()));
  EXPECT_TRUE(link.pump_once());
  EXPECT_TRUE(link.stop());
  EXPECT_EQ(link.state(), StreamState::stopped);
  EXPECT_EQ(sink.failed_events(), 0U);
  const auto* changed = sink.last_as<remote_play::event::StateChanged>();
  ASSERT_NE(changed, nullptr);
  EXPECT_EQ(changed->state, StreamState::stopped);
  const auto events_after_stop = sink.events();
  EXPECT_GT(sink.media_events(), 4U);
  EXPECT_TRUE(link.stop());
  EXPECT_EQ(sink.events(), events_after_stop);
  const auto stopped = link.send_controller_state(exchange().controller());
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error(), StreamError::invalid_state);
  exchange().close_peers();
}

INSTANTIATE_TEST_SUITE_P(
    , HomeLinkStream,
    testing::Values(StreamConsole{"Ps5", Target::ps5_1, 20, 133, 66},
                    StreamConsole{"Ps4", Target::ps4_10, 12, 65, 32}),
    [](const testing::TestParamInfo<StreamConsole>& tested) {
      return std::string{tested.param.name};
    });

}  // namespace
}  // namespace play_test
