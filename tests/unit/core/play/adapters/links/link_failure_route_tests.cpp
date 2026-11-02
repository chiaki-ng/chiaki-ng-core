#include "src/core/play/adapters/links/link_failure.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using Stage = remote_play::FailureStage;
using Error = remote_play::StreamError;
using Quit = remote_play::QuitReason;
using K = internet::remote_ctrl::ConnectError;

class LinkFailureRemoteCtrl : public ::testing::Test {
 protected:
  static auto failure_of(K kind) -> play::LinkFailure {
    return play::LinkFailure::refused(
        internet::remote_ctrl::ConnectFailure{kind, 0, {}});
  }
};

TEST_F(LinkFailureRemoteCtrl, MapsAnInvalidAttemptOrAddressToItsOwnError) {
  EXPECT_EQ(failure_of(K::invalid_attempt).result(), Error::invalid_state);
  EXPECT_EQ(failure_of(K::invalid_transport).result(),
            Error::invalid_configuration);
  EXPECT_EQ(failure_of(K::invalid_host).result(), Error::invalid_configuration);
  EXPECT_EQ(failure_of(K::invalid_port).result(), Error::invalid_configuration);
}

TEST_F(LinkFailureRemoteCtrl, MapsAFailedConnectionToACtrlFailure) {
  EXPECT_EQ(failure_of(K::prepare_failed).result(), Error::ctrl_failed);
  EXPECT_EQ(failure_of(K::startup_failed).result(), Error::ctrl_failed);
  EXPECT_EQ(failure_of(K::session_failed).result(), Error::ctrl_failed);
}

TEST_F(LinkFailureRemoteCtrl, MapsTrailingDataProblemsToTheirOwnErrors) {
  EXPECT_EQ(failure_of(K::trailing_too_large).result(), Error::ctrl_too_large);
  EXPECT_EQ(failure_of(K::trailing_malformed).result(),
            Error::ctrl_invalid_response);
  EXPECT_EQ(failure_of(K::trailing_crypto_failed).result(),
            Error::ctrl_invalid_response);
  EXPECT_EQ(failure_of(K::trailing_counter_exhausted).result(),
            Error::ctrl_failed);
  EXPECT_EQ(failure_of(K::trailing_lifecycle_failed).result(),
            Error::ctrl_failed);
}

TEST_F(LinkFailureRemoteCtrl, IsCancelledWhenStopped) {
  const auto stopped = failure_of(K::stopped);
  EXPECT_TRUE(stopped.cancelled());
  EXPECT_NE(stopped.result(), Error::ctrl_invalid_response);
}

class LinkFailureAcceptWait : public ::testing::Test {
 protected:
  static auto waited(internet::holepunch::AcceptPsnErrorKind kind)
      -> internet::holepunch::SelectedAcceptExchangeError {
    return internet::holepunch::SelectedAcceptExchangeError{
        internet::holepunch::AcceptPsnError{kind}};
  }
};

TEST_F(LinkFailureAcceptWait, IsCancelledWhenTheWaitWasCancelled) {
  const auto exchange =
      waited(internet::holepunch::AcceptPsnErrorKind::cancelled);
  const play::LinkFailure waiting{Stage::data_holepunch, Error::route_failed,
                                  play::RouteFailure{exchange}};
  EXPECT_TRUE(waiting.cancelled());
}

TEST_F(LinkFailureAcceptWait, IsNotCancelledWhenTheWaitTimedOut) {
  const auto exchange =
      waited(internet::holepunch::AcceptPsnErrorKind::timed_out);
  EXPECT_FALSE(play::RouteFailure{exchange}.cancelled());
}

TEST_F(LinkFailureAcceptWait, IsCancelledWhenTheProbeDrainWasInterrupted) {
  const internet::holepunch::SelectedAcceptExchangeError exchange{
      libchiaking::net::nat::holepunch::ProbeWaitError::cancelled};
  const play::LinkFailure draining{Stage::data_holepunch, Error::route_failed,
                                   play::RouteFailure{exchange}};
  EXPECT_TRUE(draining.cancelled());
  EXPECT_EQ(draining.describe(), "post-ACCEPT probe drain was interrupted");
}

TEST(LinkFailureRouteSwitch, DescribesAFailedDrain) {
  const play::LinkFailure drained{
      Stage::route_switch, Error::pipeline_failed,
      play::LinkFailure::SwitchDrain{libchiaking::net::transport::rudp::
                                         RemoteDatagramDrainError::receive}};
  EXPECT_EQ(drained.describe(),
            "draining stale datagrams from the switched route failed");
  EXPECT_EQ(drained.quit().reason, Quit::none);
}

}  // namespace
}  // namespace play_test
