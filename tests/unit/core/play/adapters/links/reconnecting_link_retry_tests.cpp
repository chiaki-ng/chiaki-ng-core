#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/reconnecting_link_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace rp = libchiaking::net::protocol::remote_play;
using reconnecting_links::Rig;

class ReconnectingLinkRetry : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(rig_.link().start());
    ASSERT_EQ(rig_.link().state(), rp::StreamState::establishing_route);
    rig_.current().fail(rp::StreamError::ctrl_timeout);
    ASSERT_EQ(rig_.sink().failures(), 0U);
    ASSERT_EQ(rig_.sink().states(),
              std::vector{rp::StreamState::establishing_route});
    ASSERT_TRUE(rig_.link().pump_once());
  }

  auto rig() -> Rig& { return rig_; }

 private:
  Rig rig_;
};

TEST_F(ReconnectingLinkRetry, WakesTheConsoleAndAnnouncesTheNextAttempt) {
  EXPECT_EQ(rig().watcher().wakes(), 1U);
  ASSERT_EQ(rig().watcher().tries().size(), 1U);
  const auto& first = rig().watcher().tries()[0];
  EXPECT_EQ(first.attempt, 2U);
  EXPECT_TRUE(first.woke);
  EXPECT_FALSE(first.reconnecting);
  EXPECT_EQ(first.rest, play::StartPolicy::rest_between_attempts);
}

TEST_F(ReconnectingLinkRetry, ReleasesTheFailedAttemptAndStaysConnecting) {
  EXPECT_EQ(rig().script().stopped(), 1U);
  EXPECT_EQ(rig().script().destroyed(), 1U);
  EXPECT_EQ(rig().link().state(), rp::StreamState::establishing_route);
}

TEST_F(ReconnectingLinkRetry, TakesOnlyWhatNeedsNoAttemptWhileResting) {
  EXPECT_TRUE(rig().link().report_video_loss(1, 2));
  const auto refused = rig().link().request_idr();
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), rp::StreamError::invalid_state);
  EXPECT_TRUE(rig().link().wait(std::chrono::milliseconds{1}));
  EXPECT_TRUE(rig().link().wait(std::chrono::milliseconds{0}));
}

TEST_F(ReconnectingLinkRetry, OpensTheNextAttemptOnceTheRestIsOver) {
  rig().clock().advance(std::chrono::milliseconds{1999});
  EXPECT_TRUE(rig().link().pump_once());
  EXPECT_EQ(rig().script().opened(), 1U);
  rig().clock().advance(std::chrono::milliseconds{1});
  EXPECT_TRUE(rig().link().pump_once());
  EXPECT_EQ(rig().script().opened(), 2U);
  EXPECT_EQ(rig().link().state(), rp::StreamState::establishing_route);
}

TEST_F(ReconnectingLinkRetry, ReconnectsWithoutWakingAfterAStreamFails) {
  rig().clock().advance(play::StartPolicy::rest_between_attempts);
  ASSERT_TRUE(rig().link().pump_once());
  ASSERT_EQ(rig().script().opened(), 2U);
  rig().current().stream();
  rig().current().fail(rp::StreamError::ctrl_timeout);
  EXPECT_TRUE(rig().link().pump_once());
  EXPECT_EQ(rig().watcher().wakes(), 1U);
  ASSERT_EQ(rig().watcher().tries().size(), 2U);
  const auto& second = rig().watcher().tries()[1];
  EXPECT_EQ(second.attempt, 3U);
  EXPECT_FALSE(second.woke);
  EXPECT_TRUE(second.reconnecting);
  EXPECT_EQ(rig().sink().failures(), 0U);
}

TEST(ReconnectingLinkReconnecting,
     ReconnectsAnInternetStreamTheConsoleWentSilentOn) {
  Rig rig{play::Where::internet};
  ASSERT_TRUE(rig.link().start());
  rig.current().stream();
  rig.current().lose(play::LinkFailure::lost(
      rp::ActiveSessionFailure{rp::ActiveHandshakeState::ready,
                               rp::ActiveSessionError::peer_silent,
                               {},
                               {}}));
  EXPECT_TRUE(rig.link().pump_once());
  ASSERT_EQ(rig.watcher().tries().size(), 1U);
  EXPECT_TRUE(rig.watcher().tries()[0].reconnecting);
  EXPECT_FALSE(rig.watcher().tries()[0].woke);
  EXPECT_EQ(rig.sink().failures(), 0U);
}

