#include "src/core/play/adapters/links/session_diagnostics.hpp"
#include "tests/unit/core/net/protocol/remote_play/stream/active_session_rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;

class ActiveSessionHealthLine : public ::testing::Test {
 protected:
  ActiveSessionLogSink sink;
  test_support::FakeClock clock{10'000'000};
  libchiaking::play::SessionDiagnostics trace{
      &sink, clock, libchiaking::diagnostics::Component::cloud};
  remote_play::StreamDiagnostics value;
};

TEST_F(ActiveSessionHealthLine, ReportsQueueAndSendDrops) {
  value.available = true;
  value.dropped_posts = 11;
  value.reorder_skipped = 12;
  value.reorder_dropped = 13;
  value.send_dropped = 14;
  trace.health(value);
  ASSERT_EQ(sink.messages.size(), 2U);
  EXPECT_NE(sink.messages[0].find("dropped_posts=11 reorder_skipped=12 "
                                  "reorder_dropped=13 send_dropped=14"),
            std::string::npos);
}

TEST_F(ActiveSessionHealthLine,
       ReportsCloudAudioSelectionMetadataAtTheRateLimit) {
  value.available = true;
  value.audio.present = true;
  value.audio.top_channels = 12;
  value.audio.main_present = true;
  value.audio.main_channels = 2;
  value.audio.channel_type_bits = 0x3d;
  value.audio.lane_bits = 0x41;
  value.audio.packet_present = true;
  value.audio.packet_profile = 8;
  value.audio.packet_mode = 3;
  value.audio.packet_total = 3;
  value.audio.packet_fec = 2;
  value.audio.packet_bytes = 200;
  value.audio.current_channels = 2;
  value.audio.selection_present = true;
  value.audio.selection_profile = 8;
  EXPECT_TRUE(trace.health_due());
  trace.health(value);
  ASSERT_EQ(sink.messages.size(), 3U);
  const auto& message = sink.messages.back();
  EXPECT_LT(message.size(), 600U);
  EXPECT_NE(message.find("cloud_audio top_channels=12 main_present=1 "
                         "main_channels=2"),
            std::string::npos);
  EXPECT_NE(message.find("profile_bits=0x00000000 channel_types=0x0000003d "
                         "lanes=0x00000041 first_present=1 lane=0 "
                         "profile=8 mode=3"),
            std::string::npos);
  EXPECT_NE(message.find("packet_bytes=200 current_channels=2 "
                         "selection_present=1 selection_lane=0 "
                         "selection_profile=8"),
            std::string::npos);
  EXPECT_FALSE(trace.health_due());
  trace.health(value);
  EXPECT_EQ(sink.messages.size(), 3U);
  clock.now_us += remote_play::health_log_interval_us;
  EXPECT_TRUE(trace.health_due());
  trace.health(value);
  EXPECT_EQ(sink.messages.size(), 6U);
  EXPECT_FALSE(trace.health_due());
}

TEST_F(ActiveSessionHealthLine, ReportsControllerInputAndWarnsOnTrouble) {
  value.available = true;
  value.input.history_posts = 70'000;
  value.input.history_sequence = 4'464;
  value.input.history_wraps = 1;
  value.input.active_seats = 0b1;
  value.input.held_buttons[0] = 0x1000;
  trace.health(value);
  ASSERT_EQ(sink.messages.size(), 3U);
  EXPECT_NE(sink.messages[1].find("input state_posts=0 state_refused=0 "
                                  "state_sequence=0 history_posts=70000 "
                                  "history_refused=0 history_sequence=4464 "
                                  "history_wraps=1"),
            std::string::npos);
  EXPECT_NE(sink.messages[1].find("seats=0x1 held=0x1000,0x0,0x0,0x0"),
            std::string::npos);
  EXPECT_NE(sink.messages[2].find("history sequence wrapped 1 times"),
            std::string::npos);
  clock.now_us += remote_play::health_log_interval_us;
  trace.health(value);
  EXPECT_EQ(sink.messages.size(), 5U);
  value.input.history_refused = 2;
  clock.now_us += remote_play::health_log_interval_us;
  trace.health(value);
  ASSERT_EQ(sink.messages.size(), 8U);
  EXPECT_NE(sink.messages.back().find("0 state and 2 history packets refused"),
            std::string::npos);
}

}  // namespace
}  // namespace remote_play_test
