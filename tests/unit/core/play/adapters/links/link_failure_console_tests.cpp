#include "src/core/net/protocol/remote_play/rp_reason.hpp"
#include "src/core/play/adapters/links/link_failure.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/link_failure_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

namespace play_test {
namespace {

namespace cloud = libchiaking::cloud;
namespace play = libchiaking::play;
namespace psn_remote_play = libchiaking::psn::remote_play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using link_failures::Failures;
using remote_play::RegistrationTarget;
using Stage = remote_play::FailureStage;
using Error = remote_play::StreamError;
using Refused = play::LinkFailure::AudioRequestRefused;

TEST(LinkFailureAtTheConsole, NamesARefusedSessionRequestConnection) {
  EXPECT_TRUE(Failures::contains(
      Failures::at(play::Venue::home(),
                   play::LinkFailure{Stage::session_init,
                                     Error::session_init_connection_refused})
          .sentence(),
      "refused the session request connection"));
}

TEST(LinkFailureAtTheConsole, NamesARefusedCtrlConnection) {
  EXPECT_TRUE(Failures::contains(
      Failures::at(play::Venue::home(),
                   play::LinkFailure{Stage::ctrl_startup,
                                     Error::ctrl_connection_refused})
          .sentence(),
      "refused the Ctrl connection"));
}

TEST(LinkFailureAtTheConsole, NamesACrashWithItsStatusAndReason) {
  const play::LinkFailure crashed{
      Stage::session_init, Error::session_init_refused,
      play::LinkFailure::ConsoleRefusal{libchiaking::rp_reason_crash, 403}};
  const auto text = crashed.sentence();
  EXPECT_TRUE(Failures::contains(text, "Remote Play crashed"));
  EXPECT_TRUE(Failures::contains(text, "HTTP 403."));
  EXPECT_TRUE(Failures::contains(text, "Console reason 0x80108b15."));
}

class LinkFailureOverPsn : public ::testing::Test {
 protected:
  static auto wake(play::WakeFailure failure) -> play::LinkFailure {
    return play::LinkFailure{Stage::psn_start, Error::route_failed,
                             std::move(failure)};
  }

  static auto open(psn_remote_play::StartError error) -> play::LinkFailure {
    return play::LinkFailure{Stage::psn_open, Error::route_failed,
                             play::PsnFailure{std::move(error)}};
  }

