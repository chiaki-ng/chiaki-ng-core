#include "tests/unit/core/net/protocol/remote_play/stream/active_session_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;
using remote_play::ActiveSessionError;
using Frames = std::vector<std::uint16_t>;
using Range = std::pair<std::uint16_t, std::uint16_t>;

class ActiveSessionVideo : public ::testing::Test {
 protected:
  auto session() -> remote_play::ActiveSession& { return *rig.owner; }

  void disconnect_console() {
    ASSERT_NO_FATAL_FAILURE(rig.enqueue_control(
        takion::Disconnect{"Server shutting down", std::nullopt}));
    const auto ended = session().pump_once();
    ASSERT_FALSE(ended);
    EXPECT_EQ(ended.error(), ActiveSessionError::peer_disconnected);
  }

  auto fec_failures()
      -> std::vector<remote_play::feedback_event::VideoFecFailure> {
    std::vector<remote_play::feedback_event::VideoFecFailure> failures;
    while (auto event = session().take_feedback_event())
      if (const auto* failure =
              std::get_if<remote_play::feedback_event::VideoFecFailure>(
                  &*event))
        failures.push_back(*failure);
    return failures;
  }

  void report_unreportable_losses(bool idr_on_fec_failure) {
    ASSERT_NO_FATAL_FAILURE(
        rig.make_ready(ActiveSessionRig::console_units(idr_on_fec_failure)));
    const auto start = rig.state->sent.size();
    ASSERT_NO_FATAL_FAILURE(
        rig.enqueue_video(0x9000, H264Bits::frame(false), 0, 3, 1));
    ASSERT_NO_FATAL_FAILURE(
        rig.enqueue_video(0x9001, H264Bits::frame(false), 0, 3, 1));
    ASSERT_TRUE(session().pump_once());
    EXPECT_TRUE(rig.corrupt_frames(start).empty());
    auto failures = fec_failures();
    if (idr_on_fec_failure) {
      EXPECT_EQ(rig.count_idr_requests(start), 1U);
      ASSERT_EQ(failures.size(), 1U);
      EXPECT_EQ(failures[0].frame_index, 0x9000U);
      EXPECT_TRUE(failures[0].idr_request_sent);
    } else {
      EXPECT_EQ(rig.count_idr_requests(start), 0U);
      EXPECT_TRUE(failures.empty());
    }
    ASSERT_NO_FATAL_FAILURE(
        rig.enqueue_video(0x9002, H264Bits::frame(false), 0, 3, 1));
    ASSERT_TRUE(session().pump_once());
    EXPECT_TRUE(rig.corrupt_frames(start).empty());
    EXPECT_EQ(rig.count_idr_requests(start), idr_on_fec_failure ? 1U : 0U);
    failures = fec_failures();
    ASSERT_EQ(failures.size(), idr_on_fec_failure ? 1U : 0U);
    if (!failures.empty()) {
      EXPECT_EQ(failures[0].frame_index, 0x9001U);
      EXPECT_TRUE(failures[0].idr_request_sent);
    }
    EXPECT_TRUE(session().report_video_loss(5, 4));
    ASSERT_TRUE(session().pump_once());
    EXPECT_TRUE(rig.corrupt_frames(start).empty());
    EXPECT_EQ(rig.count_idr_requests(start), 1U);
    EXPECT_TRUE(session().report_video_loss(0, 0x8000));
    ASSERT_TRUE(session().pump_once());
    EXPECT_EQ(rig.count_idr_requests(start), 1U);
    ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(0x9003, H264Bits::frame(true)));
    ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(0x9004, H264Bits::frame(false)));
    ASSERT_TRUE(session().pump_once());
    ASSERT_TRUE(session().pump_once());
    EXPECT_EQ(rig.take_video_frames(), Frames({0x9003, 0x9004}));
    EXPECT_EQ(rig.count_idr_requests(start), 1U);
    EXPECT_TRUE(session().report_video_loss(0x9004, 0x9003));
    ASSERT_TRUE(session().pump_once());
    EXPECT_TRUE(rig.corrupt_frames(start).empty());
    EXPECT_EQ(rig.count_idr_requests(start), 2U);
    EXPECT_TRUE(session().request_stop());
  }

  struct Delivered {
    std::size_t audio{};
    std::size_t video{};
    std::uint64_t units{};
  };

  auto deliver_with(bool drop_audio, bool drop_video)
      -> std::optional<Delivered> {
    ActiveSessionRig run{9};
    auto control = ActiveSessionRig::console_units();
    control.drop_audio = drop_audio;
    control.drop_video = drop_video;
    run.make_ready(control);
    if (::testing::Test::HasFatalFailure()) return std::nullopt;
    run.enqueue_video(1, H264Bits::frame(true));
    run.enqueue_video(2, H264Bits::frame(false));
    for (std::uint16_t frame = 1; frame <= 12; ++frame)
      run.enqueue_audio(frame, {std::byte{0x41}}, 0, 1, 0x0101);
    if (::testing::Test::HasFatalFailure()) return std::nullopt;
    for (int pump = 0; pump < 4; ++pump) {
      const auto pumped = run.owner->pump_once();
      EXPECT_TRUE(pumped) << pump;
      if (!pumped) return std::nullopt;
    }
    Delivered delivered;
    while (auto event = run.owner->take_media_event()) {
      if (std::holds_alternative<takion::client_event::EncodedAudio>(*event))
        ++delivered.audio;
      if (const auto* encoded =
              std::get_if<takion::client_event::EncodedVideo>(&*event))
        if (!encoded->profile_header) ++delivered.video;
    }
    delivered.units = run.owner->diagnostics().video_units;
    EXPECT_TRUE(run.owner->request_stop());
    return delivered;
  }

  ActiveSessionRig rig{9};
};

