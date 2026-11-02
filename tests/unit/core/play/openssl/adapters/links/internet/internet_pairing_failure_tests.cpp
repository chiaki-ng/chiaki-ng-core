#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/openssl/adapters/links/internet/remote_pairing_rig.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

namespace consoles = libchiaking::consoles;
namespace play = libchiaking::play;
namespace psn_remote_play = libchiaking::psn::remote_play;
using Rig = RemotePairingRig;
using Failure = Rig::Pairing::Failure;
using FailureStage = Rig::FailureStage;
using Stage = Rig::Stage;
using StreamError = Rig::StreamError;
using Reason = consoles::PairingFailure::Reason;

TEST(InternetPairingFailure, KeepsTheStageOfACancelledRun) {
  const Failure cancelled{.cancelled = true, .stage = Stage::waking_console};
  EXPECT_EQ(cancelled.typed().reason, Reason::cancelled);
  EXPECT_EQ(cancelled.typed().internet_stage, Stage::waking_console);
}

TEST(InternetPairingFailure, CallsARunThatCouldNotBeBuiltMalformed) {
  const Failure unbuilt{.error = StreamError::invalid_configuration};
  EXPECT_EQ(unbuilt.typed().reason, Reason::malformed);
  EXPECT_EQ(unbuilt.typed().internet_stage, Stage::starting);
}

TEST(InternetPairingFailure, CallsAMissingProviderNotAvailable) {
  const Failure unprovided{.error = StreamError::provider_unavailable};
  EXPECT_EQ(unprovided.typed().reason, Reason::not_available);
}

TEST(InternetPairingFailure, KeepsTheHttpStatusAndPsReasonOfARefusal) {
  Failure refused{.error = StreamError::route_failed, .stage = Stage::pairing};
  refused.detail =
      play::LinkFailure::unpaired(play::internet::PsnRemoteRegistrationFailure{
          play::internet::PsnRemoteRegistrationError::response_failed, 0, 403,
          0x80108b12, 0});
  const auto typed = refused.typed();
  EXPECT_EQ(typed.reason, Reason::refused);
  EXPECT_EQ(typed.http_status, 403);
  EXPECT_EQ(typed.ps_reason, 0x80108b12U);
  EXPECT_EQ(typed.internet_stage, Stage::pairing);
  EXPECT_FALSE(typed.unanswered);
}

TEST(InternetPairingFailure, CallsAnInvalidTokenMissingRemotePlayAccess) {
  Failure no_access{.error = StreamError::route_failed,
                    .stage = Stage::opening_session};
  no_access.detail.emplace(
      FailureStage::psn_open, StreamError::route_failed,
      play::PsnFailure{psn_remote_play::StartError{
          psn_remote_play::StartError::PushOpenFailed{
              psn_remote_play::PushOpenError::InvalidToken{}}}});
  EXPECT_EQ(no_access.typed().reason, Reason::unanswered);
  ASSERT_TRUE(no_access.typed().unanswered);
  EXPECT_EQ(no_access.typed().unanswered->reason,
            libchiaking::users::Unanswered::Reason::remote_play_access_missing);
  EXPECT_EQ(no_access.typed().internet_stage, Stage::opening_session);
}

TEST(InternetPairingFailure, KeepsTheStatusOfAnExpiredAccountSession) {
  Failure expired{.error = StreamError::route_failed,
                  .stage = Stage::opening_session};
  expired.detail.emplace(
      FailureStage::psn_open, StreamError::route_failed,
      play::PsnFailure{psn_remote_play::SessionError{
          .kind = psn_remote_play::SessionErrorKind::http_status,
          .status = 401}});
  EXPECT_EQ(expired.typed().reason, Reason::unanswered);
  ASSERT_TRUE(expired.typed().unanswered);
  EXPECT_EQ(expired.typed().unanswered->http_status, 401);
  EXPECT_EQ(expired.typed().http_status, 401);
}

TEST(InternetPairingFailure, CallsALostRouteUnreachable) {
  Failure lost{.error = StreamError::route_failed, .stage = Stage::connecting};
  lost.detail.emplace(FailureStage::control_holepunch,
                      StreamError::route_failed);
  EXPECT_EQ(lost.typed().reason, Reason::unreachable);
  EXPECT_EQ(lost.typed().internet_stage, Stage::connecting);
}

TEST(InternetPairingFailure, CallsAnAbandonedRunCancelled) {
  Failure stopped{.error = StreamError::route_failed,
                  .stage = Stage::connecting};
  stopped.detail = play::LinkFailure::abandoned(FailureStage::unknown,
                                                StreamError::route_failed);
  EXPECT_EQ(stopped.typed().reason, Reason::cancelled);
}

}  // namespace
}  // namespace play_test
