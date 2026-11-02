#include "src/core/play/adapters/links/unconfirmed_session.hpp"
#include "src/core/play/model/start_policy.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace senkusha = libchiaking::net::protocol::senkusha;
using play::UnconfirmedSession;
using StreamError = remote_play::StreamError;
using StreamState = remote_play::StreamState;

TEST(UnconfirmedSession, WaitsBrieflyForCalibrationOnAMadeUpSessionId) {
  const senkusha::Policy standard{};
  EXPECT_EQ(standard.connect_timeout, senkusha::default_connect_timeout);
  EXPECT_EQ(UnconfirmedSession{true}.calibration(standard).connect_timeout,
            UnconfirmedSession::connect_timeout);
  EXPECT_EQ(UnconfirmedSession{false}.calibration(standard), standard);
  senkusha::Policy quick{};
  quick.connect_timeout = std::chrono::milliseconds{1'000};
  EXPECT_EQ(UnconfirmedSession{true}.calibration(quick), quick);
}

TEST(UnconfirmedSession, StartsOverWhenTheConsoleNeverConfirmedTheStream) {
  const UnconfirmedSession unconfirmed{true};
  EXPECT_EQ(unconfirmed.failure(StreamState::negotiating_stream,
                                StreamError::pipeline_failed),
            StreamError::ctrl_timeout);
  EXPECT_EQ(
      unconfirmed.failure(StreamState::streaming, StreamError::pipeline_failed),
      StreamError::pipeline_failed);
  EXPECT_EQ(UnconfirmedSession{false}.failure(StreamState::negotiating_stream,
                                              StreamError::pipeline_failed),
            StreamError::pipeline_failed);
  play::StartPolicy::Failure failure;
  failure.cause = play::EndReason::Cause::control_timeout;
  EXPECT_TRUE(play::StartPolicy::worth_another_try(play::Where::home, failure));
}

}  // namespace
}  // namespace play_test