TEST_F(ActiveSessionVideo, GateRunsAtAssemblyAndFeedsThePreviousCompleteFrame) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(1, H264Bits::frame(true)));
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.take_video_frames(), Frames({1}));
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(2, H264Bits::frame(false)));
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(3, H264Bits::frame(false)));
  ASSERT_TRUE(session().pump_once());
  const auto start = rig.state->sent.size();
  ASSERT_NO_FATAL_FAILURE(
      rig.enqueue_video(4, H264Bits::frame(false), 0, 3, 1));
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(5, H264Bits::frame(false)));
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.count_idr_requests(start), 1U);
  auto ranges = rig.corrupt_frames(start);
  ASSERT_EQ(ranges.size(), 2U);
  EXPECT_EQ(ranges[0], Range(4, 4));
  EXPECT_EQ(ranges[1], ranges[0]);
  EXPECT_EQ(rig.take_video_frames(), Frames({2, 3}));
  rig.acknowledge_all();
  const auto waiting = rig.state->sent.size();
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(6, H264Bits::frame(false)));
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(7, H264Bits::frame(false)));
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  ranges = rig.corrupt_frames(waiting);
  ASSERT_EQ(ranges.size(), 2U);
  EXPECT_EQ(ranges[0], Range(4, 5));
  EXPECT_EQ(ranges[1], Range(4, 6));
  EXPECT_EQ(rig.count_idr_requests(waiting), 0U);
  EXPECT_TRUE(rig.take_video_frames().empty());
  EXPECT_EQ(session().diagnostics().dependency_rejections, 3U);
  rig.acknowledge_all();
  const auto recovered = rig.state->sent.size();
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(8, H264Bits::frame(true)));
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(9, H264Bits::frame(false)));
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  ranges = rig.corrupt_frames(recovered);
  ASSERT_EQ(ranges.size(), 1U);
  EXPECT_EQ(ranges[0], Range(4, 7));
  EXPECT_EQ(rig.take_video_frames(), Frames({8, 9}));
  rig.acknowledge_all();
  ASSERT_TRUE(session().pump_once());
  EXPECT_TRUE(session().report_video_loss(9, 9));
  rig.acknowledge_all();
  ASSERT_TRUE(session().pump_once());
  const auto rewound = rig.state->sent.size();
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(10, H264Bits::frame(true)));
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  ranges = rig.corrupt_frames(rewound);
  ASSERT_EQ(ranges.size(), 1U);
  EXPECT_EQ(ranges[0], Range(9, 9));
  EXPECT_TRUE(session().request_stop());
}

TEST_F(ActiveSessionVideo, ReportsCongestionAtTheIntervalTheConsoleAsks) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready(ActiveSessionRig::console_units(),
                                         H264Bits::header(), {}, 500));
  const auto before = rig.congestion_packets().size();
  rig.clock.now_us += 400'000;
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.congestion_packets().size(), before);
  rig.clock.now_us += 100'000;
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.congestion_packets().size(), before + 1U);
}

TEST_F(ActiveSessionVideo, AnswersTheConsolesHeartbeatAndSendsNoneUnasked) {
  const auto heartbeats = [&] {
    std::size_t count = 0;
    for (const auto& sent : rig.state->sent) {
      const auto data = ActiveSessionWire::data_chunk(sent, rig.profile, false);
      if (data && data->payload.size() == 11 &&
          data->payload[9] == std::byte{0x08} &&
          data->payload[10] == std::byte{0x03})
        ++count;
    }
    return count;
  };
  auto quiet = ActiveSessionRig::console_units();
  quiet.periodic_heartbeat = false;
  ASSERT_NO_FATAL_FAILURE(rig.make_ready(quiet));
  rig.clock.now_us += 3'000'000;
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(heartbeats(), 0U);
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_control(takion::Heartbeat{}));
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(heartbeats(), 1U);
}

TEST_F(ActiveSessionVideo, ParityArrivingAfterItsFrameIsNotARejection) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(1, H264Bits::frame(true), 0, 2, 1));
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(
      1, std::vector<std::byte>(8, std::byte{0x5a}), 1, 2, 1));
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.take_video_frames(), Frames({1}));
  const auto& diagnostics = session().diagnostics();
  EXPECT_EQ(diagnostics.video_late_parity, 1U);
  EXPECT_EQ(diagnostics.assembly_rejections, 0U);
  EXPECT_EQ(diagnostics.video_stale, 0U);
}

