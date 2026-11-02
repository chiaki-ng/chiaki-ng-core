#include "tests/unit/core/net/protocol/remote_play/stream/local_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;

static_assert(
    !std::is_copy_constructible_v<remote_play::LocalCtrlStartupResult>);

class LocalCtrlAttemptStartup : public LocalCtrlPeerRig {
 protected:
  void reach_ready(std::uint64_t attempt_id) {
    ASSERT_TRUE(attempt.start(config(attempt_id)));
    EXPECT_FALSE(attempt.session());
    accepted = accept_until(attempt);
    ASSERT_TRUE(accepted);
    ASSERT_NO_FATAL_FAILURE(receive_request(attempt, *accepted));
    ASSERT_NO_FATAL_FAILURE(send_response(*accepted));
    ASSERT_NO_FATAL_FAILURE(wait_until_ready(attempt));
  }

  void TearDown() override {
    if (accepted) {
      EXPECT_TRUE(accepted->close());
    }
    EXPECT_TRUE(listener->close());
  }

  remote_play::LocalCtrlAttempt attempt{crypto, network.sockets(), clock};
  std::unique_ptr<libchiaking::platform::TcpSocket> accepted;
};

TEST_F(LocalCtrlAttemptStartup, HandsTheCtrlSessionOverOnce) {
  ASSERT_NO_FATAL_FAILURE(reach_ready(42));
  ASSERT_TRUE(attempt.session());
  ASSERT_TRUE(attempt.session()->owner());
  EXPECT_EQ(attempt.session()->owner()->state(),
            remote_play::ctrl_lifecycle_detail::State::waiting_id);
  const auto taken = attempt.session()->take_nonce();
  ASSERT_TRUE(taken);
  EXPECT_EQ((*taken)[0], std::byte{1});
  const auto pumped = attempt.pump_once();
  ASSERT_TRUE(pumped);
  EXPECT_TRUE(pumped.value());
  EXPECT_TRUE(attempt.cancel());
  EXPECT_TRUE(attempt.cancel());
  EXPECT_FALSE(attempt.session());
  EXPECT_FALSE(attempt.pump_once());
}

TEST_F(LocalCtrlAttemptStartup, RefusesAZeroAttemptId) {
  remote_play::LocalCtrlAttempt rejected{crypto, network.sockets(), clock};
  auto invalid = config(42);
  invalid.attempt_id = 0;
  EXPECT_FALSE(rejected.start(invalid));
  EXPECT_FALSE(rejected.session());
  EXPECT_TRUE(rejected.cancel());
}

TEST_F(LocalCtrlAttemptStartup, CannotStartOnceCancelled) {
  remote_play::LocalCtrlAttempt fresh{crypto, network.sockets(), clock};
  EXPECT_TRUE(fresh.cancel());
  EXPECT_TRUE(fresh.cancel());
  EXPECT_FALSE(fresh.start(config(42)));
}

TEST_F(LocalCtrlAttemptStartup, PublishesOneReadyTicketForAFallbackSessionId) {
  ASSERT_NO_FATAL_FAILURE(reach_ready(42));
  auto* session_id = attempt.session();
  ASSERT_TRUE(session_id);
  ASSERT_TRUE(session_id->owner());
  EXPECT_EQ(session_id->owner()->state(),
            remote_play::ctrl_lifecycle_detail::State::waiting_id);
  EXPECT_FALSE(session_id->ctrl_response().server_type);
  EXPECT_EQ(session_id->ctrl_response().next_remote_counter, 0U);
  EXPECT_FALSE(session_id->take_ready_ticket());
  const std::array<std::byte, 48> fallback_seed{};
  auto fallback = remote_play::SessionId::fallback(1, fallback_seed);
  ASSERT_TRUE(fallback);
  EXPECT_TRUE(session_id->publish_fallback(std::move(fallback.value())));
  const auto ticket = session_id->take_ready_ticket();
  ASSERT_TRUE(ticket);
  EXPECT_EQ(ticket->attempt_id, 42U);
  EXPECT_EQ(ticket->session_id.view(), "1" + std::string(64, 'A'));
  for (std::size_t i = 0; i < ticket->nonce.size(); ++i)
    EXPECT_EQ(ticket->nonce[i], static_cast<std::byte>(i + 1)) << "byte " << i;
  auto duplicate = remote_play::SessionId::fallback(2, fallback_seed);
  ASSERT_TRUE(duplicate);
  EXPECT_FALSE(session_id->publish_fallback(std::move(duplicate.value())));
  EXPECT_FALSE(session_id->take_ready_ticket());
  EXPECT_TRUE(attempt.cancel());
  EXPECT_FALSE(attempt.session());
  EXPECT_FALSE(attempt.pump_once());
  EXPECT_FALSE(attempt.start(config(43)));
}

TEST_F(LocalCtrlAttemptStartup, CancelsBeforeTheConsoleAnswers) {
  ASSERT_TRUE(attempt.start(config(7)));
  EXPECT_TRUE(attempt.cancel());
  EXPECT_TRUE(attempt.cancel());
  EXPECT_FALSE(attempt.last_failure_progress());
  EXPECT_FALSE(attempt.pump_once());
  EXPECT_FALSE(attempt.session());
}

TEST_F(LocalCtrlAttemptStartup, KeepsTheErrorOfARefusedAttempt) {
  ASSERT_TRUE(attempt.start(config(10)));
  accepted = accept_until(attempt);
  ASSERT_TRUE(accepted);
  ASSERT_NO_FATAL_FAILURE(
      send_response(*accepted, "HTTP/1.1 403 Forbidden\r\n\r\n"));
  const auto deadline = deadline_after(std::chrono::seconds{2});
  bool failed{};
  while (!failed && still_before(deadline)) {
    const auto progress = attempt.pump_once();
    if (!progress) {
      EXPECT_EQ(progress.error(),
                remote_play::LocalCtrlError{
                    remote_play::ConsoleExchangeError::refused});
      failed = true;
    }
    nap();
  }
  EXPECT_TRUE(failed);
  EXPECT_FALSE(attempt.session());
  const auto failure = attempt.last_failure_progress();
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->error, remote_play::ConsoleExchangeError::refused);
  EXPECT_GT(failure->request_bytes_sent, 0U);
  EXPECT_GT(failure->response_bytes_received, 0U);
  EXPECT_FALSE(attempt.pump_once());
  EXPECT_TRUE(attempt.cancel());
  EXPECT_TRUE(attempt.last_failure_progress());
}

}  // namespace
}  // namespace remote_play_test
