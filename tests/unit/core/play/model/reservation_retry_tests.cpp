#include "src/core/play/model/reservation_retry.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

using libchiaking::play::ReservationAllocateRetry;
using libchiaking::play::ReservationLockRetry;
using libchiaking::play::ReservationPollFailure;
using libchiaking::play::ReservationRestart;

TEST(ReservationLockRetry, PollsUntilTheLimitThenGivesUp) {
  ReservationLockRetry retry;
  for (int poll = 0; poll < ReservationLockRetry::retry_limit; ++poll) {
    const auto decision = retry.on_reply(false, 3);
    EXPECT_EQ(decision.step, ReservationLockRetry::Step::wait_poll);
    EXPECT_EQ(decision.delay_seconds, 3);
  }
  EXPECT_EQ(retry.on_reply(false, 3).step, ReservationLockRetry::Step::failed);
  EXPECT_EQ(retry.on_reply(true, 3).step, ReservationLockRetry::Step::acquired);
}

TEST(ReservationLockRetry, BearsNetworkErrorsInARowUpToTheLimit) {
  ReservationLockRetry retry;
  for (int error = 0; error < ReservationLockRetry::network_error_limit;
       ++error)
    EXPECT_EQ(retry.on_failure(ReservationPollFailure::network).step,
              ReservationLockRetry::Step::wait_network);
  EXPECT_EQ(retry.on_failure(ReservationPollFailure::network).step,
            ReservationLockRetry::Step::failed);
  EXPECT_EQ(retry.on_failure(ReservationPollFailure::cancelled).step,
            ReservationLockRetry::Step::cancelled);
  EXPECT_EQ(retry.on_failure(ReservationPollFailure::refused).step,
            ReservationLockRetry::Step::failed);
}

TEST(ReservationRestart, StartsFreshSessionsUntilTheLimit) {
  ReservationRestart restart;
  for (int fresh = 1; fresh <= ReservationRestart::restart_limit; ++fresh) {
    EXPECT_EQ(restart.on_refused_lock(), ReservationRestart::restart_seconds);
    EXPECT_EQ(restart.restarts(), fresh);
  }
  EXPECT_EQ(restart.on_refused_lock(), std::nullopt);
  EXPECT_EQ(restart.restarts(), ReservationRestart::restart_limit);
}

TEST(ReservationAllocateRetry, WaitsTwiceTheFirstEstimate) {
  ReservationAllocateRetry retry;
  auto decision = retry.on_progress(true, 20, 30);
  EXPECT_EQ(decision.step, ReservationAllocateRetry::Step::wait_poll);
  EXPECT_EQ(decision.delay_seconds, 20);
  decision = retry.on_progress(true, 20, 600);
  EXPECT_EQ(decision.delay_seconds, 20);
  decision = retry.on_progress(true, 20, 600);
  EXPECT_EQ(decision.delay_seconds, 20);
  EXPECT_EQ(retry.on_progress(true, 20, 600).step,
            ReservationAllocateRetry::Step::timed_out);
  EXPECT_EQ(retry.attempt(), 4);
}

TEST(ReservationAllocateRetry, FailsWhenNothingIsQueued) {
  ReservationAllocateRetry retry;
  EXPECT_EQ(retry.on_progress(false, 0, 0).step,
            ReservationAllocateRetry::Step::failed);
  EXPECT_EQ(retry.on_failure(ReservationPollFailure::refused).step,
            ReservationAllocateRetry::Step::failed);
}

TEST(ReservationAllocateRetry, GivesUpAfterTooManyNetworkErrors) {
  ReservationAllocateRetry retry;
  for (int error = 0; error < ReservationAllocateRetry::network_error_limit;
       ++error)
    EXPECT_EQ(retry.on_failure(ReservationPollFailure::network).step,
              ReservationAllocateRetry::Step::wait_network);
  EXPECT_EQ(retry.on_failure(ReservationPollFailure::network).step,
            ReservationAllocateRetry::Step::network_failed);
}

}  // namespace
}  // namespace play_test
