#include "tests/unit/core/net/protocol/remote_play/stream/active_session_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;
using remote_play::ActiveHandshakeState;
using remote_play::ActiveSessionError;

class ActiveSessionReady : public ::testing::Test {
 protected:
  auto session() -> remote_play::ActiveSession& { return *rig.owner; }

  ActiveSessionRig rig{9};
};

TEST_F(ActiveSessionReady,
       RefusesAudioStateBeforeReadinessAndWithoutFeatureTen) {
  const auto flags = remote_play::audio_state_flags(3, 0xff, 1);
  ActiveSessionRig unanswered{15};
  ASSERT_NO_FATAL_FAILURE(unanswered.start());
  const auto early = unanswered.owner->post_audio_state(
      remote_play::AudioStateKind::flags, flags);
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(), ActiveSessionError::not_ready);
  EXPECT_FALSE(unanswered.owner->failure());

  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  const auto before = rig.state->sent.size();
  const auto posted =
      session().post_audio_state(remote_play::AudioStateKind::flags, flags);
  ASSERT_FALSE(posted);
  EXPECT_EQ(posted.error(), ActiveSessionError::unsupported_request);
  EXPECT_FALSE(session().failure());
  ASSERT_TRUE(session().pump_once());
  for (const auto& control : rig.controls_since(before))
    EXPECT_TRUE(control.size() < 2 || control[1] != std::byte{33});
  EXPECT_TRUE(session().request_stop());
}

TEST_F(ActiveSessionReady, SendsADeepLinkAsADirectMessageOnlyOnceReady) {
  takion::DeepLink link;
  link.request_id = 0;
  link.raw_data = "psaction:systemservicestatus?isSystemUiOverlaid=1";
  ActiveSessionRig unanswered{15};
  ASSERT_NO_FATAL_FAILURE(unanswered.start());
  const auto early = unanswered.owner->post_deep_link(link);
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(), ActiveSessionError::not_ready);
  EXPECT_FALSE(unanswered.owner->failure());

  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  const auto before = rig.state->sent.size();
  EXPECT_TRUE(session().post_deep_link(link));
  ASSERT_TRUE(session().pump_once());
  std::size_t sent = 0;
  for (const auto& control : rig.controls_since(before))
    if (control.size() >= 2 && control[0] == std::byte{0x08} &&
        control[1] == std::byte{29})
      ++sent;
  EXPECT_EQ(sent, 1U);
  EXPECT_TRUE(session().request_stop());
}

TEST_F(ActiveSessionReady, SendsTheMicrophoneMuteAsADirectMessageOnceReady) {
  ActiveSessionRig unanswered{15};
  ASSERT_NO_FATAL_FAILURE(unanswered.start());
  const auto early = unanswered.owner->post_microphone_mute(false);
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(), ActiveSessionError::not_ready);
  EXPECT_FALSE(unanswered.owner->failure());

  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  const auto before = rig.state->sent.size();
  EXPECT_TRUE(session().post_microphone_mute(false));
  EXPECT_TRUE(session().post_microphone_mute(true));
  ASSERT_TRUE(session().pump_once());
  std::size_t sent = 0;
  for (const auto& control : rig.controls_since(before))
    if (control.size() >= 2 && control[0] == std::byte{0x08} &&
        control[1] == std::byte{29})
      ++sent;
  EXPECT_EQ(sent, 2U);
  EXPECT_TRUE(session().request_stop());
}

TEST_F(ActiveSessionReady, CapsReportedLossAtThePacketLossMaximum) {
  auto control = ActiveSessionRig::console_units();
  control.packet_loss_max = 0.2;
  ASSERT_NO_FATAL_FAILURE(rig.make_ready(control));
  rig.clock.now_us += 200'000;
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  ASSERT_NO_FATAL_FAILURE(
      rig.enqueue_audio(1, {std::byte{0x41}}, 0, 1, 0x0101));
  ASSERT_NO_FATAL_FAILURE(
      rig.enqueue_audio(10, {std::byte{0x42}}, 0, 1, 0x0101));
  ASSERT_TRUE(session().pump_once());
  rig.clock.now_us += 200'000;
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  const auto sent = rig.congestion_packets();
  ASSERT_FALSE(sent.empty());
  const auto& last = sent.back();
  const auto server_sent = (std::to_integer<unsigned>(last[2]) << 8U) |
                           std::to_integer<unsigned>(last[3]);
  const auto lost = (std::to_integer<unsigned>(last[4]) << 8U) |
                    std::to_integer<unsigned>(last[5]);
  EXPECT_EQ(server_sent, 10U);
  EXPECT_EQ(lost, 2U);
  EXPECT_TRUE(session().request_stop());
}

