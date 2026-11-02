#include "tests/unit/core/net/protocol/remote_play/stream/active_session_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using remote_play::ActiveHandshakeState;
using remote_play::ActiveSessionError;

class ActiveSessionConnect : public ::testing::Test {
 protected:
  auto session() -> remote_play::ActiveSession& { return *rig.owner; }
  auto init_packets() const -> std::size_t {
    return ActiveSessionWire::init_packets(*rig.state, rig.profile);
  }

  ActiveSessionRig rig{9};
};

TEST_F(ActiveSessionConnect, GivesUpAfterFiveSeconds) {
  ASSERT_NO_FATAL_FAILURE(rig.start());
  EXPECT_EQ(init_packets(), 1U);
  rig.clock.now_us += 4'999'999;
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(session().state(), ActiveHandshakeState::awaiting_connection);
  const auto resent = init_packets();
  EXPECT_GT(resent, 1U);
  rig.clock.now_us += 1;
  const auto timed_out = session().pump_once();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), ActiveSessionError::handshake_timeout);
  ASSERT_TRUE(session().failure());
  EXPECT_EQ(session().failure()->stage,
            ActiveHandshakeState::awaiting_connection);
  EXPECT_FALSE(session().failure()->transport);
  EXPECT_EQ(init_packets(), resent);
  EXPECT_EQ(rig.state->close_count, 1U);
}

TEST_F(ActiveSessionConnect, RefusedReceivesResendInitAndWaitOutTheWindow) {
  ASSERT_NO_FATAL_FAILURE(rig.start());
  for (int attempt = 0; attempt < 3; ++attempt) {
    rig.state->failed_receives = 1;
    rig.clock.now_us += 1'000;
    ASSERT_TRUE(session().pump_once()) << attempt;
  }
  EXPECT_EQ(init_packets(), 4U);
  EXPECT_FALSE(session().failure());
  rig.clock.now_us = 1'000 + 4'999'999;
  ASSERT_TRUE(session().pump_once());
  EXPECT_FALSE(session().failure());
  EXPECT_GT(init_packets(), 4U);
  ++rig.clock.now_us;
  const auto timed_out = session().pump_once();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), ActiveSessionError::handshake_timeout);
  ASSERT_TRUE(session().failure());
  EXPECT_EQ(session().failure()->stage,
            ActiveHandshakeState::awaiting_connection);
}

TEST_F(ActiveSessionConnect, FailedFirstInitSendWaitsOutTheWindow) {
  rig.state->fail_send = true;
  const auto started_at = rig.clock.now_us;
  ASSERT_NO_FATAL_FAILURE(rig.start());
  EXPECT_EQ(init_packets(), 0U);
  EXPECT_EQ(rig.state->close_count, 1U);
  ASSERT_TRUE(session().pump_once());
  EXPECT_FALSE(session().failure());
  EXPECT_EQ(session().state(), ActiveHandshakeState::awaiting_connection);
  rig.clock.now_us = started_at + 4'999'999;
  ASSERT_TRUE(session().pump_once());
  EXPECT_FALSE(session().failure());
  ++rig.clock.now_us;
  const auto timed_out = session().pump_once();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), ActiveSessionError::handshake_timeout);
  ASSERT_TRUE(session().failure());
  EXPECT_EQ(session().failure()->stage,
            ActiveHandshakeState::awaiting_connection);
  EXPECT_FALSE(session().failure()->transport);
  EXPECT_EQ(rig.state->close_count, 1U);
}

TEST_F(ActiveSessionConnect, ReportsCongestionBeforeTheHandshakeCompletes) {
  ASSERT_NO_FATAL_FAILURE(rig.start());
  rig.clock.now_us += 199'999;
  ASSERT_TRUE(session().pump_once());
  EXPECT_TRUE(rig.congestion_packets().empty());
  ++rig.clock.now_us;
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(session().state(), ActiveHandshakeState::awaiting_connection);
  const auto sent = rig.congestion_packets();
  ASSERT_EQ(sent.size(), 1U);
  EXPECT_EQ(sent[0].size(), 14U);
  for (const auto byte : sent[0]) EXPECT_EQ(byte, std::byte{0});
  rig.answer_handshake();
  ASSERT_TRUE(session().pump_once());
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(session().state(), ActiveHandshakeState::awaiting_bang);
  EXPECT_EQ(rig.congestion_packets().size(), 1U);
  rig.clock.now_us += 199'999;
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.congestion_packets().size(), 1U);
  ++rig.clock.now_us;
  ASSERT_TRUE(session().pump_once());
  EXPECT_EQ(rig.congestion_packets().size(), 2U);
  EXPECT_TRUE(session().request_stop());
}

}  // namespace
}  // namespace remote_play_test