  static auto lookup(psn_remote_play::SessionError error) -> play::LinkFailure {
    return open(
        psn_remote_play::StartError{psn_remote_play::StartError::PushOpenFailed{
            psn_remote_play::PushOpenError::GetFqdnFailed{error}}});
  }
};

TEST_F(LinkFailureOverPsn, NamesAnInvalidStart) {
  const auto text = wake(play::WakeFailure::Refusal::invalid_state).sentence();
  EXPECT_FALSE(Failures::contains(text, "could not start"));
  EXPECT_TRUE(Failures::contains(text, "invalid"));
}

TEST_F(LinkFailureOverPsn, NamesACancelledStart) {
  const auto cancelled = wake(play::WakeFailure::Refusal::cancelled);
  EXPECT_TRUE(cancelled.cancelled());
  EXPECT_TRUE(Failures::contains(cancelled.sentence(), "cancelled"));
}

TEST_F(LinkFailureOverPsn, NamesAnInvalidOpen) {
  const auto text = open(psn_remote_play::StartError{
                             psn_remote_play::StartError::InvalidAttempt{}})
                        .sentence();
  EXPECT_FALSE(Failures::contains(text, "could not start"));
  EXPECT_TRUE(Failures::contains(text, "invalid"));
}

TEST_F(LinkFailureOverPsn, NamesACancelledOpen) {
  const auto stopped = open(
      psn_remote_play::StartError{psn_remote_play::StartError::Cancelled{}});
  EXPECT_TRUE(stopped.cancelled());
  EXPECT_TRUE(Failures::contains(stopped.sentence(), "cancelled"));
}

TEST_F(LinkFailureOverPsn, LacksAccessWhenThePushTokenIsRefused) {
  const auto unleased = open(
      psn_remote_play::StartError{psn_remote_play::StartError::PushOpenFailed{
          psn_remote_play::PushOpenError::InvalidToken{}}});
  const auto* psn = unleased.as<play::PsnFailure>();
  ASSERT_NE(psn, nullptr);
  EXPECT_TRUE(psn->lacks_access());
}

TEST_F(LinkFailureOverPsn, IsOfflineWhenAPsnRequestNeverLeftTheMachine) {
  const psn_remote_play::SessionError unsent{
      .kind = psn_remote_play::SessionErrorKind::transport_failure,
      .transport = libchiaking::platform::HttpError::disconnected};
  EXPECT_TRUE(lookup(unsent).offline());
  EXPECT_TRUE((play::LinkFailure{Stage::psn_open, Error::route_failed,
                                 play::PsnFailure{unsent}}
                   .offline()));
}

TEST_F(LinkFailureOverPsn, IsOnlineWhenPsnAnsweredOrTheRequestTimedOut) {
  EXPECT_FALSE(lookup({.kind = psn_remote_play::SessionErrorKind::http_status,
                       .status = 503})
                   .offline());
  EXPECT_FALSE(
      lookup({.kind = psn_remote_play::SessionErrorKind::transport_failure,
              .transport = libchiaking::platform::HttpError::timed_out})
          .offline());
  EXPECT_FALSE(open(psn_remote_play::StartError{
                        psn_remote_play::StartError::Cancelled{}})
                   .offline());
  EXPECT_FALSE((play::LinkFailure{Stage::session_init,
                                  Error::session_init_network_failure}
                    .offline()));
}

TEST(LinkFailureAudioRequest, AcceptsStereoOrNothingEverywhere) {
  for (const auto target :
       {RegistrationTarget::ps4_8, RegistrationTarget::ps4_9,
        RegistrationTarget::ps4_10, RegistrationTarget::ps4_unknown,
        RegistrationTarget::ps5_1})
    for (const auto channels : std::initializer_list<std::uint8_t>{0, 2}) {
      SCOPED_TRACE(testing::Message()
                   << testing::PrintToString(target) << ' ' << +channels);
      EXPECT_FALSE(Refused::for_console(target, channels));
    }
  for (const auto service : {cloud::CloudService::legacy_cloud_stream,
                             cloud::CloudService::cloud_stream})
    for (const auto channels : std::initializer_list<std::uint8_t>{0, 2}) {
      SCOPED_TRACE(testing::Message()
                   << testing::PrintToString(service) << ' ' << +channels);
      EXPECT_FALSE(Refused::for_cloud(service, channels));
    }
}

TEST(LinkFailureAudioRequest, RefusesSurroundOnlyWhereItIsUnsupported) {
  for (const auto channels : std::initializer_list<std::uint8_t>{6, 8, 12}) {
    SCOPED_TRACE(+channels);
    EXPECT_FALSE(Refused::for_console(RegistrationTarget::ps5_1, channels));
    EXPECT_FALSE(
        Refused::for_cloud(cloud::CloudService::cloud_stream, channels));
    EXPECT_TRUE(Refused::for_console(RegistrationTarget::ps4_10, channels));
    EXPECT_TRUE(
        Refused::for_cloud(cloud::CloudService::legacy_cloud_stream, channels));
  }
}

class LinkFailureSurroundRefusal : public ::testing::Test {
 protected:
  static auto surround(play::Venue venue) -> std::string {
    const auto refused = Refused::for_console(RegistrationTarget::ps4_10, 8);
    EXPECT_TRUE(refused);
    if (!refused) return {};
    return Failures::at(
               venue, play::LinkFailure{Stage::unknown,
                                        Error::invalid_configuration, *refused})
        .sentence();
  }
};

TEST_F(LinkFailureSurroundRefusal, NamesTheVenue) {
  EXPECT_EQ(surround(play::Venue::home()),
            "Local console: Session — surround audio is not supported for this "
            "target.");
  EXPECT_EQ(surround(play::Venue::internet()),
            "Remote console: Session — surround audio is not supported for "
            "this target.");
  EXPECT_EQ(surround(Failures::legacy_cloud_stream()),
            "LegacyCloudStream: Session — surround audio is not supported for "
            "this target.");
  EXPECT_EQ(surround(Failures::cloud_stream()),
            "CloudStream: Session — surround audio is not supported for this "
            "target.");
}

TEST(LinkFailureAudioRequest, NamesAnUnsupportedChannelCount) {
  const auto invalid = Refused::for_console(RegistrationTarget::ps5_1, 7);
  ASSERT_TRUE(invalid);
  EXPECT_EQ(
      Failures::at(play::Venue::home(),
                   play::LinkFailure{Stage::unknown,
                                     Error::invalid_configuration, *invalid})
          .sentence(),
      "Local console: Session — the requested audio channel count is not "
      "supported.");
}

}  // namespace
}  // namespace play_test