TEST_F(ActiveSessionReady, KeepsTheConsoleDisconnectReasonWhole) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  std::string reason(600, 'r');
  reason.front() = 'A';
  reason.back() = 'Z';
  ASSERT_NO_FATAL_FAILURE(
      rig.enqueue_control(takion::Disconnect{reason, std::nullopt}));
  const auto ended = session().pump_once();
  ASSERT_FALSE(ended);
  EXPECT_EQ(ended.error(), ActiveSessionError::peer_disconnected);
  ASSERT_TRUE(session().failure());
  EXPECT_EQ(session().failure()->peer_reason, reason);
}

TEST_F(ActiveSessionReady, NextDeadlineTracksReorderAndCongestion) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  const auto idle = session().next_deadline_us();
  ASSERT_TRUE(idle);
  EXPECT_GT(*idle, rig.clock.now_us);
  EXPECT_LE(*idle, rig.clock.now_us + 200'000);
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(1, H264Bits::frame(true)));
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.take_video_frames(), std::vector<std::uint16_t>({1}));
  ++rig.next_video_packet;
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(2, H264Bits::frame(false)));
  const auto hole_at = rig.clock.now_us;
  ASSERT_TRUE(session().pump_once());
  const auto due = session().next_deadline_us();
  ASSERT_TRUE(due);
  EXPECT_EQ(*due, hole_at + 16'001);
  rig.clock.now_us = *due - 1;
  ASSERT_TRUE(session().pump_once());
  EXPECT_TRUE(rig.take_video_frames().empty());
  rig.clock.now_us = *due;
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.take_video_frames(), std::vector<std::uint16_t>({2}));
  EXPECT_TRUE(session().request_stop());
  EXPECT_FALSE(session().next_deadline_us());
}

TEST_F(ActiveSessionReady, NextWakeFollowsTakionWorkAndRouteReadiness) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  const auto own = session().next_deadline_us();
  const auto wake = session().next_wake_us();
  ASSERT_TRUE(own);
  ASSERT_TRUE(wake);
  EXPECT_LE(*wake, *own);
  EXPECT_GT(*wake, rig.clock.now_us);
  EXPECT_TRUE(session().post_controller_state(remote_play::ControllerState{}));
  EXPECT_EQ(session().next_wake_us(), rig.clock.now_us);
  ASSERT_TRUE(session().pump_once());
  const auto after = session().next_wake_us();
  ASSERT_TRUE(after);
  EXPECT_GT(*after, rig.clock.now_us);

  ActiveSessionPollable socket;
  ActiveSessionPollable other;
  rig.state->polled = &socket;
  rig.state->exact = true;
  auto calls = rig.state->receive_calls;
  session().note_route_readiness(socket, false);
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.state->receive_calls, calls);
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.state->receive_calls, calls);
  calls = rig.state->receive_calls;
  session().note_route_readiness(other, false);
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.state->receive_calls, calls + 1);

  EXPECT_TRUE(session().request_stop());
  EXPECT_FALSE(session().next_wake_us());
  session().note_route_readiness(socket, false);
}

TEST_F(ActiveSessionReady, KeepsTheNextDeadlineAheadWithoutHeartbeats) {
  auto control = ActiveSessionRig::console_units();
  control.periodic_heartbeat = false;
  ASSERT_NO_FATAL_FAILURE(rig.make_ready(control));
  const auto start = rig.clock.now_us;
  for (unsigned step = 1; step <= 30; ++step) {
    rig.clock.now_us = start + step * 100'000ULL;
    ASSERT_TRUE(session().pump_once()) << step;
    const auto due = session().next_deadline_us();
    ASSERT_TRUE(due) << step;
    EXPECT_GT(*due, rig.clock.now_us) << step;
  }
}

