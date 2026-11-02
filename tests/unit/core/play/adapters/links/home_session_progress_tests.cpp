#include "src/core/play/adapters/links/home_link_progress.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>
#include <variant>

namespace play_test {
namespace {

namespace progress = libchiaking::play::home_link_progress;
namespace rp = libchiaking::net::protocol::remote_play;
using progress::Stage;

class HomeLinkProgress : public ::testing::Test {
 protected:
  static auto failure(rp::StreamError error = rp::StreamError::ctrl_failed)
      -> progress::ControlFailure {
    return {error, std::nullopt};
  }

  static void advance_to(progress::Progress& machine, Stage wanted) {
    if (wanted >= Stage::requesting_session) {
      ASSERT_TRUE(machine.process(progress::SessionRequested{}));
    }
    if (wanted >= Stage::awaiting_control) {
      ASSERT_TRUE(machine.process(progress::SessionAccepted{}));
    }
    if (wanted >= Stage::awaiting_session_id) {
      ASSERT_TRUE(machine.process(progress::ControlOpened{}));
    }
    if (wanted >= Stage::session_id_known) {
      ASSERT_TRUE(machine.process(progress::SessionIdReceived{}));
    }
    if (wanted >= Stage::calibrating) {
      ASSERT_TRUE(machine.process(progress::CalibrationStarted{}));
    }
    if (wanted >= Stage::active) {
      ASSERT_TRUE(machine.process(progress::ActiveSessionStarted{}));
    }
    ASSERT_EQ(machine.stage(), wanted);
  }
};

TEST_F(HomeLinkProgress, WalksFromTheSessionRequestToAnActiveSession) {
  progress::Progress machine;
  EXPECT_EQ(machine.stage(), Stage::idle);
  EXPECT_FALSE(machine.knows_session_id());
  ASSERT_NO_FATAL_FAILURE(advance_to(machine, Stage::awaiting_session_id));
  EXPECT_FALSE(machine.knows_session_id());
  EXPECT_TRUE(machine.process(progress::SessionIdReceived{}));
  EXPECT_TRUE(machine.knows_session_id());
  EXPECT_TRUE(machine.process(progress::CalibrationStarted{}));
  EXPECT_TRUE(machine.process(progress::ActiveSessionStarted{}));
  EXPECT_EQ(machine.stage(), Stage::active);
  EXPECT_TRUE(machine.knows_session_id());
  EXPECT_FALSE(machine.take_effect());
  EXPECT_EQ(machine.unexpected_events(), 0U);
}

TEST_F(HomeLinkProgress, RefusesStepsOutOfOrder) {
  progress::Progress machine;
  EXPECT_FALSE(machine.process(progress::ControlOpened{}));
  EXPECT_FALSE(machine.process(progress::CalibrationStarted{}));
  EXPECT_EQ(machine.stage(), Stage::idle);
  ASSERT_NO_FATAL_FAILURE(advance_to(machine, Stage::awaiting_control));
  EXPECT_FALSE(machine.process(progress::SessionAccepted{}));
  EXPECT_FALSE(machine.process(progress::SessionIdReceived{}));
  EXPECT_EQ(machine.stage(), Stage::awaiting_control);
  EXPECT_EQ(machine.unexpected_events(), 4U);
}

TEST_F(HomeLinkProgress, EndsTheStreamOnAControlFailureBeforeTheSessionId) {
  progress::Progress machine;
  ASSERT_NO_FATAL_FAILURE(advance_to(machine, Stage::awaiting_session_id));
  EXPECT_TRUE(machine.process(
      progress::ControlFailed{failure(rp::StreamError::ctrl_timeout)}));
  const auto effect = machine.take_effect();
  ASSERT_TRUE(effect);
  const auto* ending = std::get_if<progress::EndStream>(&*effect);
  ASSERT_NE(ending, nullptr);
  EXPECT_EQ(ending->failure.error, rp::StreamError::ctrl_timeout);
  EXPECT_FALSE(ending->failure.detail);
  EXPECT_FALSE(machine.take_effect());
}

TEST_F(HomeLinkProgress,
       HoldsAControlFailureDuringCalibrationUntilTheSessionIsActive) {
  progress::Progress machine;
  ASSERT_NO_FATAL_FAILURE(advance_to(machine, Stage::session_id_known));
  EXPECT_TRUE(machine.process(
      progress::ControlFailed{failure(rp::StreamError::ctrl_timeout)}));
  const auto noted = machine.take_effect();
  ASSERT_TRUE(noted);
  EXPECT_TRUE(std::holds_alternative<progress::NoteDeferredFailure>(*noted));
  EXPECT_TRUE(machine.process(progress::CalibrationStarted{}));
  EXPECT_TRUE(machine.process(progress::ControlFailed{failure()}));
  EXPECT_FALSE(machine.take_effect());
  EXPECT_TRUE(machine.process(progress::ActiveSessionStarted{}));
  const auto effect = machine.take_effect();
  ASSERT_TRUE(effect);
  const auto* earlier =
      std::get_if<progress::EndStreamForEarlierFailure>(&*effect);
  ASSERT_NE(earlier, nullptr);
  EXPECT_EQ(earlier->failure.error, rp::StreamError::ctrl_timeout);
  ASSERT_TRUE(earlier->failure.detail);
  EXPECT_EQ(earlier->failure.detail->stage(), rp::FailureStage::ctrl_receive);
  EXPECT_EQ(earlier->failure.detail->result(), rp::StreamError::ctrl_timeout);
  EXPECT_FALSE(machine.take_effect());
}

TEST_F(HomeLinkProgress, IgnoresAControlFailureOnceActive) {
  progress::Progress machine;
  ASSERT_NO_FATAL_FAILURE(advance_to(machine, Stage::active));
  EXPECT_TRUE(machine.process(progress::ControlFailed{failure()}));
  EXPECT_FALSE(machine.take_effect());
  EXPECT_EQ(machine.unexpected_events(), 0U);
}

TEST_F(HomeLinkProgress, OpensThePinPromptOnceAndAgainAfterAnAnswer) {
  progress::Progress machine;
  ASSERT_NO_FATAL_FAILURE(advance_to(machine, Stage::awaiting_session_id));
  EXPECT_FALSE(machine.asking_for_pin());
  EXPECT_FALSE(machine.process(progress::PinSent{}));
  EXPECT_TRUE(machine.process(progress::PinRequested{}));
  EXPECT_TRUE(machine.asking_for_pin());
  const auto first = machine.take<progress::AskForPin>();
  ASSERT_TRUE(first);
  EXPECT_FALSE(first->retry);
  EXPECT_TRUE(machine.process(progress::PinRequested{}));
  EXPECT_FALSE(machine.take<progress::AskForPin>());
  EXPECT_TRUE(machine.process(progress::PinSent{}));
  EXPECT_FALSE(machine.asking_for_pin());
  EXPECT_TRUE(machine.process(progress::PinRequested{}));
  const auto again = machine.take<progress::AskForPin>();
  ASSERT_TRUE(again);
  EXPECT_TRUE(again->retry);
  EXPECT_EQ(machine.stage(), Stage::awaiting_session_id);
}

TEST_F(HomeLinkProgress, OwesAMadeUpSessionIdToTheControlAnswer) {
  progress::Progress machine;
  ASSERT_NO_FATAL_FAILURE(advance_to(machine, Stage::session_id_known));
  EXPECT_TRUE(machine.process(progress::SessionIdWithoutControl{}));
  EXPECT_FALSE(machine.control_answered());
  EXPECT_TRUE(machine.process(progress::ControlAnswer{}));
  EXPECT_TRUE(machine.control_answered());
  EXPECT_TRUE(machine.take<progress::PublishLateSessionId>());
}

TEST_F(HomeLinkProgress, OwesNothingWhenControlAnsweredFirst) {
  progress::Progress answered;
  ASSERT_NO_FATAL_FAILURE(advance_to(answered, Stage::awaiting_session_id));
  EXPECT_TRUE(answered.process(progress::ControlAnswer{}));
  EXPECT_TRUE(answered.control_answered());
  EXPECT_FALSE(answered.take<progress::PublishLateSessionId>());
}

TEST_F(HomeLinkProgress, EndsEveryStageWhenTheRouteIsReleased) {
  for (const auto stage :
       {Stage::idle, Stage::requesting_session, Stage::awaiting_control,
        Stage::awaiting_session_id, Stage::session_id_known, Stage::calibrating,
        Stage::active}) {
    SCOPED_TRACE(testing::PrintToString(stage));
    progress::Progress machine;
    ASSERT_NO_FATAL_FAILURE(advance_to(machine, stage));
    if (stage == Stage::calibrating) {
      EXPECT_TRUE(machine.process(progress::ControlFailed{failure()}));
      EXPECT_TRUE(machine.take_effect());
      EXPECT_TRUE(machine.process(progress::ControlFailed{failure()}));
    }
    EXPECT_TRUE(machine.process(progress::RouteReleased{}));
    EXPECT_EQ(machine.stage(), Stage::released);
    EXPECT_FALSE(machine.knows_session_id());
    EXPECT_TRUE(machine.process(progress::RouteReleased{}));
    EXPECT_FALSE(machine.process(progress::SessionRequested{}));
    EXPECT_FALSE(machine.process(progress::ActiveSessionStarted{}));
    EXPECT_FALSE(machine.take_effect());
  }
}

}  // namespace
}  // namespace play_test