class ReconnectingLinkOffline : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(rig_.link().start());
    rig_.current().stream();
    rig_.current().lose(play::LinkFailure::lost(
        rp::ActiveSessionFailure{rp::ActiveHandshakeState::ready,
                                 rp::ActiveSessionError::peer_silent,
                                 {},
                                 {}}));
    ASSERT_TRUE(rig_.link().pump_once());
  }

  void fail_after(std::chrono::milliseconds rest, bool reached_network) {
    rig_.clock().advance(rest);
    ASSERT_TRUE(rig_.link().pump_once());
    if (reached_network)
      rig_.current().fail(rp::StreamError::ctrl_network_failure);
    else
      rig_.current().lose(play::LinkFailure{
          rp::FailureStage::psn_open, rp::StreamError::route_failed,
          play::PsnFailure{libchiaking::psn::remote_play::SessionError{
              .kind = libchiaking::psn::remote_play::SessionErrorKind::
                  transport_failure,
              .transport = libchiaking::platform::HttpError::disconnected}}});
    ASSERT_TRUE(rig_.link().pump_once());
  }

  void fail_offline() {
    fail_after(play::StartPolicy::rest_between_attempts, false);
  }

  auto rig() -> Rig& { return rig_; }

 private:
  Rig rig_{play::Where::internet};
};

TEST_F(ReconnectingLinkOffline, KeepsReconnectingWithoutCountingWhileOffline) {
  for (std::uint32_t attempt = 0;
       attempt < play::StartPolicy::reconnect_attempts + 1; ++attempt) {
    SCOPED_TRACE(attempt);
    fail_offline();
  }
  ASSERT_EQ(rig().watcher().tries().size(),
            play::StartPolicy::reconnect_attempts + 2);
  EXPECT_FALSE(rig().watcher().tries().front().offline);
  EXPECT_TRUE(rig().watcher().tries().back().offline);
  EXPECT_TRUE(rig().watcher().tries().back().reconnecting);
  EXPECT_EQ(rig().watcher().wakes(), 0U);
  EXPECT_EQ(rig().sink().failures(), 0U);
}

TEST_F(ReconnectingLinkOffline, CountsOfflineAttemptsOnceTheWaitIsOver) {
  fail_offline();
  fail_after(play::StartPolicy::offline_wait, false);
  fail_offline();
  EXPECT_EQ(rig().sink().failures(), 0U);
  fail_offline();
  EXPECT_EQ(rig().watcher().tries().size(), 4U);
  EXPECT_EQ(rig().sink().failures(), 1U);
}

TEST_F(ReconnectingLinkOffline,
       StartsTheWaitAgainOnceAnAttemptReachedTheNetwork) {
  fail_offline();
  fail_after(play::StartPolicy::offline_wait, true);
  EXPECT_FALSE(rig().watcher().tries().back().offline);
  for (int attempt = 0; attempt < 3; ++attempt) {
    SCOPED_TRACE(attempt);
    fail_offline();
  }
  EXPECT_TRUE(rig().watcher().tries().back().offline);
  EXPECT_EQ(rig().sink().failures(), 0U);
}

TEST(ReconnectingLinkGivingUp, ReleasesAFailureNotWorthAnotherTry) {
  Rig rig;
  ASSERT_TRUE(rig.link().start());
  rig.current().fail(rp::StreamError::pipeline_failed);
  EXPECT_EQ(rig.sink().failures(), 0U);
  EXPECT_TRUE(rig.link().pump_once());
  EXPECT_TRUE(rig.watcher().tries().empty());
  EXPECT_EQ(rig.sink().failures(), 1U);
  EXPECT_EQ(rig.link().state(), rp::StreamState::failed);
  ASSERT_TRUE(rig.link().failure());
  EXPECT_EQ(rig.link().failure()->result(), rp::StreamError::pipeline_failed);
  rig.current().stream();
  ASSERT_FALSE(rig.sink().states().empty());
  EXPECT_EQ(rig.sink().states().back(), rp::StreamState::streaming);
  EXPECT_TRUE(rig.link().stop());
  EXPECT_EQ(rig.link().state(), rp::StreamState::stopped);
}

TEST(ReconnectingLinkGivingUp, EndsTheLinkWhenTheConsoleWillNotWake) {
  Rig rig;
  rig.watcher().refuse_to_wake();
  ASSERT_TRUE(rig.link().start());
  rig.current().fail(rp::StreamError::ctrl_timeout);
  EXPECT_TRUE(rig.link().pump_once());
  EXPECT_EQ(rig.watcher().wakes(), 1U);
  EXPECT_TRUE(rig.watcher().tries().empty());
  EXPECT_EQ(rig.sink().failures(), 1U);
  EXPECT_EQ(rig.script().destroyed(), 0U);
}

TEST(ReconnectingLinkOverTheInternet, TriesAgainOnceWithoutWaking) {
  Rig rig{play::Where::internet};
  ASSERT_TRUE(rig.link().start());
  rig.current().fail(rp::StreamError::ctrl_network_failure);
  EXPECT_TRUE(rig.link().pump_once());
  EXPECT_EQ(rig.watcher().wakes(), 0U);
  ASSERT_EQ(rig.watcher().tries().size(), 1U);
  EXPECT_FALSE(rig.watcher().tries()[0].woke);
  rig.clock().advance(play::StartPolicy::rest_between_attempts);
  EXPECT_TRUE(rig.link().pump_once());
  rig.current().fail(rp::StreamError::ctrl_network_failure);
  EXPECT_TRUE(rig.link().pump_once());
  EXPECT_EQ(rig.watcher().tries().size(), 1U);
  EXPECT_EQ(rig.sink().failures(), 1U);
}

}  // namespace
}  // namespace play_test