TEST_F(ActiveSessionReady, KeepsASilentPeerByDefault) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  const auto start = rig.clock.now_us;
  for (unsigned second = 1; second <= 60; ++second) {
    rig.clock.now_us = start + second * 1'000'000ULL;
    ASSERT_TRUE(session().pump_once()) << second;
  }
  EXPECT_FALSE(session().failure());
  EXPECT_EQ(rig.state->close_count, 0U);
}

TEST_F(ActiveSessionReady, FailsWhenThePeerStaysSilentPastItsTimeout) {
  auto control = ActiveSessionRig::console_units();
  control.peer_silence_timeout_us = 10'000'000;
  ASSERT_NO_FATAL_FAILURE(rig.make_ready(control));
  const auto start = rig.clock.now_us;
  for (unsigned second = 1; second <= 9; ++second) {
    rig.clock.now_us = start + second * 1'000'000ULL;
    ASSERT_TRUE(session().pump_once()) << second;
  }
  ASSERT_NO_FATAL_FAILURE(
      rig.enqueue_data(7, {std::byte{0}, std::byte{1}, std::byte{2}}));
  ASSERT_TRUE(session().pump_once());
  rig.clock.now_us = start + 18'999'999ULL;
  ASSERT_TRUE(session().pump_once());
  rig.clock.now_us = start + 19'000'000ULL;
  const auto silent = session().pump_once();
  ASSERT_FALSE(silent);
  EXPECT_EQ(silent.error(), ActiveSessionError::peer_silent);
  ASSERT_TRUE(session().failure());
  EXPECT_EQ(session().failure()->cause, ActiveSessionError::peer_silent);
  EXPECT_EQ(session().failure()->stage, ActiveHandshakeState::ready);
  EXPECT_EQ(rig.state->close_count, 1U);
}

TEST_F(ActiveSessionReady, CountsSendDropsApartFromInboundRejections) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  const auto before = session().diagnostics();
  rig.state->fail_send = true;
  EXPECT_TRUE(session().post_controller_state(remote_play::ControllerState{}));
  ASSERT_TRUE(session().pump_once());
  rig.state->fail_send = false;
  const auto after = session().diagnostics();
  EXPECT_EQ(after.send_dropped, before.send_dropped + 1);
  EXPECT_EQ(after.rejected_packets, before.rejected_packets);
  rig.state->incoming.push_back({std::byte{0xff}});
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(session().diagnostics().rejected_packets,
            before.rejected_packets + 1);
  EXPECT_EQ(session().diagnostics().send_dropped, after.send_dropped);
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(1, H264Bits::frame(true)));
  rig.next_video_packet = 0;
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(1, H264Bits::frame(true)));
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(session().diagnostics().reorder_dropped, 1U);
  EXPECT_TRUE(session().request_stop());
}

TEST_F(ActiveSessionReady, PassesLinkMetricsToSessionDiagnostics) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  const auto link = session().snapshot().link;
  const auto diagnostics = session().diagnostics();
  EXPECT_TRUE(diagnostics.available);
  EXPECT_EQ(link.features, takion::native_takion_feature_mask(9));
  EXPECT_EQ(diagnostics.takion_features, link.features);
  const auto round_trip = link.round_trip.value_or(takion::RoundTripMetrics{});
  EXPECT_EQ(diagnostics.round_trip.present, link.round_trip.has_value());
  EXPECT_EQ(diagnostics.round_trip.smoothed_us, round_trip.smoothed_us);
  EXPECT_EQ(diagnostics.round_trip.variation_us, round_trip.variation_us);
  EXPECT_EQ(diagnostics.round_trip.spread, round_trip.spread);
  EXPECT_FALSE(diagnostics.extension.present);
  EXPECT_EQ(diagnostics.extension.received, 0U);
  EXPECT_FALSE(diagnostics.video_metrics.present);
  EXPECT_FALSE(diagnostics.video_metrics.reports_present);
  EXPECT_FALSE(diagnostics.tos.present);
  EXPECT_EQ(diagnostics.tos.packets, 0U);
  EXPECT_TRUE(session().request_stop());
}

}  // namespace
}  // namespace remote_play_test