TEST_F(ActiveSessionVideo, UnreportableLossesRequestOneIdrWhenFecFailuresDo) {
  report_unreportable_losses(true);
}

TEST_F(ActiveSessionVideo,
       UnreportableLossesRequestOneIdrWhenFecFailuresDoNot) {
  report_unreportable_losses(false);
}

TEST_F(ActiveSessionVideo, OverflowDropsQueuedVideoAndRequestsAnIdr) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  const auto start = rig.state->sent.size();
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(1, H264Bits::frame(true)));
  for (std::uint16_t frame = 2; frame <= 65; ++frame)
    ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(frame, H264Bits::frame(false)));
  for (std::uint16_t frame = 0; frame < 300; ++frame)
    ASSERT_NO_FATAL_FAILURE(
        rig.enqueue_audio(static_cast<std::uint16_t>(frame + 1),
                          {std::byte{0x41}}, 0, 1, 0x0101));
  for (int pump = 0; pump < 8; ++pump)
    ASSERT_TRUE(session().pump_once()) << pump;
  auto ranges = rig.corrupt_frames(start);
  ASSERT_EQ(ranges.size(), 1U);
  EXPECT_EQ(ranges[0], Range(1, 64));
  EXPECT_EQ(rig.count_idr_requests(start), 1U);
  EXPECT_TRUE(rig.take_video_frames().empty());
  EXPECT_GE(session().dropped_media_events(), 64U);
  rig.acknowledge_all();
  const auto recovery = rig.state->sent.size();
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(66, H264Bits::frame(true)));
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(67, H264Bits::frame(false)));
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  ranges = rig.corrupt_frames(recovery);
  ASSERT_EQ(ranges.size(), 1U);
  EXPECT_EQ(ranges[0], Range(1, 65));
  EXPECT_EQ(rig.take_video_frames(), Frames({66, 67}));
  EXPECT_TRUE(session().request_stop());
}

TEST_F(ActiveSessionVideo,
       MediaReceivedBeforeAConsoleDisconnectIsStillDelivered) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(1, H264Bits::frame(true)));
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.take_video_frames(), Frames({1}));
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(2, H264Bits::frame(false)));
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(3, H264Bits::frame(false)));
  ASSERT_NO_FATAL_FAILURE(disconnect_console());
  EXPECT_EQ(rig.take_video_frames(), Frames({2, 3}));
}

TEST_F(ActiveSessionVideo, ConsoleDisconnectFreesUnitsWaitingForReorder) {
  ASSERT_NO_FATAL_FAILURE(rig.make_ready());
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(1, H264Bits::frame(true)));
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.take_video_frames(), Frames({1}));
  ++rig.next_video_packet;
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(3, H264Bits::frame(false)));
  ASSERT_TRUE(session().pump_once());
  EXPECT_TRUE(rig.take_video_frames().empty());
  ASSERT_NO_FATAL_FAILURE(disconnect_console());
  EXPECT_TRUE(rig.take_video_frames().empty());
  EXPECT_EQ(session().diagnostics().reorder_skipped, 0U);
}

TEST_F(ActiveSessionVideo,
       DisabledMediaIsDroppedBeforeAssemblyLikeTheReference) {
  const auto both = deliver_with(false, false);
  ASSERT_TRUE(both);
  EXPECT_GT(both->audio, 0U);
  EXPECT_EQ(both->video, 2U);
  EXPECT_EQ(both->units, 2U);
  const auto no_audio = deliver_with(true, false);
  ASSERT_TRUE(no_audio);
  EXPECT_EQ(no_audio->audio, 0U);
  EXPECT_EQ(no_audio->video, 2U);
  EXPECT_EQ(no_audio->units, 2U);
  const auto no_video = deliver_with(false, true);
  ASSERT_TRUE(no_video);
  EXPECT_EQ(no_video->audio, both->audio);
  EXPECT_EQ(no_video->video, 0U);
  EXPECT_EQ(no_video->units, 0U);
  const auto neither = deliver_with(true, true);
  ASSERT_TRUE(neither);
  EXPECT_EQ(neither->audio, 0U);
  EXPECT_EQ(neither->video, 0U);
  EXPECT_EQ(neither->units, 0U);
}

TEST_F(ActiveSessionVideo, PublicIdrRequestMatchesTheReferenceSend) {
  ASSERT_NO_FATAL_FAILURE(
      rig.make_ready(ActiveSessionRig::console_units(false)));
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(1, H264Bits::frame(true)));
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.take_video_frames(), Frames({1}));
  const auto start = rig.state->sent.size();
  EXPECT_TRUE(session().post_idr_request());
  EXPECT_TRUE(session().post_idr_request());
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.count_idr_requests(start), 2U);
  ASSERT_NO_FATAL_FAILURE(rig.enqueue_video(2, H264Bits::frame(false)));
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.take_video_frames(), Frames({2}));
  EXPECT_TRUE(session().request_stop());
  const auto stopped = session().post_idr_request();
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error(), ActiveSessionError::not_ready);
}

}  // namespace
}  // namespace remote_play_test
